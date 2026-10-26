// MiniDB common type aliases and constants (design doc chapter 4).
#pragma once

#include <cstdint>
#include <limits>

namespace minidb {

using page_id_t = uint32_t;    // physical page number, 0 .. 2^32 - 2
using frame_id_t = uint32_t;   // buffer pool frame number
using extent_id_t = uint32_t;  // extent number
using segment_id_t = uint32_t;
using slot_id_t = uint16_t;
using txn_id_t = uint64_t;
using lsn_t = uint64_t;        // byte offset in the log file

constexpr page_id_t kInvalidPageId = std::numeric_limits<page_id_t>::max();
constexpr extent_id_t kInvalidExtentId = std::numeric_limits<extent_id_t>::max();
constexpr lsn_t kInvalidLsn = 0;
constexpr uint32_t kPageSize = 8192;
constexpr uint32_t kPagesPerExtent = 64;  // 512 KB extents

// Every page starts with a 32-byte common header (design doc 5.2).
constexpr uint32_t kPageHeaderSize = 32;

struct RID {
  page_id_t page_id = kInvalidPageId;
  slot_id_t slot = 0;

  bool operator==(const RID& other) const {
    return page_id == other.page_id && slot == other.slot;
  }
};

}  // namespace minidb
