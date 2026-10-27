#include "table/heap_page.h"

#include <algorithm>
#include <cstring>

namespace minidb {
namespace {

struct LiveRecord {
  slot_id_t slot;
  uint16_t offset;
  uint16_t length;
};

}  // namespace

void HeapPage::Init(std::byte* page, page_id_t page_id,
                    segment_id_t segment_id) {
  InitPageHeader(page, PageType::kHeap, page_id, segment_id);
  WriteU16(page, kSlotCountOffset, 0);
  WriteU16(page, kFreeLowerOffset, static_cast<uint16_t>(kSlotArrayOffset));
  WriteU16(page, kFreeUpperOffset, static_cast<uint16_t>(kPageSize));
  WriteU16(page, kFragBytesOffset, 0);
  WriteU16(page, kLiveCountOffset, 0);
  WriteU16(page, kLiveCountOffset + 2, 0);
}

HeapSlot HeapPage::GetSlot(const std::byte* page, slot_id_t slot) {
  HeapSlot result;
  if (slot >= SlotCount(page)) return result;
  const uint32_t raw = ReadU32(page, kSlotArrayOffset + slot * 4);
  result.offset = static_cast<uint16_t>(raw & 0xFFFFu);
  result.length = static_cast<uint16_t>((raw >> 16) & 0x1FFFu);
  result.flags = static_cast<uint8_t>((raw >> 29) & 0x7u);
  return result;
}

Status HeapPage::GetSlotChecked(const std::byte* page, slot_id_t slot,
                                HeapSlot* out) {
  if (slot >= SlotCount(page)) {
    return Status(StatusCode::kNotFound, "slot out of range");
  }
  *out = GetSlot(page, slot);
  if (out->empty()) {
    return Status(StatusCode::kNotFound, "slot is empty");
  }
  if (!out->forward() &&
      (static_cast<uint32_t>(out->offset) + out->length > kPageSize ||
       out->offset < kSlotArrayOffset)) {
    return Status(StatusCode::kCorruption, "slot points outside page body");
  }
  return Status::Ok();
}

void HeapPage::WriteSlot(std::byte* page, slot_id_t slot, const HeapSlot& s) {
  const uint32_t raw = (static_cast<uint32_t>(s.offset) & 0xFFFFu) |
                       ((static_cast<uint32_t>(s.length) & 0x1FFFu) << 16) |
                       ((static_cast<uint32_t>(s.flags) & 0x7u) << 29);
  WriteU32(page, kSlotArrayOffset + slot * 4, raw);
}

Result<slot_id_t> HeapPage::Insert(std::byte* page, const std::byte* record,
                                   uint16_t length, uint8_t flags) {
  if (length == 0 || length > kMaxRecordLength) {
    return Status(StatusCode::kInvalidArgument,
                  "record length out of range for heap page");
  }
  uint16_t count = SlotCount(page);
  uint16_t lower = ReadU16(page, kFreeLowerOffset);
  uint16_t upper = ReadU16(page, kFreeUpperOffset);
  uint16_t frag = ReadU16(page, kFragBytesOffset);
  uint16_t live = ReadU16(page, kLiveCountOffset);

  slot_id_t slot = count;
  bool reuse = false;
  for (slot_id_t i = 0; i < count; ++i) {
    if (GetSlot(page, i).empty()) {
      slot = i;
      reuse = true;
      break;
    }
  }

  uint32_t need = length + (reuse ? 0 : 4);
  uint32_t continuous = static_cast<uint32_t>(upper) - lower;
  if (continuous < need) {
    if (continuous + frag < need) {
      return Status(StatusCode::kNoSpace, "heap page has no space");
    }
    Compact(page);
    count = SlotCount(page);
    lower = ReadU16(page, kFreeLowerOffset);
    upper = ReadU16(page, kFreeUpperOffset);
    frag = ReadU16(page, kFragBytesOffset);
    live = ReadU16(page, kLiveCountOffset);
    continuous = static_cast<uint32_t>(upper) - lower;
    if (continuous < need) {
      return Status(StatusCode::kNoSpace, "heap page has no space after compaction");
    }
  }

  const uint16_t offset = static_cast<uint16_t>(upper - length);
  std::memcpy(page + offset, record, length);

  if (!reuse) {
    slot = count;
    WriteU16(page, kSlotCountOffset, static_cast<uint16_t>(count + 1));
    lower = static_cast<uint16_t>(lower + 4);
    WriteU16(page, kFreeLowerOffset, lower);
  }
  WriteU16(page, kFreeUpperOffset, offset);
  WriteU16(page, kFragBytesOffset, frag);
  WriteU16(page, kLiveCountOffset, static_cast<uint16_t>(live + 1));
  WriteSlot(page, slot, HeapSlot{offset, length, flags});
  return slot;
}

Status HeapPage::Delete(std::byte* page, slot_id_t slot) {
  HeapSlot current;
  const Status status = GetSlotChecked(page, slot, &current);
  if (!status.ok()) return status;
  uint16_t frag = ReadU16(page, kFragBytesOffset);
  uint16_t live = ReadU16(page, kLiveCountOffset);
  WriteU16(page, kFragBytesOffset,
           static_cast<uint16_t>(frag + current.length));
  if (live > 0) WriteU16(page, kLiveCountOffset, static_cast<uint16_t>(live - 1));
  WriteSlot(page, slot, HeapSlot{});
  return Status::Ok();
}

Status HeapPage::UpdateInPlace(std::byte* page, slot_id_t slot,
                               const std::byte* record, uint16_t length) {
  HeapSlot current;
  const Status status = GetSlotChecked(page, slot, &current);
  if (!status.ok()) return status;
  if (current.forward()) {
    return Status(StatusCode::kInvalidArgument, "cannot update a forward slot");
  }
  if (length > current.length) return Status(StatusCode::kNoSpace, "record grew");
  std::memcpy(page + current.offset, record, length);
  uint16_t frag = ReadU16(page, kFragBytesOffset);
  WriteU16(page, kFragBytesOffset,
           static_cast<uint16_t>(frag + (current.length - length)));
  WriteSlot(page, slot, HeapSlot{current.offset, length, current.flags});
  return Status::Ok();
}

Status HeapPage::Replace(std::byte* page, slot_id_t slot,
                         const std::byte* record, uint16_t length) {
  if (length == 0 || length > kMaxRecordLength) {
    return Status(StatusCode::kInvalidArgument,
                  "record length out of range for heap page");
  }
  HeapSlot current;
  const Status status = GetSlotChecked(page, slot, &current);
  if (!status.ok()) return status;
  if (current.forward()) {
    return Status(StatusCode::kInvalidArgument, "cannot replace a forward slot");
  }
  if (length <= current.length) {
    return UpdateInPlace(page, slot, record, length);
  }
  if (FreeSpace(page) + current.length < length) {
    return Status(StatusCode::kNoSpace, "heap page has no space for update");
  }

  // Release the old bytes first so Compact() reclaims them.
  const uint16_t frag = ReadU16(page, kFragBytesOffset);
  WriteU16(page, kFragBytesOffset, static_cast<uint16_t>(frag + current.length));
  WriteSlot(page, slot, HeapSlot{});
  uint16_t lower = ReadU16(page, kFreeLowerOffset);
  uint16_t upper = ReadU16(page, kFreeUpperOffset);
  if (static_cast<uint32_t>(upper) - lower < length) {
    Compact(page);
    upper = ReadU16(page, kFreeUpperOffset);
  }
  const uint16_t offset = static_cast<uint16_t>(upper - length);
  std::memcpy(page + offset, record, length);
  WriteU16(page, kFreeUpperOffset, offset);
  WriteSlot(page, slot, HeapSlot{offset, length, current.flags});
  return Status::Ok();
}

Result<std::vector<std::byte>> HeapPage::ReadRecord(const std::byte* page,
                                                    slot_id_t slot) {
  HeapSlot current;
  const Status status = GetSlotChecked(page, slot, &current);
  if (!status.ok()) return status;
  if (current.forward()) {
    return Status(StatusCode::kInvalidArgument,
                  "slot contains a forwarding pointer");
  }
  std::vector<std::byte> record(current.length);
  std::memcpy(record.data(), page + current.offset, current.length);
  return record;
}

Result<RID> HeapPage::ReadForwardTarget(const std::byte* page, slot_id_t slot) {
  HeapSlot current;
  const Status status = GetSlotChecked(page, slot, &current);
  if (!status.ok()) return status;
  if (!current.forward() || current.length < 6) {
    return Status(StatusCode::kInvalidArgument, "slot is not forwarded");
  }
  RID target;
  target.page_id = ReadU32(page, current.offset);
  target.slot = ReadU16(page, current.offset + 4);
  return target;
}

Status HeapPage::SetForward(std::byte* page, slot_id_t slot, RID target) {
  HeapSlot current;
  const Status status = GetSlotChecked(page, slot, &current);
  if (!status.ok()) return status;
  if (current.forward()) {
    // Redirecting an existing forward pointer keeps the chain at one hop.
    WriteU32(page, current.offset, target.page_id);
    WriteU16(page, current.offset + 4, target.slot);
    return Status::Ok();
  }
  if (current.length < 6) {
    return Status(StatusCode::kInternal, "record too small for forward pointer");
  }
  WriteU32(page, current.offset, target.page_id);
  WriteU16(page, current.offset + 4, target.slot);
  uint16_t frag = ReadU16(page, kFragBytesOffset);
  WriteU16(page, kFragBytesOffset,
           static_cast<uint16_t>(frag + (current.length - 6)));
  WriteSlot(page, slot, HeapSlot{current.offset, 6, kSlotForward});
  return Status::Ok();
}

void HeapPage::Compact(std::byte* page) {
  const uint16_t count = SlotCount(page);
  std::vector<LiveRecord> records;
  records.reserve(count);
  for (slot_id_t i = 0; i < count; ++i) {
    const HeapSlot s = GetSlot(page, i);
    if (!s.live()) continue;
    records.push_back(LiveRecord{i, s.offset, s.length});
  }
  std::sort(records.begin(), records.end(),
            [](const LiveRecord& a, const LiveRecord& b) {
              return a.offset > b.offset;  // highest first avoids clobbering
            });

  uint16_t write_pos = static_cast<uint16_t>(kPageSize);
  for (const LiveRecord& record : records) {
    write_pos = static_cast<uint16_t>(write_pos - record.length);
    std::memmove(page + write_pos, page + record.offset, record.length);
    HeapSlot s = GetSlot(page, record.slot);
    s.offset = write_pos;
    WriteSlot(page, record.slot, s);
  }
  WriteU16(page, kFreeUpperOffset, write_pos);
  WriteU16(page, kFragBytesOffset, 0);
}

}  // namespace minidb
