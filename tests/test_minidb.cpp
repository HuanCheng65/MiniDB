// Integration tests for the embedded MiniDB API. No external test framework is
// required (design doc 2.2 keeps dependencies self-contained).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "minidb/minidb.h"

namespace {

#ifndef MINIDB_SOURCE_DIR
#define MINIDB_SOURCE_DIR "."
#endif

int g_failures = 0;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                               \
      ++g_failures;                                                           \
    }                                                                         \
  } while (0)

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool Exec(mydb_db* db, const std::string& sql) {
  char* error = nullptr;
  const int rc = mydb_exec(db, sql.c_str(), &error);
  if (rc != MYDB_OK) {
    std::fprintf(stderr, "SQL error: %s\nSQL: %s\n",
                 error != nullptr ? error : mydb_last_error(), sql.c_str());
    mydb_free(error);
  }
  return rc == MYDB_OK;
}

// Runs a query and returns all rows as formatted strings.
std::vector<std::vector<std::string>> Query(mydb_db* db,
                                            const std::string& sql) {
  std::vector<std::vector<std::string>> rows;
  mydb_stmt* stmt = nullptr;
  if (mydb_prepare(db, sql.c_str(), &stmt) != MYDB_OK) {
    std::fprintf(stderr, "prepare failed: %s\nSQL: %s\n",
                 mydb_errmsg(db) != nullptr ? mydb_errmsg(db) : "unknown",
                 sql.c_str());
    return rows;
  }
  const int columns = mydb_column_count(stmt);
  int step = 0;
  while ((step = mydb_step(stmt)) == MYDB_ROW) {
    std::vector<std::string> row;
    for (int i = 0; i < columns; ++i) {
      const char* text = mydb_column_text(stmt, i);
      row.emplace_back(text != nullptr ? text : "NULL");
    }
    rows.push_back(std::move(row));
  }
  mydb_finalize(stmt);
  return rows;
}

bool Contains(const std::vector<std::vector<std::string>>& rows,
              const std::string& value) {
  for (const auto& row : rows) {
    for (const auto& cell : row) {
      if (cell == value) return true;
    }
  }
  return false;
}

std::string BuildBulkInsert(int start, int count) {
  std::ostringstream sql;
  sql << "INSERT INTO bulk VALUES ";
  for (int i = 0; i < count; ++i) {
    if (i != 0) sql << ",";
    sql << "(" << (start + i) << ",'" << std::string(120, 'x') << "_"
        << (start + i) << "')";
  }
  sql << ";";
  return sql.str();
}

}  // namespace

