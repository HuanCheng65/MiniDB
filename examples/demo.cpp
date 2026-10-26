// Embedded MiniDB demo: opens a folder-based database through the C API,
// creates the full TPC-H schema, inspects the system catalog, inserts sample
// rows, runs prepared queries, deletes rows, then reopens the database to show
// persistence.
//
// Build:  cmake --build build -j
// Run:    ./build/minidb_demo [db-folder] [schema.sql]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "minidb/minidb.h"

namespace {

#ifndef MINIDB_SOURCE_DIR
#define MINIDB_SOURCE_DIR "."
#endif

void Fail(const std::string& what, mydb_db* db) {
  const char* message = db != nullptr ? mydb_errmsg(db) : nullptr;
  if (message == nullptr) message = mydb_last_error();
  std::fprintf(stderr, "%s failed: %s\n", what.c_str(),
               message != nullptr ? message : "unknown error");
  std::exit(1);
}

void ExecOrDie(mydb_db* db, const std::string& sql) {
  char* error = nullptr;
  if (mydb_exec(db, sql.c_str(), &error) != MYDB_OK) {
    std::fprintf(stderr, "SQL failed:\n%s\nerror: %s\n", sql.c_str(),
                 error != nullptr ? error : "unknown");
    mydb_free(error);
    std::exit(1);
  }
  const char* message = mydb_message(db);
  if (message != nullptr) std::printf("  %s\n", message);
}

void PrintQuery(mydb_db* db, const std::string& sql) {
  std::printf("\nSQL> %s\n", sql.c_str());
  mydb_stmt* stmt = nullptr;
  if (mydb_prepare(db, sql.c_str(), &stmt) != MYDB_OK) {
    Fail("prepare", db);
  }
  const int columns = mydb_column_count(stmt);
  std::vector<size_t> widths(static_cast<size_t>(columns), 0);
  std::vector<std::string> headers(static_cast<size_t>(columns));
  for (int i = 0; i < columns; ++i) {
    const char* name = mydb_column_name(stmt, i);
    headers[static_cast<size_t>(i)] = name != nullptr ? name : "";
    widths[static_cast<size_t>(i)] = headers[static_cast<size_t>(i)].size();
  }

  std::vector<std::vector<std::string>> rows;
  int step = 0;
  while ((step = mydb_step(stmt)) == MYDB_ROW) {
    std::vector<std::string> row;
    for (int i = 0; i < columns; ++i) {
      const char* text = mydb_column_text(stmt, i);
      row.emplace_back(text != nullptr ? text : "NULL");
      widths[static_cast<size_t>(i)] =
          std::max(widths[static_cast<size_t>(i)],
                   row.back().size());
    }
    rows.push_back(std::move(row));
  }
  mydb_finalize(stmt);
  if (step == MYDB_ERROR) Fail("step", db);
  if (columns == 0) return;

  auto print_row = [&](const std::vector<std::string>& row) {
    for (int i = 0; i < columns; ++i) {
      const std::string& cell = row[static_cast<size_t>(i)];
      std::printf("%s%s", i == 0 ? "" : " | ", cell.c_str());
      if (i + 1 < columns) {
        std::printf("%s", std::string(widths[static_cast<size_t>(i)] -
                                      cell.size(), ' ')
                              .c_str());
      }
    }
    std::printf("\n");
  };
  print_row(headers);
  for (int i = 0; i < columns; ++i) {
    std::printf("%s%s", i == 0 ? "" : "-+-",
                std::string(widths[static_cast<size_t>(i)], '-').c_str());
  }
  std::printf("\n");
  for (const auto& row : rows) print_row(row);
  std::printf("(%zu rows)\n", rows.size());
}

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    std::exit(1);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string db_path = argc > 1 ? argv[1] : "minidb_demo_db";
  const std::string schema_path =
      argc > 2 ? argv[2] : MINIDB_SOURCE_DIR "/test/tpc-h/schema.sql";

  std::printf("MiniDB embedded demo (%s)\n", mydb_version());
  std::printf("database folder: %s\nschema:          %s\n", db_path.c_str(),
              schema_path.c_str());

