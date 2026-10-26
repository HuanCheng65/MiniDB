// A relation's free-space map, stored in its own <relation>.fsm file.
//
// The FSM file has a normal file header at page 0. Pages 1..N hold an array of
// one-byte categories (free bytes / 32, capped at 254) per data-page "spno".
// The entry for spno lives at page 1 + spno / 8160 and offset spno % 8160.
#pragma once

#include <cstdint>
#include <memory>

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
  }

  static uint32_t PageIndexOf(uint32_t spno) { return spno / kEntriesPerPage; }
  static uint32_t EntryOffset(uint32_t spno) { return spno % kEntriesPerPage; }

  uint32_t page_count() const { return page_count_; }
  uint32_t capacity() const { return page_count_ * kEntriesPerPage; }

  // Makes sure the FSM has an initialized entry page covering spno.
  Status EnsureCapacity(uint32_t spno);
  // Reads/writes one category byte; Use ReadPage/WritePage for a whole page.
  Status Update(uint32_t spno, uint8_t category);

  Result<ReadPageGuard> ReadPage(uint32_t page_index);
  Result<WritePageGuard> WritePage(uint32_t page_index);

  const std::shared_ptr<DiskManager>& disk() const { return disk_; }

 private:
  std::shared_ptr<DiskManager> disk_;
  BufferPool& buffer_pool_;
  uint32_t page_count_ = 0;
};

}  // namespace minidb