int main() {
  const std::string db_path =
      "/tmp/minidb_test_" + std::to_string(static_cast<long long>(getpid())) +
      "_db";
  std::error_code ec;
  std::filesystem::remove_all(db_path, ec);

  mydb_config config;
  config.buffer_pool_frames = 8;  // force eviction with a tiny pool
  config.replacer = "clock";
  config.log_level = "error";
  config.log_file = nullptr;

  mydb_db* db = nullptr;
  if (mydb_open(db_path.c_str(), &config, &db) != MYDB_OK) {
    std::fprintf(stderr, "open failed: %s\n", mydb_last_error());
    return 1;
  }

  // 1. Full TPC-H DDL via the embedded API.
  const std::string schema =
      ReadFile(MINIDB_SOURCE_DIR "/test/tpc-h/schema.sql");
  CHECK(!schema.empty());
  CHECK(Exec(db, schema));

  auto tables = Query(db, "SHOW TABLES;");
  CHECK(tables.size() == 10);  // 8 TPC-H relations + 2 system relations
  CHECK(Contains(tables, "lineitem"));
  CHECK(Contains(tables, "partsupp"));
  CHECK(Contains(tables, "sys_tables"));
  CHECK(Contains(tables, "sys_columns"));

  // Every relation is a separate data + FSM file in the database folder.
  CHECK(std::filesystem::exists(db_path + "/sys_tables.dat"));
  CHECK(std::filesystem::exists(db_path + "/sys_tables.fsm"));
  CHECK(std::filesystem::exists(db_path + "/sys_columns.dat"));
  CHECK(std::filesystem::exists(db_path + "/sys_columns.fsm"));
  CHECK(std::filesystem::exists(db_path + "/lineitem.dat"));
  CHECK(std::filesystem::exists(db_path + "/lineitem.fsm"));
  CHECK(std::filesystem::is_directory(db_path));

  auto system_tables = Query(db, "SELECT name, data_file FROM sys_tables;");
  CHECK(system_tables.size() == 10);
  CHECK(Contains(system_tables, "lineitem.dat"));
  CHECK(Contains(system_tables, "sys_tables.dat"));

  auto system_columns = Query(db, "SELECT name FROM sys_columns;");
  CHECK(!system_columns.empty());

  // System relations are read-only.
  CHECK(Exec(db, "INSERT INTO sys_tables VALUES (99, 'x', 'x', 'x', 0, 1, 0);") ==
        false);
  CHECK(Exec(db, "DROP TABLE sys_tables;") == false);
  CHECK(Exec(db, "DELETE FROM sys_tables;") == false);
  CHECK(Exec(db, "CREATE TABLE sys_forbidden (id INTEGER);") == false);

  auto columns = Query(db, "DESCRIBE lineitem;");
  CHECK(columns.size() == 16);

  auto decimal_column = Query(
      db, "SELECT l_quantity FROM lineitem WHERE l_orderkey = 0;");
  CHECK(decimal_column.empty());  // empty table, but the query must plan

  // 2. DML with several types, WHERE filtering, DECIMAL and DATE round trips.
  CHECK(Exec(db,
             "INSERT INTO nation VALUES "
             "(0, 'ALGERIA', 0, 'north africa'),"
             "(1, 'ARGENTINA', 1, 'south america'),"
             "(2, 'BRAZIL', 1, 'south america'),"
             "(3, 'CANADA', 1, 'north america');"));
  CHECK(Exec(db,
             "INSERT INTO supplier VALUES "
             "(7, 'Supplier#000000007', '7 rue de Paris', 0, "
             "'10-100-100-1000', 5755.94, 'test supplier');"));
  CHECK(Exec(db,
             "INSERT INTO orders VALUES "
             "(1, 7, 'O', 173665.47, '1996-01-02', '5-LOW', "
             "'Clerk#000000951', 0, 'test order');"));

  auto filtered = Query(db, "SELECT n_name FROM nation WHERE n_regionkey = 1;");
  CHECK(filtered.size() == 3);
  CHECK(Contains(filtered, "ARGENTINA"));

  auto supplier = Query(db, "SELECT s_acctbal FROM supplier WHERE s_suppkey = 7;");
  CHECK(supplier.size() == 1);
  if (!supplier.empty()) CHECK(supplier[0][0] == "5755.94");

  auto order = Query(db, "SELECT o_orderdate, o_totalprice FROM orders;");
  CHECK(order.size() == 1);
  if (!order.empty()) {
    CHECK(order[0][0] == "1996-01-02");
    CHECK(order[0][1] == "173665.47");
  }

  auto not_null = Query(db, "SELECT n_comment FROM nation WHERE n_nationkey = 3;");
  CHECK(not_null.size() == 1);
  if (!not_null.empty()) CHECK(not_null[0][0] == "north america");

  // 3. DELETE with and without a matching WHERE clause.
  CHECK(Exec(db, "DELETE FROM nation WHERE n_nationkey = 2;"));
  auto remaining_nations = Query(db, "SELECT n_name FROM nation;");
  CHECK(remaining_nations.size() == 3);
  CHECK(!Contains(remaining_nations, "BRAZIL"));
  CHECK(Exec(db, "DELETE FROM nation WHERE n_nationkey = 99;"));
  CHECK(Query(db, "SELECT n_name FROM nation;").size() == 3);

  // 4. Multi-page table: 20,000 rows, about six 512 KB extents.
  CHECK(Exec(db,
             "CREATE TABLE bulk (id INTEGER NOT NULL, "
             "payload VARCHAR(200) NOT NULL);"));
  const int kTotalRows = 20000;
  const int kBatch = 100;
  for (int start = 0; start < kTotalRows; start += kBatch) {
    CHECK(Exec(db, BuildBulkInsert(start, kBatch)));
  }
  auto bulk = Query(db, "SELECT * FROM bulk;");
  CHECK(bulk.size() == static_cast<size_t>(kTotalRows));
  auto one = Query(db, "SELECT payload FROM bulk WHERE id = 19999;");
  CHECK(one.size() == 1);
  if (!one.empty()) {
    CHECK(one[0][0] == std::string(120, 'x') + "_19999");
  }

  // 5. Close and reopen: schema, tuples and extent/FSM state must survive.
  CHECK(mydb_close(db) == MYDB_OK);
  db = nullptr;
  CHECK(std::filesystem::exists(db_path));

  if (mydb_open(db_path.c_str(), &config, &db) != MYDB_OK) {
    std::fprintf(stderr, "reopen failed: %s\n", mydb_last_error());
    return 1;
  }
  tables = Query(db, "SHOW TABLES;");
  CHECK(tables.size() == 11);  // 9 user + 2 system relations
  CHECK(std::filesystem::exists(db_path + "/bulk.dat"));
  CHECK(std::filesystem::exists(db_path + "/bulk.fsm"));
  bulk = Query(db, "SELECT * FROM bulk;");
  CHECK(bulk.size() == static_cast<size_t>(kTotalRows));
  supplier = Query(db, "SELECT s_acctbal FROM supplier;");
  CHECK(supplier.size() == 1 && supplier[0][0] == "5755.94");
  auto nations_after_reopen = Query(db, "SELECT n_name FROM nation;");
  CHECK(nations_after_reopen.size() == 3);
  CHECK(!Contains(nations_after_reopen, "BRAZIL"));

  // 6. DELETE without WHERE removes every row, then DROP removes the files.
  CHECK(Exec(db, "DELETE FROM bulk;"));
  CHECK(Query(db, "SELECT * FROM bulk;").empty());
  CHECK(Exec(db, "DROP TABLE bulk;"));
  tables = Query(db, "SHOW TABLES;");
  CHECK(tables.size() == 10);
  CHECK(!Contains(tables, "bulk"));
  CHECK(!std::filesystem::exists(db_path + "/bulk.dat"));
  CHECK(!std::filesystem::exists(db_path + "/bulk.fsm"));
  CHECK(Exec(db, "DROP TABLE IF EXISTS missing;"));
  char* error = nullptr;
  CHECK(mydb_exec(db, "DROP TABLE missing;", &error) == MYDB_ERROR);
  mydb_free(error);

  CHECK(mydb_close(db) == MYDB_OK);
  std::filesystem::remove_all(db_path, ec);

  if (g_failures == 0) {
    std::printf("all MiniDB tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d test check(s) failed\n", g_failures);
  return 1;
}