  std::error_code ec;
  if (std::filesystem::exists(db_path, ec)) {
    std::printf("removing previous demo database %s\n", db_path.c_str());
    std::filesystem::remove_all(db_path, ec);
  }

  mydb_config config;
  config.buffer_pool_frames = 16;  // small on purpose: watch evictions happen
  config.replacer = "clock";
  config.log_level = "warn";
  config.log_file = nullptr;

  mydb_db* db = nullptr;
  if (mydb_open(db_path.c_str(), &config, &db) != MYDB_OK) {
    Fail("mydb_open", nullptr);
  }

  std::printf("\n== 1. Execute the TPC-H DDL script ==\n");
  const std::string schema = ReadFile(schema_path);
  ExecOrDie(db, schema);
  PrintQuery(db, "SHOW TABLES;");
  PrintQuery(db, "SELECT name, data_file, fsm_file FROM sys_tables;");
  PrintQuery(db, "DESCRIBE lineitem;");

  std::printf("\n== 2. Insert sample tuples ==\n");
  ExecOrDie(db,
            "INSERT INTO nation VALUES "
            "(0, 'ALGERIA', 0, 'north africa'),"
            "(1, 'ARGENTINA', 1, 'south america'),"
            "(2, 'BRAZIL', 1, 'south america'),"
            "(3, 'CANADA', 1, 'north america'),"
            "(4, 'EGYPT', 4, 'middle east');");
  ExecOrDie(db,
            "INSERT INTO region VALUES "
            "(0, 'AFRICA', 'continent'),"
            "(1, 'AMERICA', 'continent');");
  ExecOrDie(db,
            "INSERT INTO supplier VALUES "
            "(1, 'Supplier#000000001', '1 rue de Paris', 0, "
            "'10-100-100-1000', 5755.94, 'reliable supplier'),"
            "(2, 'Supplier#000000002', '2 rue de Lyon', 1, "
            "'10-200-200-2000', 1234.56, 'fast delivery');");
  ExecOrDie(db,
            "INSERT INTO orders VALUES "
            "(1, 1, 'O', 173665.47, '1996-01-02', '5-LOW', "
            "'Clerk#000000951', 0, 'sample order');");

  std::printf("\n== 3. Prepared queries ==\n");
  PrintQuery(db, "SELECT * FROM nation;");
  PrintQuery(db,
             "SELECT n_nationkey, n_name FROM nation WHERE n_regionkey = 1;");
  PrintQuery(db,
             "SELECT s_suppkey, s_name, s_acctbal FROM supplier "
             "WHERE s_nationkey = 0;");
  PrintQuery(db,
             "SELECT o_orderkey, o_totalprice, o_orderdate FROM orders;");

  std::printf("\n== 4. DELETE with and without WHERE ==\n");
  ExecOrDie(db, "DELETE FROM nation WHERE n_nationkey = 0;");
  PrintQuery(db, "SELECT n_nationkey, n_name FROM nation;");
  ExecOrDie(db, "DELETE FROM region;");
  PrintQuery(db, "SELECT * FROM region;");

  std::printf("\n== 5. Persistence: close and reopen ==\n");
  if (mydb_close(db) != MYDB_OK) Fail("mydb_close", db);
  db = nullptr;

  if (mydb_open(db_path.c_str(), &config, &db) != MYDB_OK) {
    Fail("mydb_open (reopen)", nullptr);
  }
  PrintQuery(db, "SELECT * FROM supplier;");
  PrintQuery(db, "SHOW TABLES;");
  if (mydb_close(db) != MYDB_OK) Fail("mydb_close", db);

  uintmax_t total_bytes = 0;
  size_t file_count = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(db_path, ec)) {
    if (!entry.is_regular_file()) continue;
    total_bytes += entry.file_size();
    ++file_count;
  }
  std::printf("\nDemo finished. Database folder: %s (%zu files, %llu bytes)\n",
              db_path.c_str(), file_count,
              static_cast<unsigned long long>(total_bytes));
  return 0;
}
