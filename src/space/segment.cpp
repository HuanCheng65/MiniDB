#include "space/segment.h"

#include <algorithm>
#include <cstring>

#include "common/logger.h"
#include "space/space_manager.h"

namespace minidb {
namespace {

constexpr size_t kMetaSegmentIdOffset = Segment::kMetaOffset + 0;
constexpr size_t kSegmentTypeOffset = Segment::kMetaOffset + 4;
constexpr size_t kExtentSlotsOffset = Segment::kMetaOffset + 8;
constexpr size_t kExtentCountOffset = Segment::kMetaOffset + 12;
constexpr size_t kPageCountOffset = Segment::kMetaOffset + 16;
constexpr size_t kOverflowPageOffset = Segment::kMetaOffset + 20;
constexpr size_t kInsertHintOffset = Segment::kMetaOffset + 24;

void WriteExtentEntry(std::byte* base, size_t index, const ExtentEntry& entry) {
  const size_t offset = Segment::kExtentListOffset + index * 16;
  WriteU32(base, offset, entry.extent_id);
  WriteU32(base, offset + 4, 0);
  WriteU64(base, offset + 8, entry.used_mask);
}

ExtentEntry ReadExtentEntry(const std::byte* base, size_t index) {
  const size_t offset = Segment::kExtentListOffset + index * 16;
  ExtentEntry entry;
  entry.extent_id = ReadU32(base, offset);
  entry.used_mask = ReadU64(base, offset + 8);
  return entry;
}

}  // namespace

const std::shared_ptr<DiskManager>& Segment::data_disk() const {
  return space_.disk_ptr();
}

Result<std::unique_ptr<Segment>> Segment::Create(    SpaceManager& space, BufferPool& buffer_pool, LogManager& log,
    segment_id_t segment_id, SegmentType type, Transaction* /*txn*/,
    extent_id_t first_extent, FreeSpaceMap& fsm) {
  auto segment =
      std::unique_ptr<Segment>(new Segment(space, buffer_pool, log, fsm));
  segment->segment_id_ = segment_id;
  segment->segment_type_ = type;
  segment->header_page_ = first_extent * kPagesPerExtent;

  ExtentEntry entry;
  entry.extent_id = first_extent;
  entry.used_mask = 0x1;  // only the segment header page lives here
  segment->extents_.push_back(entry);
  segment->extent_slot_[first_extent] = 0;
  segment->page_count_ = 1;
  segment->special_pages_.insert(segment->header_page_);

  {
    auto header = buffer_pool.NewPage(space.disk_ptr(), segment->header_page_,
                                      PageType::kSegmentHeader, segment_id);
    if (!header.ok()) return header.status();
    WritePageGuard guard = header.TakeValue();
    segment->WriteMeta(guard.MutableData());
  }

  // Create the first FSM entry page immediately so the FSM file is usable.
  const Status fsm_status = fsm.EnsureCapacity(0);
  if (!fsm_status.ok()) return fsm_status;

  MINIDB_LOG_INFO("SPACE") << "segment " << segment_id << " created at page "
                           << segment->header_page_ << " extent " << first_extent
                           << " file=" << space.disk_ptr()->name();
  return segment;
}

Result<std::unique_ptr<Segment>> Segment::Open(SpaceManager& space,
                                               BufferPool& buffer_pool,
                                               LogManager& log,
                                               page_id_t header_page,
                                               FreeSpaceMap& fsm) {
  auto segment =
      std::unique_ptr<Segment>(new Segment(space, buffer_pool, log, fsm));
  segment->header_page_ = header_page;

  uint32_t extent_slots = 0;
  uint32_t overflow_page = kInvalidPageId;
  {
    auto guard_result =
        buffer_pool.FetchRead(space.disk_ptr(), header_page);
    if (!guard_result.ok()) return guard_result.status();
    ReadPageGuard guard = guard_result.TakeValue();
    const std::byte* page = guard.Data();
    if (GetPageType(page) != PageType::kSegmentHeader) {
      return Status(StatusCode::kCorruption, "page is not a segment header");
    }
    segment->segment_id_ = ReadU32(page, kMetaSegmentIdOffset);
    segment->segment_type_ =
        static_cast<SegmentType>(ReadU16(page, kSegmentTypeOffset));
    extent_slots = ReadU32(page, kExtentSlotsOffset);
    segment->page_count_ = ReadU32(page, kPageCountOffset);
    overflow_page = ReadU32(page, kOverflowPageOffset);
    segment->insert_hint_spno_ = ReadU32(page, kInsertHintOffset);
    segment->persisted_insert_hint_ = segment->insert_hint_spno_;
    const size_t inline_count =
        std::min<size_t>(extent_slots, kInlineExtentCapacity);
    for (size_t i = 0; i < inline_count; ++i) {
      segment->extents_.push_back(ReadExtentEntry(page, i));
    }
  }

  uint32_t page = overflow_page;
  size_t guard_limit = 0;
  while (page != kInvalidPageId) {
    if (++guard_limit > 1'000'000) {
      return Status(StatusCode::kCorruption, "segment extent list loop");
    }
    auto guard_result = buffer_pool.FetchRead(space.disk_ptr(), page);
    if (!guard_result.ok()) return guard_result.status();
    ReadPageGuard guard = guard_result.TakeValue();
    const std::byte* data = guard.Data();
    if (GetPageType(data) != PageType::kExtentList) {
      return Status(StatusCode::kCorruption, "bad extent list overflow page");
    }
    const uint32_t next = ReadU32(data, kOverflowHeaderOffset);
    const uint32_t count = ReadU32(data, kOverflowHeaderOffset + 4);
    if (count > kOverflowExtentCapacity) {
      return Status(StatusCode::kCorruption, "bad extent list overflow count");
    }
    for (uint32_t i = 0; i < count; ++i) {
      ExtentEntry entry;
      const size_t offset = kOverflowListOffset + i * 16;
      entry.extent_id = ReadU32(data, offset);
      entry.used_mask = ReadU64(data, offset + 8);
      segment->extents_.push_back(entry);
    }
    segment->overflow_pages_.push_back(page);
    segment->special_pages_.insert(page);
    page = next;
  }

  if (segment->extents_.size() != extent_slots) {
    return Status(StatusCode::kCorruption,
                  "segment extent list length mismatch");
  }
  if (segment->extents_.empty()) {
    return Status(StatusCode::kCorruption, "segment has no extents");
  }
  for (size_t i = 0; i < segment->extents_.size(); ++i) {
    const ExtentEntry& entry = segment->extents_[i];
    if (!entry.valid()) continue;
    if (segment->extent_slot_.count(entry.extent_id) != 0) {
      return Status(StatusCode::kCorruption, "duplicate extent in segment");
    }
    segment->extent_slot_[entry.extent_id] = static_cast<uint32_t>(i);
  }
  segment->special_pages_.insert(header_page);

  // Cover all spnos defined by the extent-list slots. The FSM is a hint and
  // is rebuilt/initialized lazily if its header count is behind.
  const Status fsm_status = fsm.EnsureCapacity(
      static_cast<uint32_t>(segment->extents_.size() * kPagesPerExtent - 1));
  if (!fsm_status.ok()) return fsm_status;
  return segment;
}

Result<page_id_t> Segment::AllocatePageInternal(Transaction* txn) {
  // Look for a free page starting where the last allocation succeeded, so a
  // bulk load does not rescan every full extent (design doc 7.3 step 1).
  uint32_t slot = 0;
  bool found = false;
  const size_t slot_total = extents_.size();
  for (size_t i = 0; i < slot_total; ++i) {
    const size_t index = (alloc_hint_slot_ + i) % slot_total;
    if (extents_[index].valid() && extents_[index].used_mask != kFullMask) {
      slot = static_cast<uint32_t>(index);
      found = true;
      break;
    }
  }

  if (!found) {
    auto extent = space_.AllocateExtent(txn);
    if (!extent.ok()) return extent.status();
    ExtentEntry entry;
    entry.extent_id = extent.value();
    entry.used_mask = 0;
    // Fill the first hole left by a freed extent before growing the list, so
    // spno values stay dense (design doc 5.5 / 7.3 step 3). The FSM entries of
    // a hole were zeroed when its pages were freed.
    auto hole = std::find_if(extents_.begin(), extents_.end(),
                             [](const ExtentEntry& e) { return !e.valid(); });
    if (hole != extents_.end()) {
      *hole = entry;
      slot = static_cast<uint32_t>(hole - extents_.begin());
    } else {
      extents_.push_back(entry);
      slot = static_cast<uint32_t>(extents_.size() - 1);
    }
    extent_slot_[entry.extent_id] = slot;
  }

  const uint64_t mask = extents_[slot].used_mask;
  const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(~mask));
  extents_[slot].used_mask |= (1ULL << bit);
  ++page_count_;
  alloc_hint_slot_ = slot;

