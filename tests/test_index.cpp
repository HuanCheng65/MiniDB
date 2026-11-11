#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/logger.h"
#include "db/database.h"
#include "index/bplus_tree.h"

using namespace minidb;
namespace {
#define REQUIRE(x)                                                    \
  do {                                                                \
    if (!(x)) {                                                       \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << #x << '\n'; \
      std::exit(1);                                                   \
    }                                                                 \
  } while (false)
void Ok(const Status& s) {
  if (!s.ok()) {
    std::cerr << s.message() << '\n';
    std::exit(1);
  }
}
template <class T>
T Take(Result<T> result) {
  Ok(result.status());
  REQUIRE(result.ok());
  return result.TakeValue();
}
std::string Temp(const std::string& suffix) {
  return "/tmp/minidb_index_" + std::to_string(getpid()) + "_" + suffix;
}
Value Key(int i, bool strings) {
  return strings ? Value::String("key_" + std::to_string(i % 157))
                 : Value::Int64(i % 157 - 78);
}
RID Rid(int i) {
  return {static_cast<page_id_t>(100 + i / 30), static_cast<slot_id_t>(i % 30)};
}
// Deliberately independent of the engine's Satisfies().
bool Matches(int cmp, CompareOp op) {
  return op == CompareOp::kEq   ? cmp == 0
         : op == CompareOp::kLt ? cmp < 0
         : op == CompareOp::kLe ? cmp <= 0
         : op == CompareOp::kGt ? cmp > 0
         : op == CompareOp::kGe ? cmp >= 0
                                : cmp != 0;
}
uint64_t Packed(RID r) {
  return (static_cast<uint64_t>(r.page_id) << 16) | r.slot;
}
void CheckModel(BPlusTree& tree, const std::set<int>& active, bool strings,
                int probe) {
  auto stats = Take(tree.Validate());
  REQUIRE(stats.entries == active.size());
  for (const CompareOp op : {CompareOp::kEq, CompareOp::kLt, CompareOp::kLe,
                             CompareOp::kGt, CompareOp::kGe, CompareOp::kNe}) {
    std::set<uint64_t> expected, actual;
    for (int i : active)
      if (Matches(CompareValues(Key(i, strings), Key(probe, strings)), op))
        expected.insert(Packed(Rid(i)));
    Ok(tree.Scan(op, Key(probe, strings), [&](const IndexEntry& e) {
      REQUIRE(actual.insert(Packed(e.rid)).second);
      return true;
    }));
    REQUIRE(actual == expected);
  }
}
void TreeRandom(bool strings) {
  const auto dir = Temp(strings ? "strings" : "integers");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir + "/tree.idx";
  std::set<int> active;
  std::vector<int> order(strings ? 5000 : 10000);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937 rng(20261110);
  std::shuffle(order.begin(), order.end(), rng);
  uint32_t pages = 0;
  {
    LogManager log;
    BufferPool pool(log, BufferConfig{4, "clock"});
    IndexMetadata m{"test",
                    "t",
                    3,
                    0,
                    strings ? TypeId::kVarchar : TypeId::kInt64,
                    static_cast<uint16_t>(strings ? 250 : 0)};
    auto tree = Take(BPlusTree::Create(pool, path, m));
    REQUIRE(Take(tree->Validate()).height == 1);
    Ok(tree->Insert(Value::Null(), Rid(0)));
    REQUIRE(tree->size() == 0);
    for (int i : order) {
      Ok(tree->Insert(Key(i, strings), Rid(i)));
      active.insert(i);
    }
    REQUIRE(!tree->Insert(Key(order[0], strings), Rid(order[0])).ok());
    CheckModel(*tree, active, strings, 55);
    const auto stats = Take(tree->Validate());
    REQUIRE(stats.height >= (strings ? 3u : 2u));
    REQUIRE(pool.Stats().evict > 0);
    REQUIRE(pool.Stats().flush > 0);
    REQUIRE(!Take(tree->Draw()).empty());
    REQUIRE(Take(tree->Dot()).find("digraph") != std::string::npos);
    // Mixed deletes/inserts exercise redistributed and merged nodes.
    for (size_t i = 0; i < order.size() * 3 / 4; ++i) {
      Ok(tree->Erase(Key(order[i], strings), Rid(order[i])));
      active.erase(order[i]);
      if (i % 997 == 0) CheckModel(*tree, active, strings, static_cast<int>(i));
    }
    CheckModel(*tree, active, strings, 91);
    // Reinsert among surviving subtrees, whose separator fences may retain
    // deleted keys. This catches routing errors hidden by a full-tree refill.
    for (int step = 0; step < 1000; ++step) {
      const int i = static_cast<int>(rng() % order.size());
      if (active.erase(i))
        Ok(tree->Erase(Key(i, strings), Rid(i)));
      else {
        Ok(tree->Insert(Key(i, strings), Rid(i)));
        active.insert(i);
      }
      if (step % 200 == 0) CheckModel(*tree, active, strings, step);
    }
    pages = tree->disk()->PageCount();
    Ok(pool.FlushAll());
  }
  {
    LogManager log;
    BufferPool pool(log, BufferConfig{3, "clock"});
    auto tree = Take(BPlusTree::Open(pool, path));
    CheckModel(*tree, active, strings, 91);
    for (int i : active) Ok(tree->Erase(Key(i, strings), Rid(i)));
    active.clear();
    auto empty = Take(tree->Validate());
    REQUIRE(empty.entries == 0 && empty.height == 1 && empty.nodes == 1 &&
            empty.free_pages > 0);
    REQUIRE(!tree->Erase(Key(0, strings), Rid(0)).ok());
    // Refill in the same order: allocation must use the persistent free list.
    for (int i : order) {
      Ok(tree->Insert(Key(i, strings), Rid(i)));
      active.insert(i);
    }
    REQUIRE(tree->disk()->PageCount() <= pages);
    CheckModel(*tree, active, strings, 12);
    std::shuffle(order.begin(), order.end(), rng);
    for (int i : order) Ok(tree->Erase(Key(i, strings), Rid(i)));
    REQUIRE(Take(tree->Validate()).height == 1);
    Ok(pool.FlushAll());
  }
  {
    LogManager log;
    BufferPool pool(log, BufferConfig{2, "clock"});
    auto tree = Take(BPlusTree::Open(pool, path));
    REQUIRE(Take(tree->Validate()).entries == 0);
  }
  std::filesystem::remove_all(dir);
}
// Bulk-loaded trees must satisfy every invariant, answer scans like the model
// and keep working under later inserts and erases, at any size and fanout.
void BulkLoad() {
  auto dir = Temp("bulk");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  LogManager log;
  BufferPool pool(log, BufferConfig{4, "clock"});
  int file = 0;
  // Length 4000 gives capacity 2; length 250 capacity 30; integers 450.
  // A 2 KB sort budget spills hundreds of runs; the default sorts in memory.
  for (const size_t memory : {BPlusTree::kDefaultSortMemory, size_t{2048}})
    for (const int length : {4000, 250, 0}) {
      const bool strings = length != 0;
      const IndexMetadata m{"b",
                            "t",
                            3,
                            0,
                            strings ? TypeId::kVarchar : TypeId::kInt64,
                            static_cast<uint16_t>(length)};
      const auto capacity = [&] {
        auto probe = Take(BPlusTree::Create(
            pool, dir + "/probe" + std::to_string(file++) + ".idx", m));
        return static_cast<int>(Take(probe->Validate()).capacity);
      }();
      const int fill = capacity - capacity / 8;
      for (const int count :
           {0, 1, capacity / 2, fill, fill + 1, capacity, capacity + 1,
            2 * fill + 1, fill * (fill + 1) + 1, strings ? 700 : 30000}) {
        if (count > 30000) continue;
        auto tree = Take(BPlusTree::Create(
            pool, dir + "/" + std::to_string(file++) + ".idx", m));
        std::vector<IndexEntry> entries;
        std::set<int> active;
        for (int i = count - 1; i >= 0; --i) {
          entries.push_back({Key(i, strings), Rid(i)});
          active.insert(i);
        }
        entries.push_back({Value::Null(), Rid(count)});
        Ok(tree->BulkLoad(std::move(entries), memory));
        CheckModel(*tree, active, strings, count / 2);
        // Only an empty tree can be bulk loaded.
        if (count > 0)
          REQUIRE(!tree->BulkLoad({{Key(0, strings), Rid(0)}}).ok());
        for (int i = count; i < count + 40; ++i) {
          Ok(tree->Insert(Key(i, strings), Rid(i)));
          active.insert(i);
        }
        for (int i = 0; i < count; i += 2) {
          Ok(tree->Erase(Key(i, strings), Rid(i)));
          active.erase(i);
        }
        CheckModel(*tree, active, strings, count / 3);
      }
    }
  auto tree = Take(BPlusTree::Create(pool, dir + "/dup.idx",
                                     {"d", "t", 3, 0, TypeId::kInt64, 0}));
  REQUIRE(
      !tree->BulkLoad({{Key(1, false), Rid(1)}, {Key(1, false), Rid(1)}}).ok());
  // The same entry twice is caught even when the copies are in different
  // runs (a 16-byte budget has room for one entry at a time).
  tree = Take(BPlusTree::Create(pool, dir + "/dup2.idx",
                                {"d", "t", 3, 0, TypeId::kInt64, 0}));
  REQUIRE(!tree->BulkLoad({{Key(1, false), Rid(1)},
                           {Key(2, false), Rid(2)},
                           {Key(1, false), Rid(1)}},
                          16)
               .ok());
  {
    BPlusTree::BulkLoader loader(*tree, 16);
    for (int i = 0; i < 5; ++i) Ok(loader.Add(Key(i, false), Rid(i)));
    REQUIRE(loader.runs() == 4);  // the fifth is still in memory
    REQUIRE(std::filesystem::exists(dir + "/dup2.idx.sort"));
  }
  REQUIRE(!std::filesystem::exists(dir + "/dup2.idx.sort"));
  // More runs than the budget has read buffers for are merged in passes:
  // five runs, two at a time, take two passes before the final merge.
  tree = Take(BPlusTree::Create(pool, dir + "/passes.idx",
                                {"d", "t", 3, 0, TypeId::kInt64, 0}));
  {
    BPlusTree::BulkLoader loader(*tree, 16);
    for (int i = 4; i >= 0; --i) Ok(loader.Add(Key(i, false), Rid(i)));
    Ok(loader.Finish());
    REQUIRE(loader.runs() == 5 && loader.merge_passes() == 2);
  }
  CheckModel(*tree, {0, 1, 2, 3, 4}, false, 2);
  std::filesystem::remove_all(dir);
}

void TinyFanoutAndBoundaries() {
  auto dir = Temp("tiny");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  LogManager log;
  BufferPool pool(log, BufferConfig{2, "clock"});
  auto tree = Take(BPlusTree::Create(pool, dir + "/s.idx",
                                     {"s", "t", 3, 0, TypeId::kVarchar, 4000}));
  for (int i = 0; i < 300; ++i) Ok(tree->Insert(Key(i, true), Rid(i)));
  REQUIRE(Take(tree->Validate()).height >= 5);
  for (int i = 0; i < 300; i += 2) Ok(tree->Erase(Key(i, true), Rid(i)));
  Take(tree->Validate());
  for (int i = 1; i < 300; i += 2) Ok(tree->Erase(Key(i, true), Rid(i)));
  REQUIRE(Take(tree->Validate()).height == 1);
  const std::string binary("a\0b", 3);
  Ok(tree->Insert(Value::String(binary), Rid(0)));
  REQUIRE(Take(tree->Find(Value::String(binary))).size() == 1);
  REQUIRE(!tree->Insert(Value::String(std::string(4001, 'x')), Rid(1)).ok());
  REQUIRE(!tree->Insert(Value::Int64(1), Rid(1)).ok());
  auto integers = Take(BPlusTree::Create(pool, dir + "/i.idx",
                                         {"i", "t", 3, 0, TypeId::kInt64, 0}));
  const std::vector<int64_t> limits = {std::numeric_limits<int64_t>::min(),
                                       -9007199254740993LL,
                                       -9007199254740992LL,
                                       9007199254740992LL,
                                       9007199254740993LL,
                                       std::numeric_limits<int64_t>::max() - 1,
                                       std::numeric_limits<int64_t>::max()};
  for (size_t i = 0; i < limits.size(); ++i)
    Ok(integers->Insert(Value::Int64(limits[i]), Rid(i)));
  for (size_t i = 0; i < limits.size(); ++i) {
    auto found = Take(integers->Find(Value::Int64(limits[i])));
    if (found.size() != 1 || !(found[0] == Rid(i)))
      std::cerr << "BIGINT lookup key=" << limits[i]
                << " matches=" << found.size() << '\n';
    REQUIRE(found.size() == 1 && found[0] == Rid(i));
  }
  Take(integers->Validate());
  Ok(pool.FlushAll());
  std::filesystem::remove_all(dir);
}
QueryResult Exec(Database& db, const std::string& sql) {
  QueryResult r;
  auto s = db.Execute(sql, &r);
  if (!s.ok()) std::cerr << "SQL: " << sql.substr(0, 200) << '\n';
  Ok(s);
  return r;
}
std::vector<std::string> Rows(const QueryResult& r) {
  std::vector<std::string> rows;
  for (const auto& row : r.rows) {
    std::string s;
    for (const auto& v : row)
      s += std::to_string(static_cast<int>(v.kind)) + ":" + ValueToString(v) +
           "|";
    rows.push_back(s);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}
void SqlIntegration() {
  const auto dir = Temp("sql");
  std::filesystem::remove_all(dir);
  Config config;
  config.path = dir;
  config.buffer_pool_frames = 8;
  config.log_level = "off";
  auto db = Take(Database::Open(config));
  Exec(*db, "CREATE TABLE t (id BIGINT, name CHAR(25), note VARCHAR(4000));");
  std::string insert = "INSERT INTO t VALUES ";
  for (int i = 0; i < 1200; ++i) {
    if (i) insert += ',';
    insert += '(' + std::to_string(i % 93) + ",'name" + std::to_string(i % 31) +
              "   ','x')";
  }
  Exec(*db, insert);
  Exec(*db,
       "INSERT INTO t VALUES (NULL, NULL, 'null'), (9007199254740992, 'big', "
       "'x'), (9007199254740993, 'big', 'x');");
  std::vector<std::string> queries = {
      "SELECT rid, * FROM t WHERE id = 17",
      "SELECT rid, * FROM t WHERE id >= 90",
      "SELECT rid, * FROM t WHERE id < 4",
      "SELECT rid, * FROM t WHERE id <= 7",
      "SELECT rid, * FROM t WHERE id > 90",
      "SELECT rid, * FROM t WHERE id != 50",
      "SELECT rid, * FROM t WHERE name = 'name3'",
      "SELECT rid, * FROM t WHERE name >= 'name9'",
      "SELECT rid, * FROM t WHERE name < 'name11'",
      "SELECT rid, * FROM t WHERE name = 'name3   '",
      "SELECT rid, * FROM t WHERE name < 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'",
      "SELECT rid, * FROM t WHERE id = 9007199254740993"};
  std::vector<std::vector<std::string>> expected;
  for (const auto& sql : queries) expected.push_back(Rows(Exec(*db, sql)));
  Exec(*db, "CREATE INDEX id_idx ON t(id); CREATE INDEX name_idx ON t(name);");
  for (size_t i = 0; i < queries.size(); ++i) {
    auto r = Exec(*db, queries[i]);
    REQUIRE(Rows(r) == expected[i]);
    // Inequality matches almost every row, so it stays a heap scan.
    const bool inequality = queries[i].find("!=") != std::string::npos ||
                            queries[i].find("<>") != std::string::npos;
    REQUIRE((r.message.find("INDEX SCAN") == std::string::npos) == inequality);
  }
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id >= 0 LIMIT 3").rows.size() == 3);
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id >= 0 LIMIT 0").rows.empty());
  REQUIRE(!db->Execute("CREATE INDEX id_idx ON t(id)", nullptr).ok());
  Exec(*db, "CREATE INDEX IF NOT EXISTS id_idx ON t(id)");
  REQUIRE(!db->Execute("CREATE INDEX bad ON t(missing)", nullptr).ok());
  REQUIRE(
      !db->Execute("CREATE INDEX bad ON sys_tables(table_id)", nullptr).ok());
  REQUIRE(!db->Execute("CREATE INDEX bad ON t(id,name)", nullptr).ok());
  Exec(*db,
       "CREATE TABLE unsupported (v DOUBLE); INSERT INTO unsupported VALUES "
       "(1.5)");
  REQUIRE(!db->Execute("CREATE INDEX bad ON unsupported(v)", nullptr).ok());
  Exec(*db, "UPDATE t SET id = -7, name = 'changed' WHERE id = 17");
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id = 17").rows.empty());
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id = -7").rows.size() ==
          expected[0].size());
  // Force forwarding by growing rows on a full heap page; original RID is
  // stable.
  const auto original = Rows(Exec(*db, "SELECT rid FROM t WHERE id = -7"));
  Exec(*db,
       "UPDATE t SET note = '" + std::string(3500, 'q') + "' WHERE id = -7");
  REQUIRE(Rows(Exec(*db, "SELECT rid FROM t WHERE id = -7")) == original);
  Exec(*db, "UPDATE t SET id = NULL, name = NULL WHERE id = -7");
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE name = 'changed'").rows.empty());
  Exec(*db, "UPDATE t SET id = -8, name = 'restored' WHERE note = 'null'");
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id = -8").rows.size() == 1);
  Exec(*db, "DELETE FROM t WHERE name = 'name5'");
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE name = 'name5'").rows.empty());
  const auto copy_path = Temp("copy.tbl");
  {
    std::ofstream f(copy_path);
    f << "777|copy name|copy note|\n778|copy two|copy note|\n";
  }
  Exec(*db, "COPY t FROM '" + copy_path + "'");
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id = 777").rows.size() == 1);
  Exec(*db, "CREATE INDEX id_second ON t(id)");
  Exec(*db, "INSERT INTO t VALUES (777, 'duplicate', 'x')");
  Exec(*db,
       "PRAGMA index_check(id_idx); PRAGMA index_check(id_second); PRAGMA "
       "index_check(name_idx)");
  Ok(db->Close());
  db.reset();
  db = Take(Database::Open(config));
  REQUIRE(Exec(*db, "PRAGMA indexes(t)").rows.size() == 3);
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id = 777").rows.size() == 2);
  for (const auto& sql : queries) {
    const auto indexed = Rows(Exec(*db, sql));
    const auto indexes = Exec(*db, "PRAGMA indexes(t)");
    for (const auto& row : indexes.rows)
      Exec(*db, "DROP INDEX " + row[0].string_value);
    REQUIRE(Rows(Exec(*db, sql)) == indexed);
    Exec(*db, "CREATE INDEX id_idx ON t(id); CREATE INDEX name_idx ON t(name)");
  }
  Exec(*db, "DELETE FROM t");
  REQUIRE(Exec(*db, "PRAGMA index_stats(id_idx)").rows[0][1].int64_value == 0);
  REQUIRE(Exec(*db, "PRAGMA index_stats(name_idx)").rows[0][2].int32_value ==
          1);
  Exec(*db,
       "INSERT INTO t VALUES (1, 'after delete', 'x'); DROP INDEX id_idx; DROP "
       "INDEX IF EXISTS id_idx;");
  REQUIRE(Exec(*db, "SELECT * FROM t WHERE id = 1").message.empty());
  Exec(*db, "DROP TABLE t");
  REQUIRE(Exec(*db, "PRAGMA indexes").rows.empty());
  Ok(db->Close());
  db.reset();
  db = Take(Database::Open(config));
  REQUIRE(Exec(*db, "PRAGMA indexes").rows.empty());
  Ok(db->Close());
  db.reset();
  std::filesystem::remove_all(dir);
  std::filesystem::remove(copy_path);
}
// UPDATE and DELETE locate rows through an index when one exists. The same
// statements on an indexed and an unindexed copy of a table must change the
// same rows, including when they rewrite the very column they filter on.
void DmlThroughIndex() {
  const auto dir = Temp("dml");
  std::filesystem::remove_all(dir);
  Config config;
  config.path = dir;
  config.buffer_pool_frames = 8;
  config.log_level = "off";
  auto db = Take(Database::Open(config));
  const auto insert_rows = [](const std::string& table) {
    std::string insert = "INSERT INTO " + table + " VALUES ";
    for (int i = 0; i < 1500; ++i) {
      if (i) insert += ',';
      insert +=
          '(' + std::to_string(i) + ',' +
          (i % 17 == 0 ? "NULL" : std::to_string(i * 7 % 101)) + ',' +
          (i % 13 == 0 ? "NULL" : "'name" + std::to_string(i % 10) + "'") +
          ",'x')";
    }
    return insert;
  };
  for (const std::string table : {"plain", "indexed"}) {
    Exec(*db, "CREATE TABLE " + table +
                  " (id INTEGER, k INTEGER, s CHAR(12), v VARCHAR(4000));");
    Exec(*db, insert_rows(table));
  }
  // Rows that outgrow their page are forwarded but keep their RID.
  const std::string grow = " SET v = '" + std::string(2500, 'x') + "' WHERE ";
  Exec(*db, "UPDATE indexed" + grow + "id < 40");
  Exec(*db, "UPDATE plain" + grow + "id < 40");
  Exec(*db, "CREATE INDEX ik ON indexed(k); CREATE INDEX i_s ON indexed(s);");

  struct Step {
    std::string before, after;  // around the table name
    bool by_index;
  };
  const std::vector<Step> steps = {
      {"UPDATE ", " SET k = 500 WHERE k = 7", true},
      {"UPDATE ", " SET k = 8 WHERE k >= 90", true},
      {"UPDATE ", " SET k = 1000 WHERE k < 5", true},
      {"UPDATE ", " SET k = NULL WHERE k = 20", true},
      {"UPDATE ", " SET s = 'moved' WHERE s <= 'name3'", true},
      {"UPDATE ", " SET k = 3 WHERE s = 'moved   '", true},
      {"UPDATE ", grow + "k = 8", true},
      {"UPDATE ", " SET k = 77 WHERE k != 500", false},
      {"UPDATE ", " SET s = 'late' WHERE id >= 1400", false},
      {"DELETE FROM ", " WHERE k = 500", true},
      {"DELETE FROM ", " WHERE s > 'name8'", true},
      {"DELETE FROM ", " WHERE k <= 3", true},
      {"UPDATE ", " SET v = 'small' WHERE k = 77", true},
      {"DELETE FROM ", " WHERE k = 12345", true},
      {"DELETE FROM ", " WHERE k >= 0", true},
      {"DELETE FROM ", "", false},
  };
  for (const Step& step : steps) {
    const auto plain = Exec(*db, step.before + "plain" + step.after);
    const auto indexed = Exec(*db, step.before + "indexed" + step.after);
    REQUIRE(plain.changes == indexed.changes);
    REQUIRE(plain.message.find("INDEX SCAN") == std::string::npos);
    REQUIRE((indexed.message.find("INDEX SCAN") != std::string::npos) ==
            step.by_index);
    REQUIRE(Rows(Exec(*db, "SELECT * FROM plain")) ==
            Rows(Exec(*db, "SELECT * FROM indexed")));
    const auto remaining = Exec(*db, "SELECT * FROM indexed").rows.size();
    int64_t nulls = 0;
    for (const auto& row : Exec(*db, "SELECT k FROM indexed").rows)
      nulls += row[0].IsNull();
    REQUIRE(Exec(*db, "PRAGMA index_check(ik)").rows[0][1].int64_value ==
            static_cast<int64_t>(remaining) - nulls);
    Exec(*db, "PRAGMA index_check(i_s)");
  }
  REQUIRE(Exec(*db, "SELECT * FROM indexed").rows.empty());

  // CREATE INDEX under a tiny sort budget goes through the external merge
  // and must produce an index that answers like a heap scan.
  Exec(*db, insert_rows("indexed"));
  Exec(*db, "DROP INDEX ik; PRAGMA index_sort_memory = 1;");
  const auto built = Exec(*db, "CREATE INDEX ik ON indexed(k)");
  REQUIRE(built.message.find("external sort") != std::string::npos);
  Exec(*db, "PRAGMA index_check(ik)");
  Exec(*db, insert_rows("plain"));
  for (const std::string where : {"k = 7", "k >= 90", "k < 5"})
    REQUIRE(Rows(Exec(*db, "SELECT * FROM plain WHERE " + where)) ==
            Rows(Exec(*db, "SELECT * FROM indexed WHERE " + where)));
  Ok(db->Close());
  std::filesystem::remove_all(dir);
}

void ChecksumDetection() {
  auto dir = Temp("crc");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir + "/t.idx";
  {
    LogManager log;
    BufferPool pool(log, BufferConfig{2, "clock"});
    auto tree = Take(
        BPlusTree::Create(pool, path, {"t", "t", 3, 0, TypeId::kInt32, 0}));
    Ok(tree->Insert(Value::Int32(1), Rid(1)));
    Ok(pool.FlushAll());
  }
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(2 * kPageSize + 65);
    file.put('\x7f');
  }
  {
    LogManager log;
    BufferPool pool(log, BufferConfig{2, "clock"});
    auto tree = Take(BPlusTree::Open(pool, path));
    auto result = tree->Find(Value::Int32(1));
    REQUIRE(!result.ok() && result.status().code() == StatusCode::kCorruption);
  }
  std::filesystem::remove_all(dir);
}
}  // namespace
int main() {
  Logger::Instance().SetLevel(Logger::ParseLevel("off"));
  TreeRandom(false);
  std::cout << "integer randomized/persistence: OK\n";
  TreeRandom(true);
  std::cout << "string randomized/internal splits: OK\n";
  BulkLoad();
  std::cout << "bulk load sizes/fanouts/then maintenance: OK\n";
  TinyFanoutAndBoundaries();
  std::cout << "tiny fanout/BIGINT boundaries: OK\n";
  SqlIntegration();
  std::cout << "SQL build/maintenance/ranges/reopen/drop: OK\n";
  DmlThroughIndex();
  std::cout << "UPDATE/DELETE through indexes vs. heap scan: OK\n";
  ChecksumDetection();
  std::cout << "checksum corruption detection: OK\n";
}
