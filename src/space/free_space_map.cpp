#include "space/free_space_map.h"

#include <algorithm>

#include "common/logger.h"

namespace minidb {

Status FreeSpaceMap::EnsureCapacity(uint32_t spno) {
  const uint32_t needed = PageIndexOf(spno) + 1;
  if (needed <= page_count_) return Status::Ok();
  if (disk_ == nullptr || disk_->kind() != FileKind::kFsm) {
    return Status(StatusCode::kInternal, "invalid FSM file");
  }

  // Entry page i lives at physical page 1 + i, so the file must contain at
  // least page index 1 + needed - 1 = needed.
  const uint32_t required_pages = needed + 1;
  if (disk_->PageCount() < required_pages) {
    const uint32_t extents =
        (required_pages + kPagesPerExtent - 1) / kPagesPerExtent;
    const Status status = disk_->ExtendTo(extents);
    if (!status.ok()) return status;
    const Status persist = disk_->PersistHeader();
    if (!persist.ok()) return persist;
  }

  for (uint32_t index = page_count_; index < needed; ++index) {
    auto page = buffer_pool_.NewPage(disk_, 1 + index, PageType::kFsm,
                                     disk_->header().table_id);
    if (!page.ok()) return page.status();
  }
  page_count_ = needed;
  max_hint_.resize(needed, 0);  // fresh pages are all zero
  disk_->header().fsm_page_count = needed;
  const Status persist = disk_->PersistHeader();
  if (!persist.ok()) return persist;
  MINIDB_LOG_DEBUG("FSM") << "file=" << disk_->name()
                          << " capacity=" << capacity();
  return Status::Ok();
}

Status FreeSpaceMap::Update(uint32_t spno, uint8_t category) {
  const Status capacity = EnsureCapacity(spno);
  if (!capacity.ok()) return capacity;
  auto page = WritePage(PageIndexOf(spno));
  if (!page.ok()) return page.status();
  WritePageGuard guard = page.TakeValue();
  const uint8_t previous =
      ReadU8(guard.Data(), kPageHeaderSize + EntryOffset(spno));
  if (previous == category) return Status::Ok();
  WriteU8(guard.MutableData(), kPageHeaderSize + EntryOffset(spno), category);
  MINIDB_LOG_DEBUG("FSM") << "file=" << disk_->name() << " spno=" << spno
                          << " cat " << static_cast<int>(previous) << " -> "
                          << static_cast<int>(category);
  const uint32_t page_index = PageIndexOf(spno);
  if (max_hint_[page_index] != kUnknownMax &&
      category > max_hint_[page_index]) {
    max_hint_[page_index] = category;
  }
  return Status::Ok();
}

Result<uint8_t> FreeSpaceMap::Get(uint32_t spno) {
  if (PageIndexOf(spno) >= page_count_) return static_cast<uint8_t>(0);
  auto page = ReadPage(PageIndexOf(spno));
  if (!page.ok()) return page.status();
  return ReadU8(page.value().Data(), kPageHeaderSize + EntryOffset(spno));
}

Result<ReadPageGuard> FreeSpaceMap::ReadPage(uint32_t page_index) {
  if (page_index >= page_count_) {
    return Status(StatusCode::kNotFound, "FSM page is not initialized");
  }
  auto guard = buffer_pool_.FetchRead(disk_, 1 + page_index);
  if (!guard.ok()) return guard.status();
  const std::byte* data = guard.value().Data();
  if (GetPageType(data) != PageType::kFsm && !PageIsAllZero(data)) {
    return Status(StatusCode::kCorruption, "bad FSM page type");
  }
  return guard;
}

Result<WritePageGuard> FreeSpaceMap::WritePage(uint32_t page_index) {
  if (page_index >= page_count_) {
    return Status(StatusCode::kNotFound, "FSM page is not initialized");
  }
  auto guard = buffer_pool_.FetchWrite(disk_, 1 + page_index);
  if (!guard.ok()) return guard.status();
  const std::byte* data = guard.value().Data();
  if (GetPageType(data) != PageType::kFsm) {
    if (!PageIsAllZero(data)) {
      return Status(StatusCode::kCorruption, "bad FSM page type");
    }
    MINIDB_LOG_WARN("FSM") << "file=" << disk_->name() << " FSM page "
                           << page_index << " was never written; reinitializing";
    InitPageHeader(guard.value().MutableData(), PageType::kFsm, 1 + page_index,
                   disk_->header().table_id);
  }
  return guard;
}

}  // namespace minidb