  const page_id_t page_id =
      extents_[slot].extent_id * kPagesPerExtent + bit;
  const uint32_t spno = SpnoOf(page_id, slot);
  const Status fsm_status = fsm_.EnsureCapacity(spno);
  if (!fsm_status.ok()) {
    extents_[slot].used_mask &= ~(1ULL << bit);
    --page_count_;
    return fsm_status;
  }
  return page_id;
}

Result<page_id_t> Segment::AllocatePage(Transaction* txn) {
  auto allocation = AllocatePageInternal(txn);
  if (!allocation.ok()) return allocation.status();
  const Status status = Persist(txn);
  if (!status.ok()) return status;
  MINIDB_LOG_DEBUG("SPACE") << "ALLOC file=" << space_.disk_ptr()->name()
                            << " page=" << allocation.value()
                            << " seg=" << segment_id_;
  return allocation.value();
}

Status Segment::FreePage(Transaction* txn, page_id_t page_id) {
  const extent_id_t extent = page_id / kPagesPerExtent;
  const uint32_t bit = page_id % kPagesPerExtent;
  auto slot = SlotOfExtent(extent);
  if (!slot.ok()) {
    return Status(StatusCode::kNotFound,
                  "page " + std::to_string(page_id) + " not in segment");
  }
  ExtentEntry& entry = extents_[slot.value()];
  if ((entry.used_mask & (1ULL << bit)) == 0) {
    return Status(StatusCode::kNotFound, "page is already free");
  }
  if (special_pages_.count(page_id) != 0) {
    return Status(StatusCode::kInvalidArgument,
                  "segment header/extent-list pages cannot be freed");
  }
  const uint32_t spno = SpnoOf(page_id, slot.value());
  const Status fsm_status = fsm_.Update(spno, 0);
  if (!fsm_status.ok()) return fsm_status;

  entry.used_mask &= ~(1ULL << bit);
  --page_count_;
  buffer_pool_.DiscardPage(space_.disk_ptr(), page_id);
  MINIDB_LOG_DEBUG("SPACE") << "FREE file=" << space_.disk_ptr()->name()
                            << " page=" << page_id << " seg=" << segment_id_;
  special_pages_.erase(page_id);

  if (entry.used_mask == 0 && slot.value() != 0) {
    const Status status = space_.FreeExtent(txn, entry.extent_id);
    if (!status.ok()) return status;
    extent_slot_.erase(entry.extent_id);
    entry.extent_id = kInvalidExtentId;
  }
  return Persist(txn);
}

