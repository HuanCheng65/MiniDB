// Storage-layer tests against the internal C++ API (heap pages, forwarding,
// FSM search, extent reuse, checksums). Same self-contained CHECK style as
// test_minidb.cpp.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "buffer/buffer_pool.h"
#include "catalog/catalog.h"
#include "common/crc32c.h"
#include "common/util.h"
#include "record/schema.h"
#include "record/tuple.h"
#include "record/value.h"
#include "table/heap_page.h"
#include "table/table_heap.h"
#include "txn/transaction.h"

namespace minidb {
namespace {

int g_failures = 0;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                               \
      ++g_failures;                                                           \
    }                                                                         \
  } while (0)

std::string TempDir(const std::string& name) {
  const std::string dir = "/tmp/minidb_storage_" +
                          std::to_string(static_cast<long long>(getpid())) +
                          "_" + name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return dir;
}

Schema TestSchema() {
  return Schema::Make({Column{"id", TypeId::kInt32, 0, 0, false},
                       Column{"v", TypeId::kVarchar, 4000, 0, false}})
      .TakeValue();
}

Tuple Row(const Schema& schema, int id, size_t length, char fill = 'a') {
  return Tuple::FromValues(schema, {Value::Int32(id),
                                    Value::String(std::string(length, fill))})
      .TakeValue();
}

// Decodes (id, length of v) of every row a full scan returns.
std::vector<std::pair<int, size_t>> ScanAll(TableHeap& heap,
                                            std::vector<RID>* rids = nullptr) {
  std::vector<std::pair<int, size_t>> rows;
  auto iterator = heap.Scan(nullptr);
  RID rid;
  Tuple tuple;
  while (iterator.Next(&rid, &tuple)) {
    auto values = Tuple::Decode(heap.schema(), tuple.data().data(), tuple.size());
    CHECK(values.ok());
    if (!values.ok()) continue;
    rows.emplace_back(values.value()[0].int32_value,
                      values.value()[1].string_value.size());
    if (rids != nullptr) rids->push_back(rid);
  }
  CHECK(iterator.status().ok());
  return rows;
}

// Owns the pieces Database normally wires together.
struct Env {
  explicit Env(const std::string& dir, uint32_t frames = 64, bool create = true) {
    BufferConfig config;
    config.frames = frames;
    pool = std::make_unique<BufferPool>(log, config);
    catalog = (create ? Catalog::Bootstrap(dir, *pool, log)
                      : Catalog::Open(dir, *pool, log))
                  .TakeValue();
  }
  ~Env() { Close(); }
  void Close() {
    if (!catalog) return;
    Transaction txn(1);
    CHECK(catalog->FlushAllSegments(&txn).ok());
    CHECK(pool->FlushAll().ok());
    catalog.reset();
    pool.reset();
  }
  TableHeap& Create(const std::string& name) {
    Transaction txn(1);
    return *catalog->CreateTable(&txn, name, TestSchema()).value()->heap;
  }
  TableHeap& Get(const std::string& name) {
    return *catalog->GetTable(name).value()->heap;
  }

  LogManager log;
  std::unique_ptr<BufferPool> pool;
  std::unique_ptr<Catalog> catalog;
};

bool IsForward(Env& env, TableHeap& heap, RID rid) {
  auto guard = env.pool->FetchRead(heap.disk(), rid.page_id);
  CHECK(guard.ok());
  return HeapPage::GetSlot(guard.value().Data(), rid.slot).forward();
}

void TestCrc32c() {
  std::mt19937 rng(42);
  for (size_t length : {0u, 1u, 7u, 8u, 9u, 31u, 4093u, 8192u}) {
    std::vector<uint8_t> data(length);
    for (auto& b : data) b = static_cast<uint8_t>(rng());
    const uint32_t portable = Crc32cPortable(0, data.data(), data.size());
    CHECK(Crc32c(data.data(), data.size()) == portable);
    const size_t half = length / 2;
    const uint32_t split = Crc32cExtend(Crc32c(data.data(), half),
                                        data.data() + half, length - half);
    CHECK(split == portable);
  }
  // Standard check value for "123456789".
  CHECK(Crc32c("123456789", 9) == 0xE3069283u);
  std::printf("crc32c: hardware=%d\n", Crc32cIsHardwareAccelerated() ? 1 : 0);
}

