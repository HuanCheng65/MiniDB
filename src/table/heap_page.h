// Slotted heap page (design doc 5.7, 7.4).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/status.h"
#include "common/types.h"
#include "storage/page.h"

namespace minidb {

struct HeapSlot {
  uint16_t offset = 0;
  uint16_t length = 0;
  uint8_t flags = 0;

  bool empty() const { return flags == 0; }
  bool used() const { return (flags & 0x1) != 0; }
  bool forward() const { return (flags & 0x2) != 0; }
  bool moved_in() const { return (flags & 0x4) != 0; }
  bool live() const { return flags != 0; }
};

class HeapPage {
 public:
  static constexpr size_t kSlotCountOffset = kPageHeaderSize + 0;
  static constexpr size_t kFreeLowerOffset = kPageHeaderSize + 2;
  static constexpr size_t kFreeUpperOffset = kPageHeaderSize + 4;
  static constexpr size_t kFragBytesOffset = kPageHeaderSize + 6;
  static constexpr size_t kLiveCountOffset = kPageHeaderSize + 8;
  static constexpr size_t kSlotArrayOffset = kPageHeaderSize + 12;
  static constexpr uint16_t kMaxRecordLength = 0x1FFF;

  enum SlotFlags : uint8_t {
    kSlotUsed = 0x1,
    kSlotForward = 0x2,
    kSlotMovedIn = 0x4,
  };

  static void Init(std::byte* page, page_id_t page_id,
                   segment_id_t segment_id);

  static uint16_t SlotCount(const std::byte* page) {
    return ReadU16(page, kSlotCountOffset);
  }
  static uint16_t LiveCount(const std::byte* page) {
    return ReadU16(page, kLiveCountOffset);
  }
  static uint32_t FreeSpace(const std::byte* page) {
    return static_cast<uint32_t>(ReadU16(page, kFreeUpperOffset)) -
           ReadU16(page, kFreeLowerOffset) + ReadU16(page, kFragBytesOffset);
  }

  static HeapSlot GetSlot(const std::byte* page, slot_id_t slot);
  static Status GetSlotChecked(const std::byte* page, slot_id_t slot,
                               HeapSlot* out);

  static Result<slot_id_t> Insert(std::byte* page, const std::byte* record,
                                  uint16_t length, uint8_t flags = kSlotUsed);
  static Status Delete(std::byte* page, slot_id_t slot);
  static Status UpdateInPlace(std::byte* page, slot_id_t slot,
                              const std::byte* record, uint16_t length);
  // Replaces a record with a possibly longer one on the same page, compacting
  // when needed. The slot number and flags are kept (design doc 9.2 step 2).
  static Status Replace(std::byte* page, slot_id_t slot,
                        const std::byte* record, uint16_t length);

  static Result<std::vector<std::byte>> ReadRecord(const std::byte* page,
                                                   slot_id_t slot);
  static Result<RID> ReadForwardTarget(const std::byte* page, slot_id_t slot);
  static Status SetForward(std::byte* page, slot_id_t slot, RID target);

  // Compact live records toward the end of the page. Slot numbers and RIDs do
  // not change (design doc 7.4).
  static void Compact(std::byte* page);

 private:
  static void WriteSlot(std::byte* page, slot_id_t slot, const HeapSlot& s);
};

}  // namespace minidb