Result<page_id_t> Segment::FindPageWithSpace(uint32_t need_bytes) {
  if (fsm_.capacity() == 0) return kInvalidPageId;
  uint32_t need_cat = (need_bytes + 31) / 32;
  if (need_cat == 0) need_cat = 1;
  if (need_cat > 254) need_cat = 254;

  const uint32_t total_entries = fsm_.capacity();
  const uint32_t start = insert_hint_spno_ % total_entries;
  const size_t start_page = start / FreeSpaceMap::kEntriesPerPage;
  const uint32_t start_offset = start % FreeSpaceMap::kEntriesPerPage;
  const size_t page_total = fsm_.page_count();

  // Scan from the hint to the end, then wrap once and finish with the part of
  // the start page that precedes the hint (design doc 7.4 step 2).
  for (size_t k = 0; k <= page_total; ++k) {
    const size_t page_index = (start_page + k) % page_total;
    uint32_t lo = 0;
    uint32_t hi = FreeSpaceMap::kEntriesPerPage;
    if (k == 0) {
      lo = start_offset;
    } else if (k == page_total) {
      hi = start_offset;
      if (hi == 0) break;
    }
    if (fsm_.MaxCategoryHint(static_cast<uint32_t>(page_index)) < need_cat) {
      continue;
    }
    auto guard_result = fsm_.ReadPage(static_cast<uint32_t>(page_index));
    if (!guard_result.ok()) return guard_result.status();
    ReadPageGuard guard = guard_result.TakeValue();
    const std::byte* data = guard.Data();
    uint8_t seen_max = 0;
    for (uint32_t offset = lo; offset < hi; ++offset) {
      const uint8_t category = ReadU8(data, kPageHeaderSize + offset);
      if (category > seen_max) seen_max = category;
      if (category < need_cat) continue;
      const uint32_t spno = static_cast<uint32_t>(
          page_index * FreeSpaceMap::kEntriesPerPage + offset);
      const uint32_t slot = spno / kPagesPerExtent;
      const uint32_t bit = spno % kPagesPerExtent;
      if (slot >= extents_.size()) continue;
      const ExtentEntry& entry = extents_[slot];
      if (!entry.valid() || (entry.used_mask & (1ULL << bit)) == 0) continue;
      insert_hint_spno_ = spno;
      return entry.extent_id * kPagesPerExtent + bit;
    }
    if (lo == 0 && hi == FreeSpaceMap::kEntriesPerPage) {
      // A full scan found nothing usable: remember the page's real maximum so
      // later searches can skip it without reading it.
      fsm_.SetMaxCategoryHint(static_cast<uint32_t>(page_index), seen_max);
    }
  }
  return kInvalidPageId;
}

