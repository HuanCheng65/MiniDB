// Common page header and little-endian accessors (design doc 5.2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace minidb {

enum class PageType : uint16_t {
  kInvalid = 0,
  kFileHeader = 1,
  kExtentBitmap = 2,
  kSegmentHeader = 3,
  kExtentList = 4,
  kFsm = 5,
  kHeap = 6,
  kBtreeInternal = 7,
  kBtreeLeaf = 8,
};

constexpr size_t kChecksumOffset = 0;
constexpr size_t kPageTypeOffset = 4;
constexpr size_t kFlagsOffset = 6;
constexpr size_t kPageIdOffset = 8;
constexpr size_t kSegmentIdOffset = 12;
constexpr size_t kPageLsnOffset = 16;
constexpr size_t kReservedOffset = 24;

inline uint8_t ReadU8(const std::byte* page, size_t offset) {
  return std::to_integer<uint8_t>(page[offset]);
}

inline void WriteU8(std::byte* page, size_t offset, uint8_t value) {
  page[offset] = static_cast<std::byte>(value);
}

inline uint16_t ReadU16(const std::byte* page, size_t offset) {
  uint16_t value;
  std::memcpy(&value, page + offset, sizeof(value));
  return value;
}

inline void WriteU16(std::byte* page, size_t offset, uint16_t value) {
  std::memcpy(page + offset, &value, sizeof(value));
}

inline uint32_t ReadU32(const std::byte* page, size_t offset) {
  uint32_t value;
  std::memcpy(&value, page + offset, sizeof(value));
  return value;
}

inline void WriteU32(std::byte* page, size_t offset, uint32_t value) {
  std::memcpy(page + offset, &value, sizeof(value));
}

inline uint64_t ReadU64(const std::byte* page, size_t offset) {
  uint64_t value;
  std::memcpy(&value, page + offset, sizeof(value));
  return value;
}

inline void WriteU64(std::byte* page, size_t offset, uint64_t value) {
  std::memcpy(page + offset, &value, sizeof(value));
}

inline void InitPageHeader(std::byte* page, PageType type, page_id_t page_id,
                           segment_id_t segment_id) {
  WriteU32(page, kChecksumOffset, 0);
  WriteU16(page, kPageTypeOffset, static_cast<uint16_t>(type));
  WriteU16(page, kFlagsOffset, 0);
  WriteU32(page, kPageIdOffset, page_id);
  WriteU32(page, kSegmentIdOffset, segment_id);
  WriteU64(page, kPageLsnOffset, 0);
  WriteU64(page, kReservedOffset, 0);
}

inline PageType GetPageType(const std::byte* page) {
  return static_cast<PageType>(ReadU16(page, kPageTypeOffset));
}

inline page_id_t GetPageId(const std::byte* page) {
  return ReadU32(page, kPageIdOffset);
}

inline segment_id_t GetSegmentId(const std::byte* page) {
  return ReadU32(page, kSegmentIdOffset);
}

inline bool PageIsAllZero(const std::byte* page) {
  for (uint32_t i = 0; i < kPageSize; ++i) {
    if (page[i] != std::byte{0}) return false;
  }
  return true;
}

}  // namespace minidb