void TestFormatDouble() {
  CHECK(FormatDouble(4192.40) == "4192.4");
  CHECK(FormatDouble(555285.16) == "555285.16");
  CHECK(FormatDouble(-0.5) == "-0.5");
  CHECK(FormatDouble(42.0) == "42");
  // Shortest form must still read back exactly; precision(15) did not.
  for (double v : {0.1 + 0.2, 1.0 / 3.0, 1e-300, 1.7976931348623157e308}) {
    CHECK(std::strtod(FormatDouble(v).c_str(), nullptr) == v);
  }
}

void TestSchemaOverflow() {
  std::vector<Column> columns;
  for (int i = 0; i < 9; ++i) {
    columns.push_back(Column{"c" + std::to_string(i), TypeId::kChar, 8000, 0, true});
  }
  CHECK(!Schema::Make(columns).ok());  // 72000 bytes used to wrap to 6464
}

void TestForwarding() {
  Env env(TempDir("forward"));
  TableHeap& heap = env.Create("t");
  const Schema& schema = heap.schema();
  Transaction txn(1);
  std::vector<RID> rids;
  for (int i = 0; i < 40; ++i) rids.push_back(heap.Insert(&txn, Row(schema, i, 200)).value());

  // Grow beyond what the page can hold: the row moves and rid forwards.
  CHECK(heap.Update(&txn, rids[0], Row(schema, 0, 3000)).ok());
  CHECK(IsForward(env, heap, rids[0]));
  std::vector<RID> scanned;
  auto rows = ScanAll(heap, &scanned);
  CHECK(rows.size() == 40);  // moved-in copy must not be returned twice
  const std::set<std::pair<int, size_t>> distinct(rows.begin(), rows.end());
  CHECK(distinct.size() == 40);
  CHECK(distinct.count(std::make_pair(0, size_t{3000})) == 1);
  CHECK(std::find(scanned.begin(), scanned.end(), rids[0]) != scanned.end());

  // Update again, larger: still one hop from the original RID.
  CHECK(heap.Update(&txn, rids[0], Row(schema, 0, 3900)).ok());
  CHECK(ScanAll(heap).size() == 40);
  auto got = heap.Get(&txn, rids[0]);
  CHECK(got.ok() && got.value().size() > 3900);

  // Shrink in place at the target.
  CHECK(heap.Update(&txn, rids[0], Row(schema, 0, 10)).ok());
  CHECK(ScanAll(heap).size() == 40);

  // Collect-then-delete, as DELETE does, must succeed for every row.
  std::vector<RID> all;
  ScanAll(heap, &all);
  int failures = 0;
  for (const RID& rid : all) failures += heap.Delete(&txn, rid).ok() ? 0 : 1;
  CHECK(failures == 0);
  CHECK(ScanAll(heap).empty());
  // RIDs on freed pages must not resurrect deleted rows from stale bytes.
  for (const RID& rid : all) CHECK(!heap.Get(&txn, rid).ok());
  // The first heap page is kept when the table becomes empty (doc 7.4).
  CHECK(heap.segment().page_count() == 2);
}

void TestGrowInPage() {
  Env env(TempDir("grow"));
  TableHeap& heap = env.Create("t");
  const Schema& schema = heap.schema();
  Transaction txn(1);
  const RID a = heap.Insert(&txn, Row(schema, 1, 100)).value();
  const RID b = heap.Insert(&txn, Row(schema, 2, 100)).value();
  CHECK(a.page_id == b.page_id);
  // Plenty of room on the page: grows in place, no forwarding (doc 9.2 step 2).
  CHECK(heap.Update(&txn, a, Row(schema, 1, 2000, 'z')).ok());
  CHECK(!IsForward(env, heap, a));
  auto got = heap.Get(&txn, a);
  CHECK(got.ok());
  auto values = Tuple::Decode(schema, got.value().data().data(), got.value().size());
  CHECK(values.ok() && values.value()[1].string_value == std::string(2000, 'z'));
  auto other = heap.Get(&txn, b);
  CHECK(other.ok());
}

void TestFsmFindsEarlierPages() {
  Env env(TempDir("fsm"));
  TableHeap& heap = env.Create("t");
  const Schema& schema = heap.schema();
  Transaction txn(1);
  std::vector<RID> rids;
  for (int i = 0; i < 400; ++i) rids.push_back(heap.Insert(&txn, Row(schema, i, 1000)).value());
  const page_id_t early_page = rids[8].page_id;  // not the kept first page
  CHECK(heap.Delete(&txn, rids[8]).ok());
  // Move the search hint to the end of the table without emptying that page.
  CHECK(heap.Delete(&txn, rids[rids.size() - 2]).ok());
  const uint32_t pages = heap.segment().page_count();
  // Some insert must land on the early page before any new page is allocated.
  bool reused = false;
  for (int k = 0; k < 64 && !reused; ++k) {
    const RID placed = heap.Insert(&txn, Row(schema, 10000 + k, 1000)).value();
    if (heap.segment().page_count() != pages) break;
    reused = placed.page_id == early_page;
  }
  CHECK(reused);
  CHECK(heap.segment().page_count() == pages);
}

