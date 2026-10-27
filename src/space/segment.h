// Segment: extent list, page bitmap and page allocation for one relation data
// file. The free-space map lives in a separate <relation>.fsm file.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "buffer/buffer_pool.h"
#include "common/status.h"
#include "common/types.h"
#include "log/log_manager.h"
#include "space/free_space_map.h"
#include "txn/transaction.h"

namespace minidb {

class SpaceManager;

enum class SegmentType : uint16_t { kHeap = 1, kIndex = 2 };

struct ExtentEntry {
  extent_id_t extent_id = kInvalidExtentId;
  uint64_t used_mask = 0;

  bool valid() const { return extent_id != kInvalidExtentId; }
};

class Segment {
 public:
  static constexpr size_t kMetaOffset = 32;
  static constexpr size_t kExtentListOffset = 32 + 192;
  static constexpr size_t kInlineExtentCapacity = 498;
  static constexpr size_t kOverflowHeaderOffset = 32;
  static constexpr size_t kOverflowListOffset = 40;
  static constexpr size_t kOverflowExtentCapacity =
      (kPageSize - kOverflowListOffset) / 16;
  static constexpr uint64_t kFullMask = ~0ULL;

  static Result<std::unique_ptr<Segment>> Create(
      SpaceManager& space, BufferPool& buffer_pool, LogManager& log,
      segment_id_t segment_id, SegmentType type, Transaction* txn,
      extent_id_t first_extent, FreeSpaceMap& fsm);
  static Result<std::unique_ptr<Segment>> Open(SpaceManager& space,
                                               BufferPool& buffer_pool,
                                               LogManager& log,
                                               page_id_t header_page,
                                               FreeSpaceMap& fsm);
  ~Segment() = default;

  Result<page_id_t> AllocatePage(Transaction* txn);
  Status FreePage(Transaction* txn, page_id_t page_id);

  // Returns kInvalidPageId when no page has enough space.
  Result<page_id_t> FindPageWithSpace(uint32_t need_bytes);
  void UpdateFreeSpace(page_id_t page_id, uint32_t free_bytes);
  Status Flush(Transaction* txn);

  std::vector<page_id_t> AllocatedPages() const;
  Status Drop(Transaction* txn);

  page_id_t header_page() const { return header_page_; }
  // The first page allocated after the segment header; TableHeap never frees
  // it so that an emptied table keeps one heap page (design doc 7.4).
  bool IsFirstDataPage(page_id_t page_id) const {
    return page_id == header_page_ + 1;
  }
  segment_id_t segment_id() const { return segment_id_; }
  SegmentType segment_type() const { return segment_type_; }
  uint32_t page_count() const { return page_count_; }
  size_t extent_count() const;
  // Extent-list length including holes; spno values range over this.
  size_t extent_slots() const { return extents_.size(); }
  const std::vector<ExtentEntry>& extents() const { return extents_; }
  uint32_t insert_hint() const { return insert_hint_spno_; }
  // True for an allocated page that is not the segment header or an
  // extent-list page, i.e. a page that may hold records.
  bool IsAllocatedDataPage(page_id_t page_id) const;
  // spno of an allocated page, or an error if the page is not in the segment.
  Result<uint32_t> SpnoOfPage(page_id_t page_id) const;
  // The FSM category (free bytes / 32) recorded for a page.
  Result<uint8_t> FreeSpaceCategory(page_id_t page_id);

  const std::shared_ptr<DiskManager>& data_disk() const;
  FreeSpaceMap& fsm() { return fsm_; }

 private:
  Segment(SpaceManager& space, BufferPool& buffer_pool, LogManager& log,
          FreeSpaceMap& fsm)
      : space_(space), buffer_pool_(buffer_pool), log_(log), fsm_(fsm) {}

  Result<page_id_t> AllocatePageInternal(Transaction* txn);
  Status Persist(Transaction* txn);
  void WriteMeta(std::byte* page) const;
  Result<uint32_t> SlotOfExtent(extent_id_t extent) const;
  uint32_t SpnoOf(page_id_t page_id, uint32_t slot) const {
    return slot * kPagesPerExtent + (page_id % kPagesPerExtent);
  }

  SpaceManager& space_;
  BufferPool& buffer_pool_;
  LogManager& log_;
  FreeSpaceMap& fsm_;

  segment_id_t segment_id_ = 0;
  SegmentType segment_type_ = SegmentType::kHeap;
  page_id_t header_page_ = kInvalidPageId;
  std::vector<ExtentEntry> extents_;
  std::unordered_map<extent_id_t, uint32_t> extent_slot_;
  std::vector<page_id_t> overflow_pages_;
  std::unordered_set<page_id_t> special_pages_;
  uint32_t page_count_ = 0;
  uint32_t insert_hint_spno_ = 0;
  uint32_t persisted_insert_hint_ = 0;  // value last written to the header
  uint32_t alloc_hint_slot_ = 0;  // where the last page allocation succeeded
};

}  // namespace minidb
