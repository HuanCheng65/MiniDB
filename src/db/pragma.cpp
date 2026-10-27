// PRAGMA statements: engine introspection and settings as ordinary result
// sets (design doc 13.2). Every client - the shell, the C API, the tests -
// reads them through mydb_prepare/mydb_step, the same way SQLite exposes
// PRAGMA and PostgreSQL exposes pg_stat_* views and pageinspect functions.
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/util.h"
#include "db/database.h"
#include "table/heap_page.h"

namespace minidb {
namespace {

using Row = std::vector<Value>;

void SetColumns(QueryResult* result,
                std::initializer_list<std::pair<const char*, TypeId>> columns) {
  for (const auto& [name, type] : columns) {
    result->columns.push_back({name, type});
  }
}

Value Int(int64_t value) { return Value::Int64(value); }
Value Text(std::string value) { return Value::String(std::move(value)); }
Value Percent(double value) {
  return Value::Double(std::round(value * 100.0) / 100.0);
}

const char* PageTypeName(PageType type) {
  switch (type) {
    case PageType::kInvalid:
      return "INVALID";
    case PageType::kFileHeader:
      return "FILE_HEADER";
    case PageType::kExtentBitmap:
      return "EXTENT_BITMAP";
    case PageType::kSegmentHeader:
      return "SEGMENT_HEADER";
    case PageType::kExtentList:
      return "EXTENT_LIST";
    case PageType::kFsm:
      return "FSM";
    case PageType::kHeap:
      return "HEAP";
    case PageType::kBtreeInternal:
      return "BTREE_INTERNAL";
    case PageType::kBtreeLeaf:
      return "BTREE_LEAF";
  }
  return "UNKNOWN";
}

std::string SlotFlagsName(const HeapSlot& slot) {
  if (slot.empty()) return "EMPTY";
  std::string name;
  const auto add = [&](const char* flag) {
    if (!name.empty()) name += "|";
    name += flag;
  };
  if (slot.used()) add("USED");
  if (slot.forward()) add("FORWARD");
  if (slot.moved_in()) add("MOVED_IN");
  return name;
}

struct PragmaContext {
  const PragmaSpec& spec;
  Catalog& catalog;
  BufferPool& pool;
  Transaction* txn;
  QueryResult* result;