void TestExtentHoleReuse() {
  Env env(TempDir("holes"));
  TableHeap& heap = env.Create("t");
  const Schema& schema = heap.schema();
  Transaction txn(1);
  // ~2 rows per page -> a few hundred pages -> several extents.
  std::vector<RID> rids;
  for (int i = 0; i < 600; ++i) rids.push_back(heap.Insert(&txn, Row(schema, i, 3500)).value());
  const size_t slots = heap.segment().extent_slots();
  CHECK(slots >= 4);
  const size_t extents = heap.segment().extent_count();

  // Empty one middle extent completely.
  const extent_id_t victim = rids[300].page_id / kPagesPerExtent;
  int removed = 0;
  for (const RID& rid : rids) {
    if (rid.page_id / kPagesPerExtent == victim) {
      CHECK(heap.Delete(&txn, rid).ok());
      ++removed;
    }
  }
  CHECK(heap.segment().extent_count() == extents - 1);

  // Refill exactly what was removed: the tail extent's free pages plus the
  // hole are enough, so the hole is reused and the list does not grow.
  for (int i = 0; i < removed; ++i) {
    CHECK(heap.Insert(&txn, Row(schema, 1000 + i, 3500)).ok());
  }
  CHECK(heap.segment().extent_count() == extents);
  CHECK(heap.segment().extent_slots() == slots);
  CHECK(ScanAll(heap).size() == 600);
}

void TestReopenAndZeroFsmPage() {
  const std::string dir = TempDir("reopen");
  {
    Env env(dir);
    TableHeap& heap = env.Create("t");
    Transaction txn(1);
    std::vector<RID> rids;
    for (int i = 0; i < 100; ++i) rids.push_back(heap.Insert(&txn, Row(heap.schema(), i, 300)).value());
    CHECK(heap.Update(&txn, rids[3], Row(heap.schema(), 3, 3800)).ok());
  }
  // Simulate an FSM page that was never flushed (e.g. after kill -9).
  {
    std::fstream fsm(dir + "/t.fsm", std::ios::in | std::ios::out | std::ios::binary);
    CHECK(fsm.good());
    const std::vector<char> zeros(kPageSize, 0);
    fsm.seekp(static_cast<std::streamoff>(kPageSize));
    fsm.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }
  Env env(dir, 64, /*create=*/false);
  TableHeap& heap = env.Get("t");
  CHECK(ScanAll(heap).size() == 100);
  Transaction txn(1);
  CHECK(heap.Insert(&txn, Row(heap.schema(), 100, 300)).ok());
  CHECK(ScanAll(heap).size() == 101);
}

void TestBufferPoolStats() {
  Env env(TempDir("pool"), 4);
  TableHeap& heap = env.Create("t");
  // Reading past the end of the file fails and must give the frame back to
  // the free list instead of leaving it for a later (counted) eviction.
  CHECK(!env.pool->FetchRead(heap.disk(), 1u << 20).ok());
  const uint64_t evicted = env.pool->Stats().evict;
  for (int i = 0; i < 10; ++i) {
    CHECK(!env.pool->FetchRead(heap.disk(), 1u << 20).ok());
  }
  CHECK(env.pool->Stats().evict == evicted);
  // A tiny pool still works end to end with heavy eviction.
  Transaction txn(1);
  for (int i = 0; i < 300; ++i) CHECK(heap.Insert(&txn, Row(heap.schema(), i, 500)).ok());
  CHECK(ScanAll(heap).size() == 300);
  CHECK(env.pool->Stats().evict > 0);
}

}  // namespace
}  // namespace minidb

int main() {
  using namespace minidb;
  TestCrc32c();
  TestFormatDouble();
  TestSchemaOverflow();
  TestForwarding();
  TestGrowInPage();
  TestFsmFindsEarlierPages();
  TestExtentHoleReuse();
  TestReopenAndZeroFsmPage();
  TestBufferPoolStats();
  if (g_failures == 0) {
    std::printf("all storage tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d storage check(s) failed\n", g_failures);
  return 1;
}
