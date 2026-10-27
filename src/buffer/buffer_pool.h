// Buffer pool, CLOCK replacer and RAII page guards (design doc chapter 8).
// Frames are keyed by (relation file id, page id) so one pool serves every
// relation data/FSM file in the database folder.
#pragma once

#include <atomic>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "common/types.h"
#include "log/log_manager.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

namespace minidb {

struct BufferConfig {
  uint32_t frames = 1024;
  std::string replacer = "clock";
};

struct BufferStats {
  uint64_t hit = 0;
  uint64_t miss = 0;
  uint64_t evict = 0;
  uint64_t flush = 0;
  uint64_t new_page = 0;

  double HitRate() const {
    const uint64_t total = hit + miss;
    return total == 0 ? 0.0
                      : 100.0 * static_cast<double>(hit) /
                            static_cast<double>(total);
  }
};

// One occupied frame, as reported by PRAGMA buffer_frames.
struct FrameInfo {
  frame_id_t frame = 0;
  std::string file;  // file name of the relation data/FSM file
  page_id_t page_id = kInvalidPageId;
  PageType page_type = PageType::kInvalid;  // kInvalid when write-latched
  int pin_count = 0;
  bool dirty = false;
};

class Replacer {
 public:
  virtual ~Replacer() = default;
  virtual void RecordAccess(frame_id_t frame) = 0;
  virtual void SetEvictable(frame_id_t frame, bool evictable) = 0;
  virtual std::optional<frame_id_t> Evict() = 0;
  virtual void Remove(frame_id_t frame) = 0;
};

class ClockReplacer : public Replacer {
 public:
  explicit ClockReplacer(size_t frame_count);

  void RecordAccess(frame_id_t frame) override;
  void SetEvictable(frame_id_t frame, bool evictable) override;
  std::optional<frame_id_t> Evict() override;
  void Remove(frame_id_t frame) override;

 private:
  std::vector<bool> referenced_;
  std::vector<bool> evictable_;
  size_t hand_ = 0;
  size_t size_ = 0;
};

struct PageKey {
  uint32_t file_id = 0;
  page_id_t page_id = kInvalidPageId;

  bool operator==(const PageKey& other) const {
    return file_id == other.file_id && page_id == other.page_id;
  }
};

struct PageKeyHash {
  size_t operator()(const PageKey& key) const {
    return (static_cast<size_t>(key.file_id) << 32) ^
           static_cast<size_t>(key.page_id);
  }
};

struct Frame {
  std::shared_ptr<DiskManager> disk;
  page_id_t page_id = kInvalidPageId;
  int pin_count = 0;
  bool dirty = false;
  std::shared_mutex latch;
  alignas(64) std::byte data[kPageSize];
};

class BufferPool;

class ReadPageGuard {
 public:
  ReadPageGuard() = default;
  ReadPageGuard(BufferPool* pool, frame_id_t frame,
                std::shared_lock<std::shared_mutex>&& latch);
  ReadPageGuard(const ReadPageGuard&) = delete;
  ReadPageGuard& operator=(const ReadPageGuard&) = delete;
  ReadPageGuard(ReadPageGuard&& other) noexcept;
  ReadPageGuard& operator=(ReadPageGuard&& other) noexcept;
  ~ReadPageGuard();

  const std::byte* Data() const;
  page_id_t PageId() const;

 private:
  void Release();

  BufferPool* pool_ = nullptr;
  frame_id_t frame_ = 0;
  std::shared_lock<std::shared_mutex> latch_;
};

class WritePageGuard {
 public:
  WritePageGuard() = default;
  WritePageGuard(BufferPool* pool, frame_id_t frame,
                 std::unique_lock<std::shared_mutex>&& latch);
  WritePageGuard(const WritePageGuard&) = delete;
  WritePageGuard& operator=(const WritePageGuard&) = delete;
  WritePageGuard(WritePageGuard&& other) noexcept;
  WritePageGuard& operator=(WritePageGuard&& other) noexcept;
  ~WritePageGuard();

  std::byte* MutableData();
  const std::byte* Data() const;
  page_id_t PageId() const;

 private:
  void Release();

  BufferPool* pool_ = nullptr;
  frame_id_t frame_ = 0;
  std::unique_lock<std::shared_mutex> latch_;
};

class BufferPool {
 public:
  BufferPool(LogManager& log, const BufferConfig& config);
  ~BufferPool() = default;

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  Result<ReadPageGuard> FetchRead(const std::shared_ptr<DiskManager>& disk,
                                  page_id_t page_id);
  Result<WritePageGuard> FetchWrite(const std::shared_ptr<DiskManager>& disk,
                                    page_id_t page_id);
  // Initializes a page that has already been allocated by a Segment.
  Result<WritePageGuard> NewPage(const std::shared_ptr<DiskManager>& disk,
                                 page_id_t page_id, PageType type,
                                 segment_id_t segment_id);

  Status FlushPage(const std::shared_ptr<DiskManager>& disk,
                   page_id_t page_id);
  // Writes every dirty frame and fsyncs every file seen by the pool.
  Status FlushAll();
  // Forgets a freed page so stale bytes can never be observed.
  void DiscardPage(const std::shared_ptr<DiskManager>& disk,
                   page_id_t page_id);
  // Drops all cached pages of a file (used when a table file is deleted).
  // Fails when any page of that file is still pinned.
  Status DiscardFile(uint32_t file_id);

  const BufferStats& Stats() const { return stats_; }
  // Occupied frames in frame order. Frames whose page is write-latched report
  // PageType::kInvalid instead of waiting for the latch.
  std::vector<FrameInfo> Frames();
  size_t FrameCount() const { return frames_.size(); }

 private:
  friend class ReadPageGuard;
  friend class WritePageGuard;

  Result<frame_id_t> AcquireFrameLocked(
      const std::shared_ptr<DiskManager>& disk, page_id_t page_id,
      bool read_from_disk);
  Result<frame_id_t> TakeFreeFrameLocked();
  Status WriteBackLocked(frame_id_t frame);
  void Unpin(frame_id_t frame);
  void MarkDirty(frame_id_t frame);
  void RegisterFileLocked(const std::shared_ptr<DiskManager>& disk);

  std::byte* FrameData(frame_id_t frame) {
    return frames_[frame]->data;
  }
  const std::byte* FrameData(frame_id_t frame) const {
    return frames_[frame]->data;
  }
  page_id_t FramePageId(frame_id_t frame) const {
    return frames_[frame]->page_id;
  }

  LogManager& log_;
  std::vector<std::unique_ptr<Frame>> frames_;
  std::unordered_map<PageKey, frame_id_t, PageKeyHash> page_table_;
  std::unordered_map<uint32_t, std::shared_ptr<DiskManager>> files_;
  std::list<frame_id_t> free_list_;
  std::unique_ptr<Replacer> replacer_;
  std::mutex mutex_;
  BufferStats stats_;
};

}  // namespace minidb