  Status ArgCount(size_t expected, const char* usage) const {
    if (spec.args.size() != expected || spec.has_value) {
      return Status(StatusCode::kInvalidArgument,
                    std::string("usage: PRAGMA ") + usage);
    }
    return Status::Ok();
  }
  Result<TableInfo*> TableArg(size_t index) const {
    return catalog.GetTable(spec.args[index].text);
  }
  Result<page_id_t> PageArg(size_t index) const {
    const Literal& literal = spec.args[index];
    if (literal.kind != Literal::Kind::kInteger || literal.integer < 0 ||
        literal.integer >= kInvalidPageId) {
      return Status(StatusCode::kInvalidArgument, "expected a page number");
    }
    return static_cast<page_id_t>(literal.integer);
  }
};

// Per-page counts gathered from the slot directories of a table's heap pages.
struct HeapPageSummary {
  int64_t records = 0;    // user-visible rows stored on the page
  int64_t forwards = 0;   // FORWARD slots (rows living elsewhere)
  int64_t moved_in = 0;   // MOVED_IN copies of rows from other pages
  int64_t empty_slots = 0;
  uint32_t free_bytes = 0;
};

HeapPageSummary SummarizeHeapPage(const std::byte* page) {
  HeapPageSummary summary;
  const uint16_t count = HeapPage::SlotCount(page);
  for (slot_id_t slot = 0; slot < count; ++slot) {
    const HeapSlot s = HeapPage::GetSlot(page, slot);
    if (s.empty()) {
      ++summary.empty_slots;
    } else if (s.forward()) {
      ++summary.forwards;
    } else if (s.moved_in()) {
      ++summary.moved_in;
    } else {
      ++summary.records;
    }
  }
  summary.free_bytes = HeapPage::FreeSpace(page);
  return summary;
}

// Calls fn(page_id, page bytes) for every allocated heap page of a table.
Status ForEachHeapPage(BufferPool& pool, TableInfo* table,
                       const std::function<Status(page_id_t, const std::byte*)>& fn) {
  for (page_id_t page_id : table->segment->AllocatedPages()) {
    auto guard = pool.FetchRead(table->data_disk, page_id);
    if (!guard.ok()) return guard.status();
    const std::byte* page = guard.value().Data();
    if (GetPageType(page) != PageType::kHeap) continue;
    const Status status = fn(page_id, page);
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

Status PragmaList(PragmaContext& ctx);

Status BufferStatsPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(0, "buffer_stats");
  if (!args.ok()) return args;
  const std::vector<FrameInfo> frames = ctx.pool.Frames();
  int64_t pinned = 0;
  int64_t dirty = 0;
  for (const FrameInfo& frame : frames) {
    if (frame.pin_count > 0) ++pinned;
    if (frame.dirty) ++dirty;
  }
  const BufferStats& stats = ctx.pool.Stats();
  SetColumns(ctx.result, {{"frames", TypeId::kInt64},
                          {"used", TypeId::kInt64},
                          {"pinned", TypeId::kInt64},
                          {"dirty", TypeId::kInt64},
                          {"hit", TypeId::kInt64},
                          {"miss", TypeId::kInt64},
                          {"hit_rate", TypeId::kDouble},
                          {"evict", TypeId::kInt64},
                          {"flush", TypeId::kInt64},
                          {"new_page", TypeId::kInt64}});
  ctx.result->rows.push_back(
      {Int(static_cast<int64_t>(ctx.pool.FrameCount())),
       Int(static_cast<int64_t>(frames.size())), Int(pinned), Int(dirty),
       Int(static_cast<int64_t>(stats.hit)), Int(static_cast<int64_t>(stats.miss)),
       Percent(stats.HitRate()), Int(static_cast<int64_t>(stats.evict)),
       Int(static_cast<int64_t>(stats.flush)),
       Int(static_cast<int64_t>(stats.new_page))});
  return Status::Ok();
}

Status BufferFramesPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(0, "buffer_frames");
  if (!args.ok()) return args;
  SetColumns(ctx.result, {{"frame", TypeId::kInt64},
                          {"file", TypeId::kVarchar},
                          {"page_id", TypeId::kInt64},
                          {"page_type", TypeId::kVarchar},
                          {"pin_count", TypeId::kInt64},
                          {"dirty", TypeId::kInt64}});
  for (const FrameInfo& frame : ctx.pool.Frames()) {
    ctx.result->rows.push_back(
        {Int(frame.frame), Text(frame.file), Int(frame.page_id),
         frame.page_type == PageType::kInvalid ? Value::Null()
                                               : Text(PageTypeName(frame.page_type)),
         Int(frame.pin_count), Int(frame.dirty ? 1 : 0)});
  }
  return Status::Ok();
}

Status TableInfoPragma(PragmaContext& ctx) {
  Status status = ctx.ArgCount(1, "table_info(table)");
  if (!status.ok()) return status;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  TableInfo* info = table.value();
  Segment& segment = *info->segment;

  int64_t heap_pages = 0;
  HeapPageSummary total;
  status = ForEachHeapPage(ctx.pool, info, [&](page_id_t, const std::byte* page) {
    const HeapPageSummary summary = SummarizeHeapPage(page);
    ++heap_pages;
    total.records += summary.records;
    total.forwards += summary.forwards;
    total.moved_in += summary.moved_in;
    total.empty_slots += summary.empty_slots;
    total.free_bytes += summary.free_bytes;
    return Status::Ok();
  });
  if (!status.ok()) return status;

  const auto file_bytes = [](const DiskManager& disk) {
    return static_cast<int64_t>(disk.header().file_extents) * kPagesPerExtent *
           kPageSize;
  };
  const double capacity =
      static_cast<double>(heap_pages) * (kPageSize - HeapPage::kSlotArrayOffset);
  SetColumns(ctx.result, {{"table", TypeId::kVarchar},
                          {"data_file", TypeId::kVarchar},
                          {"data_bytes", TypeId::kInt64},
                          {"fsm_file", TypeId::kVarchar},
                          {"fsm_bytes", TypeId::kInt64},
                          {"segment_header", TypeId::kInt64},
                          {"extents", TypeId::kInt64},
                          {"extent_slots", TypeId::kInt64},
                          {"pages", TypeId::kInt64},
                          {"heap_pages", TypeId::kInt64},
                          {"rows", TypeId::kInt64},
                          {"forwarded", TypeId::kInt64},
                          {"empty_slots", TypeId::kInt64},
                          {"free_bytes", TypeId::kInt64},
                          {"fill_pct", TypeId::kDouble},
                          {"fsm_pages", TypeId::kInt64}});
  ctx.result->rows.push_back(
      {Text(info->name), Text(info->data_file), Int(file_bytes(*info->data_disk)),
       Text(info->fsm_file), Int(file_bytes(*info->fsm_disk)),
       Int(segment.header_page()), Int(static_cast<int64_t>(segment.extent_count())),
       Int(static_cast<int64_t>(segment.extent_slots())), Int(segment.page_count()),
       Int(heap_pages), Int(total.records + total.forwards), Int(total.forwards),
       Int(total.empty_slots), Int(total.free_bytes),
       Percent(capacity > 0 ? 100.0 * (1.0 - total.free_bytes / capacity) : 0.0),
       Int(info->fsm->page_count())});
  return Status::Ok();
}

Status ExtentsPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(1, "extents(table)");
  if (!args.ok()) return args;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  const Segment& segment = *table.value()->segment;
  SetColumns(ctx.result, {{"slot", TypeId::kInt64},
                          {"extent_id", TypeId::kInt64},
                          {"first_page", TypeId::kInt64},
                          {"used_pages", TypeId::kInt64},
                          {"page_mask", TypeId::kVarchar}});
  const std::vector<ExtentEntry>& extents = segment.extents();
  for (size_t slot = 0; slot < extents.size(); ++slot) {
    const ExtentEntry& entry = extents[slot];
    if (!entry.valid()) {  // hole left by a freed extent (design doc 5.5)
      ctx.result->rows.push_back({Int(static_cast<int64_t>(slot)), Value::Null(),
                                  Value::Null(), Int(0), Text("(hole)")});
      continue;
    }
    const page_id_t first = entry.extent_id * kPagesPerExtent;
    // One character per page, page 0 of the extent first: H = segment
    // header, 1 = allocated, . = free.
    std::string mask(kPagesPerExtent, '.');
    for (uint32_t bit = 0; bit < kPagesPerExtent; ++bit) {
      if ((entry.used_mask >> bit) & 1ULL) {
        mask[bit] = first + bit == segment.header_page() ? 'H' : '1';
      }
    }
    ctx.result->rows.push_back(
        {Int(static_cast<int64_t>(slot)), Int(entry.extent_id), Int(first),
         Int(__builtin_popcountll(entry.used_mask)), Text(std::move(mask))});
  }
  return Status::Ok();
}

Status ExtentBitmapPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(1, "extent_bitmap(table)");
  if (!args.ok()) return args;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  auto bitmap = table.value()->space->ReadBitmap();
  if (!bitmap.ok()) return bitmap.status();
  const ExtentBitmapInfo& info = bitmap.value();
  SetColumns(ctx.result, {{"first_extent", TypeId::kInt64},
                          {"last_extent", TypeId::kInt64},
                          {"allocated", TypeId::kInt64},
                          {"bits", TypeId::kVarchar},
                          {"alloc_hint", TypeId::kInt64}});
  for (size_t first = 0; first < info.allocated.size(); first += 64) {
    const size_t last = std::min(info.allocated.size(), first + 64) - 1;
    std::string bits;
    int64_t allocated = 0;
    for (size_t i = first; i <= last; ++i) {
      bits.push_back(info.allocated[i] ? '1' : '0');
      if (info.allocated[i]) ++allocated;
    }
    ctx.result->rows.push_back({Int(static_cast<int64_t>(first)),
                                Int(static_cast<int64_t>(last)), Int(allocated),
                                Text(std::move(bits)), Int(info.alloc_hint)});
  }
  return Status::Ok();
}

