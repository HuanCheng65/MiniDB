// Extent allocation via a relation data file's page-1 bitmap.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "buffer/buffer_pool.h"
#include "common/status.h"
#include "common/types.h"
#include "log/log_manager.h"
#include "space/free_space_map.h"
#include "space/segment.h"
#include "storage/disk_manager.h"
#include "txn/transaction.h"

namespace minidb {

struct ExtentBitmapInfo {
  uint32_t alloc_hint = 0;
  std::vector<bool> allocated;  // one entry per extent in the file
};

class SpaceManager {
 public:
  static constexpr page_id_t kBitmapPageId = 1;
  static constexpr size_t kBitmapAllocHintOffset = 32;
  static constexpr size_t kBitmapOffset = 40;

  SpaceManager(std::shared_ptr<DiskManager> disk, BufferPool& buffer_pool,
               LogManager& log)
      : disk_(std::move(disk)), buffer_pool_(buffer_pool), log_(log) {}

  Result<extent_id_t> AllocateExtent(Transaction* txn);
  Status FreeExtent(Transaction* txn, extent_id_t extent);
  // Copy of the extent bitmap for the extents the file currently contains.
  Result<ExtentBitmapInfo> ReadBitmap();

  Result<std::unique_ptr<Segment>> CreateSegment(Transaction* txn,
                                                 segment_id_t segment_id,
                                                 SegmentType type,
                                                 extent_id_t first_extent,
                                                 FreeSpaceMap& fsm);
  Result<std::unique_ptr<Segment>> OpenSegment(page_id_t header_page,
                                               FreeSpaceMap& fsm);

  static size_t BitmapBitCount() {
    return (kPageSize - kBitmapOffset) * 8;
  }

  DiskManager& disk() { return *disk_; }
  const DiskManager::Ptr& disk_ptr() const { return disk_; }
  BufferPool& buffer_pool() { return buffer_pool_; }
  LogManager& log() { return log_; }

 private:
  static bool TestBit(const std::byte* bitmap, size_t bit) {
    return (bitmap[bit / 8] & static_cast<std::byte>(1u << (bit % 8))) !=
           std::byte{0};
  }
  static void SetBit(std::byte* bitmap, size_t bit, bool value) {
    const auto mask = static_cast<std::byte>(1u << (bit % 8));
    if (value) {
      bitmap[bit / 8] |= mask;
    } else {
      bitmap[bit / 8] &= ~mask;
    }
  }

  DiskManager::Ptr disk_;
  BufferPool& buffer_pool_;
  LogManager& log_;
};

}  // namespace minidb