void Segment::UpdateFreeSpace(page_id_t page_id, uint32_t free_bytes) {
  const extent_id_t extent = page_id / kPagesPerExtent;
  auto slot = SlotOfExtent(extent);
  if (!slot.ok()) return;
  const uint32_t spno = SpnoOf(page_id, slot.value());
  uint32_t category = free_bytes / 32;
  if (category > 254) category = 254;
  const Status status = fsm_.Update(spno, static_cast<uint8_t>(category));
  if (!status.ok()) {
    MINIDB_LOG_WARN("FSM") << "cannot update spno " << spno << ": "
                           << status.message();
    return;
  }
  insert_hint_spno_ = spno;
}

Status Segment::Flush(Transaction* txn) {
  // Allocation and free already persist the extent list; only the insert hint
  // is kept in memory between them, so skip the rewrite when it is current.
  if (insert_hint_spno_ == persisted_insert_hint_) return Status::Ok();
  return Persist(txn);
}

bool Segment::IsAllocatedDataPage(page_id_t page_id) const {
  auto slot = SlotOfExtent(page_id / kPagesPerExtent);
  if (!slot.ok()) return false;
  const uint64_t bit = 1ULL << (page_id % kPagesPerExtent);
  return (extents_[slot.value()].used_mask & bit) != 0 &&
         special_pages_.count(page_id) == 0;
}

Result<uint32_t> Segment::SpnoOfPage(page_id_t page_id) const {
  auto slot = SlotOfExtent(page_id / kPagesPerExtent);
  if (!slot.ok()) return slot.status();
  return SpnoOf(page_id, slot.value());
}

Result<uint8_t> Segment::FreeSpaceCategory(page_id_t page_id) {
  auto spno = SpnoOfPage(page_id);
  if (!spno.ok()) return spno.status();
  return fsm_.Get(spno.value());
}

void Segment::WriteMeta(std::byte* page) const {
  std::memset(page + kPageHeaderSize, 0, kPageSize - kPageHeaderSize);
  WriteU32(page, kMetaSegmentIdOffset, segment_id_);
  WriteU16(page, kSegmentTypeOffset, static_cast<uint16_t>(segment_type_));
  WriteU16(page, kSegmentTypeOffset + 2, 0);
  WriteU32(page, kExtentSlotsOffset, static_cast<uint32_t>(extents_.size()));
  WriteU32(page, kExtentCountOffset, static_cast<uint32_t>(extent_count()));
  WriteU32(page, kPageCountOffset, page_count_);
  WriteU32(page, kOverflowPageOffset,
           overflow_pages_.empty() ? kInvalidPageId : overflow_pages_.front());
  WriteU32(page, kInsertHintOffset, insert_hint_spno_);
  const size_t inline_count =
      std::min(extents_.size(), kInlineExtentCapacity);
  for (size_t i = 0; i < inline_count; ++i) {
    WriteExtentEntry(page, i, extents_[i]);
  }
}