Status FreeSpacePragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(1, "free_space(table)");
  if (!args.ok()) return args;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  TableInfo* info = table.value();
  SetColumns(ctx.result, {{"page_id", TypeId::kInt64},
                          {"spno", TypeId::kInt64},
                          {"fsm_category", TypeId::kInt64},
                          {"fsm_free_bytes", TypeId::kInt64},
                          {"actual_free_bytes", TypeId::kInt64},
                          {"rows", TypeId::kInt64}});
  // Collect the page contents first: reading an FSM page while holding a heap
  // page latch would break the latch order of design doc 11.5.
  std::vector<std::pair<page_id_t, HeapPageSummary>> pages;
  Status status = ForEachHeapPage(ctx.pool, info, [&](page_id_t page_id,
                                                      const std::byte* page) {
    pages.emplace_back(page_id, SummarizeHeapPage(page));
    return Status::Ok();
  });
  if (!status.ok()) return status;
  for (const auto& [page_id, summary] : pages) {
    auto spno = info->segment->SpnoOfPage(page_id);
    if (!spno.ok()) return spno.status();
    auto category = info->segment->FreeSpaceCategory(page_id);
    if (!category.ok()) return category.status();
    ctx.result->rows.push_back(
        {Int(page_id), Int(spno.value()), Int(category.value()),
         Int(category.value() * 32), Int(summary.free_bytes),
         Int(summary.records + summary.forwards)});
  }
  return Status::Ok();
}

