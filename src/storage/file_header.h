// Per-relation file header (page 0), design doc 5.3 adapted to a folder-based
// database. Every relation owns one data file and one FSM file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/status.h"
#include "common/types.h"
#include "storage/page.h"

namespace minidb {

enum class FileKind : uint32_t {
  kData = 1,  // relation data: page 1 is the extent bitmap
  kFsm = 2,   // free-space map: pages 1..N hold FSM entries
};

constexpr char kFileMagic[8] = {'M', 'I', 'N', 'I', 'D',
                                'B', 'F', '2'};

struct FileHeader {
  char magic[8] = {'M', 'I', 'N', 'I', 'D', 'B', 'F', '2'};
  uint32_t format_version = 2;
  uint32_t page_size = kPageSize;
  uint32_t pages_per_extent = kPagesPerExtent;
  uint32_t file_extents = 1;
  uint32_t file_kind = static_cast<uint32_t>(FileKind::kData);
  uint32_t table_id = 0;
  page_id_t segment_header_page = kInvalidPageId;
  uint32_t fsm_page_count = 0;
  uint8_t clean_shutdown = 0;

  FileKind kind() const { return static_cast<FileKind>(file_kind); }

  void EncodeInto(std::byte* page) const {
    std::memcpy(page + 32, magic, sizeof(magic));
    WriteU32(page, 40, format_version);
    WriteU32(page, 44, page_size);
    WriteU32(page, 48, pages_per_extent);
    WriteU32(page, 52, file_extents);
    WriteU32(page, 56, file_kind);
    WriteU32(page, 60, table_id);
    WriteU32(page, 64, segment_header_page);
    WriteU32(page, 68, fsm_page_count);
    WriteU8(page, 72, clean_shutdown);
  }

  static Result<FileHeader> DecodeFrom(const std::byte* page) {
    FileHeader header;
    std::memcpy(header.magic, page + 32, sizeof(header.magic));
    if (std::memcmp(header.magic, kFileMagic, sizeof(kFileMagic)) != 0) {
      return Status(StatusCode::kCorruption, "bad MiniDB magic in file header");
    }
    header.format_version = ReadU32(page, 40);
    header.page_size = ReadU32(page, 44);
    header.pages_per_extent = ReadU32(page, 48);
    header.file_extents = ReadU32(page, 52);
    header.file_kind = ReadU32(page, 56);
    header.table_id = ReadU32(page, 60);
    header.segment_header_page = ReadU32(page, 64);
    header.fsm_page_count = ReadU32(page, 68);
    header.clean_shutdown = ReadU8(page, 72);

    if (header.format_version != 2) {
      return Status(StatusCode::kNotSupported,
                    "unsupported MiniDB format version " +
                        std::to_string(header.format_version));
    }
    if (header.page_size != kPageSize ||
        header.pages_per_extent != kPagesPerExtent) {
      return Status(StatusCode::kCorruption,
                    "page/extent size mismatch in file header");
    }
    if (header.kind() != FileKind::kData && header.kind() != FileKind::kFsm) {
      return Status(StatusCode::kCorruption, "unknown relation file kind");
    }
    if (header.file_extents == 0) {
      return Status(StatusCode::kCorruption, "relation file has no extents");
    }
    return header;
  }
};

}  // namespace minidb
