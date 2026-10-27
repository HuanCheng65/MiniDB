// DiskManager: page I/O for one relation file (data or FSM).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "common/status.h"
#include "common/types.h"
#include "storage/file_header.h"

namespace minidb {

class DiskManager {
 public:
  using Ptr = std::shared_ptr<DiskManager>;

  // Creates a brand-new file; fails if the path already exists.
  static Result<Ptr> Create(const std::string& path, FileKind kind,
                            uint32_t table_id);
  // Opens an existing relation file and verifies its kind.
  static Result<Ptr> Open(const std::string& path, FileKind expected_kind);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Verifies checksum and page_id. An all-zero page is accepted as an
  // "allocated but never written" page (design doc 5.2).
  Status ReadPage(page_id_t page_id, std::byte* out);
  // Writes a checksummed copy; the caller's buffer is not modified, so a
  // frame can be flushed without exclusive access to its bytes.
  Status WritePage(page_id_t page_id, const std::byte* data);

  Status ExtendTo(uint32_t extents);
  Status Sync();

  // Cached page 0. Call PersistHeader() after modifying it.
  FileHeader& header() { return header_; }
  const FileHeader& header() const { return header_; }
  Status PersistHeader();

  uint32_t PageCount() const { return header_.file_extents * kPagesPerExtent; }
  const std::string& path() const { return path_; }
  // File name without the database folder, for logs and PRAGMA output.
  const std::string& name() const { return name_; }
  // Process-wide identity used by the shared buffer pool.
  uint32_t file_id() const { return file_id_; }
  FileKind kind() const { return header_.kind(); }

 private:
  DiskManager(int fd, std::string path, uint32_t file_id)
      : fd_(fd),
        path_(std::move(path)),
        name_(std::filesystem::path(path_).filename().string()),
        file_id_(file_id) {}

  static Result<Ptr> CreateInternal(const std::string& path, FileKind kind,
                                    uint32_t table_id, bool exclusive);
  static Result<Ptr> OpenInternal(const std::string& path,
                                  FileKind expected_kind);
  Status InitializeNew(FileKind kind, uint32_t table_id);
  Status LoadHeader(FileKind expected_kind);

  int fd_ = -1;
  std::string path_;
  std::string name_;
  uint32_t file_id_ = 0;
  FileHeader header_;
  std::mutex mutex_;
};

}  // namespace minidb