Status FreeSpaceSummaryPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(1, "free_space_summary(table)");
  if (!args.ok()) return args;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  TableInfo* info = table.value();
  // Histogram of what the FSM records, in 1 KB buckets (design doc 13.2).
  std::map<uint32_t, int64_t> buckets;
  for (page_id_t page_id : info->segment->AllocatedPages()) {
    auto category = info->segment->FreeSpaceCategory(page_id);
    if (!category.ok()) return category.status();
    ++buckets[category.value() * 32 / 1024];
  }
  SetColumns(ctx.result, {{"free_bytes", TypeId::kVarchar},
                          {"pages", TypeId::kInt64},
                          {"histogram", TypeId::kVarchar}});
  int64_t widest = 1;
  for (const auto& entry : buckets) widest = std::max(widest, entry.second);
  for (uint32_t bucket = 0; bucket < kPageSize / 1024; ++bucket) {
    const int64_t pages = buckets.count(bucket) != 0 ? buckets[bucket] : 0;
    const std::string range = std::to_string(bucket * 1024) + "-" +
                              std::to_string(bucket * 1024 + 1023);
    const size_t bar = static_cast<size_t>((pages * 40 + widest - 1) / widest);
    ctx.result->rows.push_back(
        {Text(range), Int(pages), Text(std::string(bar, '#'))});
  }
  return Status::Ok();
}

Status PageHeaderPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(2, "page_header(table, page_id)");
  if (!args.ok()) return args;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  auto page_id = ctx.PageArg(1);
  if (!page_id.ok()) return page_id.status();
  TableInfo* info = table.value();

  Row row;
  {
    auto guard = ctx.pool.FetchRead(info->data_disk, page_id.value());
    if (!guard.ok()) return guard.status();
    const std::byte* page = guard.value().Data();
    const PageType type = GetPageType(page);
    row = {Int(page_id.value()), Text(PageTypeName(type)), Int(GetSegmentId(page)),
           Int(static_cast<int64_t>(ReadU64(page, kPageLsnOffset)))};
    if (type == PageType::kHeap) {
      row.push_back(Int(HeapPage::SlotCount(page)));
      row.push_back(Int(HeapPage::LiveCount(page)));
      row.push_back(Int(ReadU16(page, HeapPage::kFreeLowerOffset)));
      row.push_back(Int(ReadU16(page, HeapPage::kFreeUpperOffset)));
      row.push_back(Int(ReadU16(page, HeapPage::kFragBytesOffset)));
      row.push_back(Int(HeapPage::FreeSpace(page)));
    } else {
      row.resize(row.size() + 6, Value::Null());
    }
  }
  auto category = info->segment->FreeSpaceCategory(page_id.value());
  row.push_back(category.ok() ? Int(category.value()) : Value::Null());

  SetColumns(ctx.result, {{"page_id", TypeId::kInt64},
                          {"page_type", TypeId::kVarchar},
                          {"segment_id", TypeId::kInt64},
                          {"page_lsn", TypeId::kInt64},
                          {"slot_count", TypeId::kInt64},
                          {"live_count", TypeId::kInt64},
                          {"free_lower", TypeId::kInt64},
                          {"free_upper", TypeId::kInt64},
                          {"frag_bytes", TypeId::kInt64},
                          {"free_space", TypeId::kInt64},
                          {"fsm_category", TypeId::kInt64}});
  ctx.result->rows.push_back(std::move(row));
  return Status::Ok();
}

Status PageSlotsPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(2, "page_slots(table, page_id)");
  if (!args.ok()) return args;
  auto table = ctx.TableArg(0);
  if (!table.ok()) return table.status();
  auto page_id = ctx.PageArg(1);
  if (!page_id.ok()) return page_id.status();
  auto guard = ctx.pool.FetchRead(table.value()->data_disk, page_id.value());
  if (!guard.ok()) return guard.status();
  const std::byte* page = guard.value().Data();
  if (GetPageType(page) != PageType::kHeap) {
    return Status(StatusCode::kInvalidArgument,
                  "page " + std::to_string(page_id.value()) + " is a " +
                      PageTypeName(GetPageType(page)) + " page, not a heap page");
  }
  SetColumns(ctx.result, {{"slot", TypeId::kInt64},
                          {"offset", TypeId::kInt64},
                          {"length", TypeId::kInt64},
                          {"flags", TypeId::kVarchar},
                          {"forward_to", TypeId::kVarchar}});
  const uint16_t count = HeapPage::SlotCount(page);
  for (slot_id_t slot = 0; slot < count; ++slot) {
    const HeapSlot s = HeapPage::GetSlot(page, slot);
    Value target = Value::Null();
    if (s.forward()) {
      auto rid = HeapPage::ReadForwardTarget(page, slot);
      if (rid.ok()) target = Text(FormatRid(rid.value()));
    }
    ctx.result->rows.push_back(
        {Int(slot), s.empty() ? Value::Null() : Int(s.offset),
         s.empty() ? Value::Null() : Int(s.length), Text(SlotFlagsName(s)),
         std::move(target)});
  }
  return Status::Ok();
}