Status Segment::Persist(Transaction* txn) {
  // Grow the overflow chain first; this may itself allocate pages/extents.
  for (size_t iteration = 0; iteration < 1'000'000; ++iteration) {
    const size_t extra = extents_.size() > kInlineExtentCapacity
                             ? extents_.size() - kInlineExtentCapacity
                             : 0;
    const size_t needed =
        (extra + kOverflowExtentCapacity - 1) / kOverflowExtentCapacity;
    if (overflow_pages_.size() >= needed) break;
    auto allocation = AllocatePageInternal(txn);
    if (!allocation.ok()) return allocation.status();
    overflow_pages_.push_back(allocation.value());
    special_pages_.insert(allocation.value());
  }

  {
    auto guard_result =
        buffer_pool_.FetchWrite(space_.disk_ptr(), header_page_);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    WriteMeta(guard.MutableData());
  }
  persisted_insert_hint_ = insert_hint_spno_;

  for (size_t i = 0; i < overflow_pages_.size(); ++i) {
    auto guard_result =
        buffer_pool_.FetchWrite(space_.disk_ptr(), overflow_pages_[i]);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    std::byte* data = guard.MutableData();
    InitPageHeader(data, PageType::kExtentList, overflow_pages_[i], segment_id_);
    std::memset(data + kPageHeaderSize, 0, kPageSize - kPageHeaderSize);
    const page_id_t next =
        i + 1 < overflow_pages_.size() ? overflow_pages_[i + 1] : kInvalidPageId;
    WriteU32(data, kOverflowHeaderOffset, next);
    const size_t start = kInlineExtentCapacity + i * kOverflowExtentCapacity;
    const size_t count =
        std::min(kOverflowExtentCapacity, extents_.size() - start);
    WriteU32(data, kOverflowHeaderOffset + 4, static_cast<uint32_t>(count));
    for (size_t j = 0; j < count; ++j) {
      const size_t offset = kOverflowListOffset + j * 16;
      WriteU32(data, offset, extents_[start + j].extent_id);
      WriteU32(data, offset + 4, 0);
      WriteU64(data, offset + 8, extents_[start + j].used_mask);
    }
  }
  return Status::Ok();
}

Result<uint32_t> Segment::SlotOfExtent(extent_id_t extent) const {
  auto found = extent_slot_.find(extent);
  if (found == extent_slot_.end()) {
    return Status(StatusCode::kNotFound,
                  "extent " + std::to_string(extent) + " not in segment");
  }
  return found->second;
}

std::vector<page_id_t> Segment::AllocatedPages() const {
  std::vector<page_id_t> pages;
  for (size_t slot = 0; slot < extents_.size(); ++slot) {
    const ExtentEntry& entry = extents_[slot];
    if (!entry.valid()) continue;
    uint64_t mask = entry.used_mask;
    while (mask != 0) {
      const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask));
      mask &= mask - 1;
      const page_id_t page_id = entry.extent_id * kPagesPerExtent + bit;
      if (special_pages_.count(page_id) == 0) pages.push_back(page_id);
    }
  }
  return pages;
}

Status Segment::Drop(Transaction* txn) {
  for (const ExtentEntry& entry : extents_) {
    if (!entry.valid()) continue;
    const Status status = space_.FreeExtent(txn, entry.extent_id);
    if (!status.ok()) return status;
  }
  extents_.clear();
  extent_slot_.clear();
  overflow_pages_.clear();
  special_pages_.clear();
  page_count_ = 0;
  return Status::Ok();
}

size_t Segment::extent_count() const {
  size_t count = 0;
  for (const ExtentEntry& entry : extents_) {
    if (entry.valid()) ++count;
  }
  return count;
}

}  // namespace minidb
