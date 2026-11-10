// Manual acceptance run against real dbgen supplier.tbl, in a NEW database.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/database.h"

using namespace minidb;
namespace {
void Check(bool ok, const std::string& what) {
  if (!ok) throw std::runtime_error(what);
}
QueryResult Exec(Database& db, const std::string& sql) {
  QueryResult r;
  const auto s = db.Execute(sql, &r);
  Check(s.ok(), s.message());
  return r;
}
std::string Quote(const std::string& text) {
  std::string out = "'";
  for (char c : text) {
    out += c;
    if (c == '\'') out += c;
  }
  return out + "'";
}
std::vector<std::string> Rows(const QueryResult& r) {
  std::vector<std::string> rows;
  for (const auto& row : r.rows) {
    std::string s;
    for (const auto& v : row) s += ValueToString(v) + '|';
    rows.push_back(s);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    Check(argc == 3,
          "usage: minidb_verify_index_tpch supplier.tbl NEW_DATABASE_FOLDER");
    const std::string data = std::filesystem::absolute(argv[1]).string();
    const std::string directory = std::filesystem::absolute(argv[2]).string();
    Check(!std::filesystem::exists(directory),
          "database folder already exists; choose a new path");
    std::ifstream input(data);
    Check(input.good(), "cannot open supplier.tbl");
    std::vector<std::pair<int64_t, std::string>> suppliers;
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) continue;
      auto a = line.find('|'), b = line.find('|', a + 1);
      Check(a != std::string::npos && b != std::string::npos,
            "invalid supplier row");
      suppliers.emplace_back(std::stoll(line.substr(0, a)),
                             line.substr(a + 1, b - a - 1));
    }
    Check(!suppliers.empty(), "supplier.tbl is empty");
    Config config;
    config.path = directory;
    config.buffer_pool_frames = 16;
    config.log_level = "off";
    auto opened = Database::Open(config);
    Check(opened.ok(), opened.status().message());
    auto db = opened.TakeValue();
    Exec(*db,
         "CREATE TABLE supplier (s_suppkey INTEGER NOT NULL, s_name CHAR(25) "
         "NOT NULL, "
         "s_address VARCHAR(40) NOT NULL, s_nationkey INTEGER NOT NULL, "
         "s_phone CHAR(15) NOT NULL, "
         "s_acctbal DECIMAL(15,2) NOT NULL, s_comment VARCHAR(101) NOT NULL)");
    auto loaded = Exec(*db, "COPY supplier FROM " + Quote(data));
    Check(loaded.changes == static_cast<int>(suppliers.size()),
          "COPY row count mismatch");
    const auto started = std::chrono::steady_clock::now();
    Exec(*db,
         "CREATE INDEX supplier_key ON supplier(s_suppkey); CREATE INDEX "
         "supplier_name ON supplier(s_name)");
    const double build_seconds = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
    std::cout << "supplier_rows=" << suppliers.size()
              << " buffer_frames=16 build_seconds=" << build_seconds << '\n';
    for (const auto* name : {"supplier_key", "supplier_name"}) {
      const auto stats =
          Exec(*db, std::string("PRAGMA index_stats(") + name + ')');
      std::cout << name;
      for (size_t i = 0; i < stats.columns.size(); ++i)
        std::cout << ' ' << stats.columns[i].name << '='
                  << ValueToString(stats.rows[0][i]);
      std::cout << '\n';
      for (const auto* format : {"tree", "dot"}) {
        auto drawing =
            Exec(*db, std::string("PRAGMA index_") + format + '(' + name + ')');
        std::ofstream output(directory + '/' + name + '.' + format);
        output << drawing.rows[0][0].string_value;
        Check(output.good(), "cannot write index drawing");
      }
    }
    Check(db->Close().ok(), "close failed");
    db.reset();
    opened = Database::Open(config);
    Check(opened.ok(), opened.status().message());
    db = opened.TakeValue();
    for (const auto& [key, name] : suppliers) {
      const auto by_key = Exec(
          *db, "SELECT s_suppkey, s_name FROM supplier WHERE s_suppkey = " +
                   std::to_string(key));
      const auto by_name =
          Exec(*db, "SELECT s_suppkey, s_name FROM supplier WHERE s_name = " +
                        Quote(name));
      Check(by_key.rows.size() == 1 && by_key.rows[0][0].int32_value == key &&
                by_key.rows[0][1].string_value == name,
            "integer lookup mismatch");
      Check(Rows(by_key) == Rows(by_name), "string lookup mismatch");
      Check(by_key.message == "INDEX SCAN supplier_key" &&
                by_name.message == "INDEX SCAN supplier_name",
            "missing index scan");
    }
    const auto before = Exec(
        *db, "SELECT s_suppkey, s_name FROM supplier WHERE s_suppkey >= 5000");
    Exec(*db, "DROP INDEX supplier_key");
    Check(Rows(before) == Rows(Exec(*db,
                                    "SELECT s_suppkey, s_name FROM supplier "
                                    "WHERE s_suppkey >= 5000")),
          "range differs from full scan");
    Exec(*db, "CREATE INDEX supplier_key ON supplier(s_suppkey)");
    const auto key = suppliers.front().first;
    Exec(*db,
         "UPDATE supplier SET s_suppkey = -1, s_name = 'IndexUpdated' WHERE "
         "s_suppkey = " +
             std::to_string(key));
    Check(Exec(*db, "SELECT * FROM supplier WHERE s_suppkey = " +
                        std::to_string(key))
              .rows.empty(),
          "old key survived update");
    Check(Exec(*db, "SELECT * FROM supplier WHERE s_name = 'IndexUpdated'")
                  .rows.size() == 1,
          "new name missing");
    Exec(*db, "DELETE FROM supplier WHERE s_suppkey = -1");
    Check(Exec(*db, "SELECT * FROM supplier WHERE s_name = 'IndexUpdated'")
              .rows.empty(),
          "deleted name survived");
    Exec(*db,
         "INSERT INTO supplier VALUES (-2, 'IndexInserted', 'address', 0, "
         "'00-000-000-0000', 12.34, 'index test')");
    Check(
        Exec(*db, "SELECT * FROM supplier WHERE s_suppkey = -2").rows.size() ==
            1,
        "insert missing");
    Exec(*db,
         "PRAGMA index_check(supplier_key); PRAGMA index_check(supplier_name)");
    std::cout << "reopen_point_lookups=" << suppliers.size() * 2
              << " range_vs_heap=OK insert_update_delete=OK\n";
    std::cout << "buffer_miss=" << db->buffer_pool().Stats().miss
              << " evict=" << db->buffer_pool().Stats().evict
              << " flush=" << db->buffer_pool().Stats().flush << '\n';
    Check(db->Close().ok(), "close failed");
    db.reset();
    opened = Database::Open(config);
    Check(opened.ok(), opened.status().message());
    db = opened.TakeValue();
    Check(Exec(*db, "SELECT * FROM supplier WHERE s_name = 'IndexInserted'")
                  .rows.size() == 1,
          "DML persistence failed");
    Exec(*db,
         "PRAGMA index_check(supplier_key); PRAGMA index_check(supplier_name)");
    Check(db->Close().ok(), "close failed");
    std::cout << "final_reopen=OK artifacts=" << directory << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