Status FlushPragma(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(0, "flush");
  if (!args.ok()) return args;
  const uint64_t before = ctx.pool.Stats().flush;
  Status status = ctx.catalog.FlushAllSegments(ctx.txn);
  if (!status.ok()) return status;
  status = ctx.pool.FlushAll();
  if (!status.ok()) return status;
  ctx.result->message = "flushed " +
                        std::to_string(ctx.pool.Stats().flush - before) +
                        " dirty page(s) and synced all files";
  return Status::Ok();
}

Status LogLevelPragma(PragmaContext& ctx) {
  if (!ctx.spec.args.empty()) {
    return Status(StatusCode::kInvalidArgument,
                  "usage: PRAGMA log_level [= debug|info|warn|error|off]");
  }
  if (ctx.spec.has_value) {
    const std::string level = ToLower(ctx.spec.value.text);
    if (level != "debug" && level != "info" && level != "warn" &&
        level != "error" && level != "off") {
      return Status(StatusCode::kInvalidArgument,
                    "log level must be debug, info, warn, error or off");
    }
    Logger::Instance().SetLevel(Logger::ParseLevel(level));
  }
  SetColumns(ctx.result, {{"log_level", TypeId::kVarchar}});
  ctx.result->rows.push_back(
      {Text(Trim(ToLower(Logger::LevelName(Logger::Instance().level()))))});
  return Status::Ok();
}

struct PragmaEntry {
  const char* name;
  const char* arguments;
  const char* description;
  Status (*run)(PragmaContext&);
};

constexpr PragmaEntry kPragmas[] = {
    {"pragma_list", "", "this list", &PragmaList},
    {"buffer_stats", "", "buffer pool counters and hit rate",
     &BufferStatsPragma},
    {"buffer_frames", "", "occupied buffer frames: file, page, pins, dirty",
     &BufferFramesPragma},
    {"table_info", "table", "files, extents, pages, rows, forwarding, fill",
     &TableInfoPragma},
    {"extents", "table", "segment extent list with per-page masks",
     &ExtentsPragma},
    {"extent_bitmap", "table", "data-file extent bitmap, 64 extents per row",
     &ExtentBitmapPragma},
    {"free_space", "table", "FSM category vs. actual free bytes per heap page",
     &FreeSpacePragma},
    {"free_space_summary", "table", "histogram of FSM free space",
     &FreeSpaceSummaryPragma},
    {"page_header", "table, page_id", "common and heap page header fields",
     &PageHeaderPragma},
    {"page_slots", "table, page_id", "slot directory of a heap page",
     &PageSlotsPragma},
    {"flush", "", "write all dirty pages and fsync", &FlushPragma},
    {"log_level", "[= level]", "show or set debug|info|warn|error|off",
     &LogLevelPragma},
};

Status PragmaList(PragmaContext& ctx) {
  const Status args = ctx.ArgCount(0, "pragma_list");
  if (!args.ok()) return args;
  SetColumns(ctx.result, {{"name", TypeId::kVarchar},
                          {"arguments", TypeId::kVarchar},
                          {"description", TypeId::kVarchar}});
  for (const PragmaEntry& entry : kPragmas) {
    ctx.result->rows.push_back(
        {Text(entry.name), Text(entry.arguments), Text(entry.description)});
  }
  return Status::Ok();
}

}  // namespace

Status Database::ExecutePragma(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  PragmaContext ctx{statement->pragma, *catalog_, *pool_, txn, result};
  const std::string name = ToLower(statement->pragma.name);
  for (const PragmaEntry& entry : kPragmas) {
    if (name == entry.name) return entry.run(ctx);
  }
  return Status(StatusCode::kNotFound,
                "unknown pragma " + name + " (see PRAGMA pragma_list)");
}

}  // namespace minidb
