#include "table/table_heap.h"

#include <cstring>

#include "common/logger.h"
#include "table/heap_page.h"

namespace minidb {

Result<RID> TableHeap::Insert(Transaction* txn, const Tuple& tuple) {
  const std::vector<std::byte>& record = tuple.data();
  if (record.empty() || record.size() > HeapPage::kMaxRecordLength) {
    return Status(StatusCode::kInvalidArgument,
                  "tuple is too large for a heap page");
  }
  const uint16_t length = static_cast<uint16_t>(record.size());
  const uint32_t need = static_cast<uint32_t>(length) + 4;
  const segment_id_t segment_id = segment_->segment_id();

  for (int attempt = 0; attempt < 128; ++attempt) {
    auto found = segment_->FindPageWithSpace(need);
    if (!found.ok()) return found.status();
    page_id_t page_id = found.value();

    if (page_id == kInvalidPageId) {
      auto allocated = segment_->AllocatePage(txn);
      if (!allocated.ok()) return allocated.status();
      page_id = allocated.value();

      uint32_t free_space = 0;
      {
        auto guard_result = buffer_pool_.NewPage(segment_->data_disk(), page_id, PageType::kHeap,
                                                 segment_id);
        if (!guard_result.ok()) return guard_result.status();
        WritePageGuard guard = guard_result.TakeValue();
        HeapPage::Init(guard.MutableData(), page_id, segment_id);
        auto slot = HeapPage::Insert(guard.MutableData(), record.data(), length);
        if (!slot.ok()) {
          // Should be impossible on a fresh page.
          uint32_t free = HeapPage::FreeSpace(guard.Data());
          (void)free;
          return slot.status();
        }
        free_space = HeapPage::FreeSpace(guard.Data());
        const RID rid{page_id, slot.value()};
        guard = WritePageGuard();  // UNPIN before touching the FSM page
        segment_->UpdateFreeSpace(page_id, free_space);
        return rid;
      }
    }

    auto guard_result = buffer_pool_.FetchWrite(segment_->data_disk(), page_id);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    std::byte* page = guard.MutableData();
    if (GetPageType(page) != PageType::kHeap) {
      return Status(StatusCode::kCorruption, "segment page is not a heap page");
    }
    auto slot = HeapPage::Insert(page, record.data(), length);
    if (slot.ok()) {
      const RID rid{page_id, slot.value()};
      const uint32_t free_space = HeapPage::FreeSpace(page);
      guard = WritePageGuard();
      segment_->UpdateFreeSpace(page_id, free_space);
      MINIDB_LOG_DEBUG("HEAP") << "INSERT rid=(" << rid.page_id << ","
                               << rid.slot << ") len=" << length;
      return rid;
    }

    // FSM was optimistic. Correct it with the real value and keep searching.
    const uint32_t actual_free = HeapPage::FreeSpace(page);
    guard = WritePageGuard();
    segment_->UpdateFreeSpace(page_id, actual_free);
  }
  return Status(StatusCode::kNoSpace, "could not find room for the tuple");
}

Result<RID> TableHeap::ResolveForward(RID rid, int* hops) {
  *hops = 0;
  for (int i = 0; i < 8; ++i) {
    auto guard_result = buffer_pool_.FetchRead(segment_->data_disk(), rid.page_id);
    if (!guard_result.ok()) return guard_result.status();
    ReadPageGuard guard = guard_result.TakeValue();
    const std::byte* page = guard.Data();
    if (GetPageType(page) != PageType::kHeap) {
      return Status(StatusCode::kCorruption, "RID does not point to a heap page");
    }
    HeapSlot slot;
    const Status status = HeapPage::GetSlotChecked(page, rid.slot, &slot);
    if (!status.ok()) return status;
    if (!slot.forward()) {
      *hops = i;
      return rid;
    }
    auto target = HeapPage::ReadForwardTarget(page, rid.slot);
    if (!target.ok()) return target.status();
    rid = target.value();
  }
  return Status(StatusCode::kCorruption, "forwarding chain is too long");
}

Result<Tuple> TableHeap::Get(Transaction* /*txn*/, RID rid) {
  int hops = 0;
  auto resolved = ResolveForward(rid, &hops);
  if (!resolved.ok()) return resolved.status();

  auto guard_result = buffer_pool_.FetchRead(segment_->data_disk(), resolved.value().page_id);
  if (!guard_result.ok()) return guard_result.status();
  ReadPageGuard guard = guard_result.TakeValue();
  auto record = HeapPage::ReadRecord(guard.Data(), resolved.value().slot);
  if (!record.ok()) return record.status();
  return Tuple::FromRaw(record.TakeValue());
}

Status TableHeap::DeleteAt(Transaction* txn, RID rid) {
  uint32_t free_space = 0;
  bool empty = false;
  {
    auto guard_result = buffer_pool_.FetchWrite(segment_->data_disk(), rid.page_id);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    std::byte* page = guard.MutableData();
    const Status status = HeapPage::Delete(page, rid.slot);
    if (!status.ok()) return status;
    free_space = HeapPage::FreeSpace(page);
    empty = HeapPage::LiveCount(page) == 0;
  }
  segment_->UpdateFreeSpace(rid.page_id, free_space);
  if (empty) {
    const Status status = segment_->FreePage(txn, rid.page_id);
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

Status TableHeap::Delete(Transaction* txn, RID rid) {
  int hops = 0;
  auto resolved = ResolveForward(rid, &hops);
  if (!resolved.ok()) return resolved.status();
  if (hops > 0) {
    const Status status = DeleteAt(txn, resolved.value());
    if (!status.ok()) return status;
  }
  return DeleteAt(txn, rid);
}

Result<RID> TableHeap::Update(Transaction* txn, RID rid, const Tuple& tuple) {
  if (tuple.data().empty() || tuple.data().size() > HeapPage::kMaxRecordLength) {
    return Status(StatusCode::kInvalidArgument, "tuple is too large");
  }
  int hops = 0;
  auto resolved = ResolveForward(rid, &hops);
  if (!resolved.ok()) return resolved.status();
  const RID target = resolved.value();

  // Try an in-place update first.
  bool in_place = false;
  uint32_t free_space = 0;
  {
    auto guard_result = buffer_pool_.FetchWrite(segment_->data_disk(), target.page_id);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    std::byte* page = guard.MutableData();
    HeapSlot slot;
    const Status status = HeapPage::GetSlotChecked(page, target.slot, &slot);
    if (!status.ok()) return status;
    if (tuple.data().size() <= slot.length) {
      const Status update = HeapPage::UpdateInPlace(
          page, target.slot, tuple.data().data(),
          static_cast<uint16_t>(tuple.data().size()));
      if (!update.ok()) return update;
      free_space = HeapPage::FreeSpace(page);
      in_place = true;
    }
  }
  if (in_place) {
    segment_->UpdateFreeSpace(target.page_id, free_space);
    return rid;
  }

  // Grow: insert a new version and leave a forwarding pointer behind.
  auto inserted = Insert(txn, tuple);
  if (!inserted.ok()) return inserted.status();
  const RID new_rid = inserted.value();

  if (target == rid) {
    auto guard_result = buffer_pool_.FetchWrite(segment_->data_disk(), rid.page_id);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    std::byte* page = guard.MutableData();
    const Status status = HeapPage::SetForward(page, rid.slot, new_rid);
    if (!status.ok()) return status;
    free_space = HeapPage::FreeSpace(page);
    guard = WritePageGuard();
    segment_->UpdateFreeSpace(rid.page_id, free_space);
  } else {
    // rid -> target -> new_rid, collapsing to rid -> new_rid.
    const Status deleted = DeleteAt(txn, target);
    if (!deleted.ok()) return deleted;
    auto guard_result = buffer_pool_.FetchWrite(segment_->data_disk(), rid.page_id);
    if (!guard_result.ok()) return guard_result.status();
    WritePageGuard guard = guard_result.TakeValue();
    std::byte* page = guard.MutableData();
    const Status status = HeapPage::SetForward(page, rid.slot, new_rid);
    if (!status.ok()) return status;
    free_space = HeapPage::FreeSpace(page);
    guard = WritePageGuard();
    segment_->UpdateFreeSpace(rid.page_id, free_space);
  }
  MINIDB_LOG_DEBUG("HEAP") << "UPDATE rid=(" << rid.page_id << "," << rid.slot
                           << ") -> FORWARD (" << new_rid.page_id << ","
                           << new_rid.slot << ")";
  return rid;
}

TableHeap::Iterator TableHeap::Scan(Transaction* /*txn*/) {
  Iterator iterator;
  iterator.heap_ = this;
  iterator.pages_ = segment_->AllocatedPages();
  iterator.page_index_ = 0;
  iterator.next_slot_ = 0;
  return iterator;
}

void TableHeap::Iterator::Reset() {
  guard_.reset();
  cached_page_ = kInvalidPageId;
}

bool TableHeap::Iterator::Next(RID* rid, Tuple* tuple) {
  while (page_index_ < pages_.size()) {
    const page_id_t page_id = pages_[page_index_];
    if (!guard_.has_value() || cached_page_ != page_id) {
      Reset();
      auto guard_result =
          heap_->buffer_pool_.FetchRead(heap_->disk(), page_id);
      if (!guard_result.ok()) {
        ++page_index_;
        next_slot_ = 0;
        continue;
      }
      guard_ = guard_result.TakeValue();
      cached_page_ = page_id;
    }

    const std::byte* page = guard_->Data();
    if (GetPageType(page) != PageType::kHeap) {
      Reset();
      ++page_index_;
      next_slot_ = 0;
      continue;
    }

    const uint16_t slot_count = HeapPage::SlotCount(page);
    while (next_slot_ < slot_count) {
      const slot_id_t slot = next_slot_++;
      const HeapSlot current = HeapPage::GetSlot(page, slot);
      if (current.empty() || current.moved_in()) continue;

      if (current.forward()) {
        auto target = HeapPage::ReadForwardTarget(page, slot);
        if (!target.ok()) continue;
        const RID original{page_id, slot};
        Reset();  // release this page before fetching the forwarded record
        auto record = heap_->Get(nullptr, target.value());
        if (!record.ok()) continue;
        *rid = original;
        *tuple = record.TakeValue();
        return true;
      }

      auto record = HeapPage::ReadRecord(page, slot);
      if (!record.ok()) continue;
      *rid = RID{page_id, slot};
      *tuple = Tuple::FromRaw(record.TakeValue());
      return true;
    }

    Reset();
    ++page_index_;
    next_slot_ = 0;
  }
  return false;
}

}  // namespace minidb
