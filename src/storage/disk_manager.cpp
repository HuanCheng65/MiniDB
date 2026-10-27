#include "storage/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include "common/crc32c.h"

namespace minidb {
namespace {

constexpr uint64_t PageOffset(page_id_t page_id) {
  return static_cast<uint64_t>(page_id) * kPageSize;
}

bool AllZero(const std::byte* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != std::byte{0}) return false;
  }
  return true;
}

// CRC32C of a page with its checksum field taken as zero (design doc 5.2),
// computed without copying the page.
uint32_t PageChecksum(const std::byte* page) {
  constexpr std::byte kZero[sizeof(uint32_t)] = {};
  uint32_t crc = Crc32cExtend(0, page, kChecksumOffset);
  crc = Crc32cExtend(crc, kZero, sizeof(kZero));
  const size_t rest = kChecksumOffset + sizeof(uint32_t);
  return Crc32cExtend(crc, page + rest, kPageSize - rest);
}

// Grows a file from old_size to new_size bytes. fallocate reserves real
// blocks so the file is not sparse (less fragmentation, and ENOSPC surfaces
// here rather than at some later page write); filesystems without fallocate
// support fall back to ftruncate.
Status GrowFile(int fd, uint64_t old_size, uint64_t new_size) {
  if (::fallocate(fd, 0, static_cast<off_t>(old_size),
                  static_cast<off_t>(new_size - old_size)) == 0) {
    return Status::Ok();
  }
  if (errno != EOPNOTSUPP && errno != ENOSYS) {
    return Status(StatusCode::kIoError,
                  "fallocate failed: " + std::string(std::strerror(errno)));
  }
  if (::ftruncate(fd, static_cast<off_t>(new_size)) != 0) {
    return Status(StatusCode::kIoError,
                  "ftruncate failed: " + std::string(std::strerror(errno)));
  }
  return Status::Ok();
}

uint32_t NextFileId() {
  static std::atomic<uint32_t> next{1};
  return next.fetch_add(1);
}

}  // namespace

Result<DiskManager::Ptr> DiskManager::Create(const std::string& path,
                                             FileKind kind,
                                             uint32_t table_id) {
  return CreateInternal(path, kind, table_id, /*exclusive=*/true);
}

Result<DiskManager::Ptr> DiskManager::CreateInternal(const std::string& path,
                                                     FileKind kind,
                                                     uint32_t table_id,
                                                     bool exclusive) {
  const std::filesystem::path fs_path(path);
  if (exclusive && std::filesystem::exists(fs_path)) {
    return Status(StatusCode::kAlreadyExists,
                  "relation file already exists: " + path);
  }
  if (fs_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(fs_path.parent_path(), ec);
  }

  const int flags = O_RDWR | O_CREAT | (exclusive ? O_EXCL : 0);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    return Status(StatusCode::kIoError,
                  "cannot create " + path + ": " + std::strerror(errno));
  }

  auto manager =
      std::unique_ptr<DiskManager>(new DiskManager(fd, path, NextFileId()));
  Status status = manager->InitializeNew(kind, table_id);
  if (!status.ok()) {
    manager->fd_ = -1;
    ::close(fd);
    return status;
  }
  return Ptr(manager.release());
}

Result<DiskManager::Ptr> DiskManager::Open(const std::string& path,
                                           FileKind expected_kind) {
  return OpenInternal(path, expected_kind);
}

Result<DiskManager::Ptr> DiskManager::OpenInternal(
    const std::string& path, FileKind expected_kind) {
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return Status(StatusCode::kIoError,
                  "cannot open " + path + ": " + std::strerror(errno));
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status(StatusCode::kIoError, "fstat failed for " + path);
  }
  if (st.st_size < static_cast<off_t>(2 * kPageSize)) {
    ::close(fd);
    return Status(StatusCode::kCorruption,
                  "relation file is too small: " + path);
  }

  auto manager =
      std::unique_ptr<DiskManager>(new DiskManager(fd, path, NextFileId()));
  const Status status = manager->LoadHeader(expected_kind);
  if (!status.ok()) {
    manager->fd_ = -1;
    ::close(fd);
    return status;
  }
  return Ptr(manager.release());
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) ::close(fd_);
}

