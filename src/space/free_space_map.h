// A relation's free-space map, stored in its own <relation>.fsm file.
//
// The FSM file has a normal file header at page 0. Pages 1..N hold an array of
// one-byte categories (free bytes / 32, capped at 254) per data-page "spno".
// The entry for spno lives at page 1 + spno / 8160 and offset spno % 8160.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "buffer/buffer_pool.h"
#include "common/status.h"
#include "storage/disk_manager.h"

namespace minidb {

class FreeSpaceMap {
 public:
  static constexpr uint32_t kEntriesPerPage = kPageSize - kPageHeaderSize;

  FreeSpaceMap(std::shared_ptr<DiskManager> disk, BufferPool& buffer_pool)
      : disk_(std::move(disk)), buffer_pool_(buffer_pool) {
    page_count_ = disk_->header().fsm_page_count;
    // Unknown after open: the first full scan of each page fills these in.
    max_hint_.assign(page_count_, kUnknownMax);
  }

  static uint32_t PageIndexOf(uint32_t spno) { return spno / kEntriesPerPage; }
  static uint32_t EntryOffset(uint32_t spno) { return spno % kEntriesPerPage; }

  uint32_t page_count() const { return page_count_; }
  uint32_t capacity() const { return page_count_ * kEntriesPerPage; }

  // Makes sure the FSM has an initialized entry page covering spno.
  Status EnsureCapacity(uint32_t spno);
  // Reads/writes one category byte; Use ReadPage/WritePage for a whole page.
  Status Update(uint32_t spno, uint8_t category);
  // Reads one category byte; 0 when spno is beyond the initialized pages.
  Result<uint8_t> Get(uint32_t spno);

  // In-memory upper bound of the largest category on an FSM page; searches
  // skip pages whose bound is below what they need. Not persisted.
  uint8_t MaxCategoryHint(uint32_t page_index) const {
    return page_index < max_hint_.size() ? max_hint_[page_index] : kUnknownMax;
  }
  void SetMaxCategoryHint(uint32_t page_index, uint8_t value) {
    if (page_index < max_hint_.size()) max_hint_[page_index] = value;
  }

  // An all-zero FSM page (e.g. allocated but never flushed before a crash) is
  // accepted as "no free space recorded"; FSM contents are only hints.
  Result<ReadPageGuard> ReadPage(uint32_t page_index);
  Result<WritePageGuard> WritePage(uint32_t page_index);

  const std::shared_ptr<DiskManager>& disk() const { return disk_; }

 private:
  std::shared_ptr<DiskManager> disk_;
  BufferPool& buffer_pool_;
  static constexpr uint8_t kUnknownMax = 255;

  uint32_t page_count_ = 0;
  std::vector<uint8_t> max_hint_;
};

}  // namespace minidb