Status DiskManager::InitializeNew(FileKind kind, uint32_t table_id) {
  const uint64_t size =
      static_cast<uint64_t>(kPagesPerExtent) * kPageSize;
  Status status = GrowFile(fd_, 0, size);
  if (!status.ok()) return status;

  header_ = FileHeader();
  header_.file_extents = 1;
  header_.file_kind = static_cast<uint32_t>(kind);
  header_.table_id = table_id;
  header_.segment_header_page = kInvalidPageId;
  header_.fsm_page_count = 0;
  header_.clean_shutdown = 0;

  std::array<std::byte, kPageSize> page0{};
  InitPageHeader(page0.data(), PageType::kFileHeader, 0, 0);
  header_.EncodeInto(page0.data());
  status = WritePage(0, page0.data());
  if (!status.ok()) return status;

  if (kind == FileKind::kData) {
    std::array<std::byte, kPageSize> bitmap{};
    InitPageHeader(bitmap.data(), PageType::kExtentBitmap, 1, 0);
    WriteU32(bitmap.data(), 32, 1);  // alloc_hint: extent 0 is reserved
    WriteU32(bitmap.data(), 36, 0);  // reserved
    // Extent 0 (header page + bitmap page) is reserved and marked used.
    bitmap[40] = std::byte{0x01};
    status = WritePage(1, bitmap.data());
    if (!status.ok()) return status;
  }
  // FSM page 1 is initialized lazily by FreeSpaceMap::EnsureCapacity().

  return Sync();
}

Status DiskManager::LoadHeader(FileKind expected_kind) {
  std::array<std::byte, kPageSize> page0{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const ssize_t n = ::pread(fd_, page0.data(), kPageSize, 0);
    if (n != static_cast<ssize_t>(kPageSize)) {
      return Status(StatusCode::kIoError, "short read of file header");
    }
  }
  if (PageIsAllZero(page0.data())) {
    return Status(StatusCode::kCorruption,
                  "empty file is not a MiniDB relation file: " + path_);
  }
  const uint32_t stored = ReadU32(page0.data(), kChecksumOffset);
  if (PageChecksum(page0.data()) != stored) {
    return Status(StatusCode::kCorruption, "file header checksum mismatch");
  }
  Result<FileHeader> header = FileHeader::DecodeFrom(page0.data());
  if (!header.ok()) return header.status();
  header_ = header.value();
  if (header_.kind() != expected_kind) {
    return Status(StatusCode::kCorruption,
                  "unexpected relation file kind for " + path_);
  }
  return Status::Ok();
}

Status DiskManager::ReadPage(page_id_t page_id, std::byte* out) {
  if (page_id >= PageCount()) {
    return Status(StatusCode::kInvalidArgument,
                  "page id " + std::to_string(page_id) + " beyond file");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const ssize_t n = ::pread(fd_, out, kPageSize, PageOffset(page_id));
  if (n != static_cast<ssize_t>(kPageSize)) {
    return Status(StatusCode::kIoError,
                  "short read of page " + std::to_string(page_id));
  }
  if (AllZero(out, kPageSize)) return Status::Ok();

  const uint32_t stored = ReadU32(out, kChecksumOffset);
  if (PageChecksum(out) != stored) {
    return Status(StatusCode::kCorruption,
                  "checksum mismatch for page " + std::to_string(page_id));
  }
  if (ReadU32(out, kPageIdOffset) != page_id) {
    return Status(StatusCode::kCorruption,
                  "page id field mismatch for page " + std::to_string(page_id));
  }
  return Status::Ok();
}

Status DiskManager::WritePage(page_id_t page_id, const std::byte* data) {
  if (page_id >= PageCount()) {
    return Status(StatusCode::kInvalidArgument,
                  "write beyond file: page " + std::to_string(page_id));
  }
  alignas(64) std::array<std::byte, kPageSize> out;
  std::memcpy(out.data(), data, kPageSize);
  WriteU32(out.data(), kChecksumOffset, 0);
  WriteU32(out.data(), kChecksumOffset, Crc32c(out.data(), kPageSize));
  std::lock_guard<std::mutex> lock(mutex_);
  const ssize_t n = ::pwrite(fd_, out.data(), kPageSize, PageOffset(page_id));
  if (n != static_cast<ssize_t>(kPageSize)) {
    return Status(StatusCode::kIoError,
                  "short write of page " + std::to_string(page_id) + ": " +
                      std::strerror(errno));
  }
  return Status::Ok();
}

Status DiskManager::ExtendTo(uint32_t extents) {
  if (extents <= header_.file_extents) return Status::Ok();
  const uint64_t size =
      static_cast<uint64_t>(extents) * kPagesPerExtent * kPageSize;
  const uint64_t old_size =
      static_cast<uint64_t>(header_.file_extents) * kPagesPerExtent * kPageSize;
  std::lock_guard<std::mutex> lock(mutex_);
  const Status status = GrowFile(fd_, old_size, size);
  if (!status.ok()) return status;
  header_.file_extents = extents;
  return Status::Ok();
}

Status DiskManager::Sync() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (::fsync(fd_) != 0) {
    return Status(StatusCode::kIoError, "fsync failed: " +
                                            std::string(std::strerror(errno)));
  }
  return Status::Ok();
}

Status DiskManager::PersistHeader() {
  std::array<std::byte, kPageSize> page0{};
  InitPageHeader(page0.data(), PageType::kFileHeader, 0, 0);
  header_.EncodeInto(page0.data());
  return WritePage(0, page0.data());
}

}  // namespace minidb
