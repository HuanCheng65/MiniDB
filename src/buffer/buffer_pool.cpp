#include "buffer/buffer_pool.h"

#include <cstring>
#include <filesystem>

#include "common/logger.h"

namespace minidb {

// ---------------------------------------------------------------- Replacer

ClockReplacer::ClockReplacer(size_t frame_count)
    : referenced_(frame_count, false), evictable_(frame_count, false) {}

void ClockReplacer::RecordAccess(frame_id_t frame) {
  if (frame < referenced_.size()) referenced_[frame] = true;
}

void ClockReplacer::SetEvictable(frame_id_t frame, bool evictable) {
  if (frame >= evictable_.size()) return;
  if (evictable && !evictable_[frame]) {
    evictable_[frame] = true;
    ++size_;
  } else if (!evictable && evictable_[frame]) {
    evictable_[frame] = false;
    --size_;
  }
}

std::optional<frame_id_t> ClockReplacer::Evict() {
  if (size_ == 0) return std::nullopt;
  const size_t n = referenced_.size();
  for (size_t scanned = 0; scanned < 2 * n; ++scanned) {
    const frame_id_t frame = static_cast<frame_id_t>(hand_);
    hand_ = (hand_ + 1) % n;
    if (!evictable_[frame]) continue;
    if (referenced_[frame]) {
      referenced_[frame] = false;
      continue;
    }
    evictable_[frame] = false;
    --size_;
    return frame;
  }
  return std::nullopt;
}

void ClockReplacer::Remove(frame_id_t frame) {
  if (frame >= evictable_.size()) return;
  if (evictable_[frame]) {
    evictable_[frame] = false;
    --size_;
  }
  referenced_[frame] = false;
}

// ------------------------------------------------------------------ Guards

ReadPageGuard::ReadPageGuard(BufferPool* pool, frame_id_t frame,
                             std::shared_lock<std::shared_mutex>&& latch)
    : pool_(pool), frame_(frame), latch_(std::move(latch)) {}

ReadPageGuard::ReadPageGuard(ReadPageGuard&& other) noexcept
    : pool_(other.pool_),
      frame_(other.frame_),
      latch_(std::move(other.latch_)) {
  other.pool_ = nullptr;
}

ReadPageGuard& ReadPageGuard::operator=(ReadPageGuard&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    frame_ = other.frame_;
    latch_ = std::move(other.latch_);
    other.pool_ = nullptr;
  }
  return *this;
}

ReadPageGuard::~ReadPageGuard() { Release(); }

void ReadPageGuard::Release() {
  if (pool_ == nullptr) return;
  latch_.unlock();
  pool_->Unpin(frame_);
  pool_ = nullptr;
}

const std::byte* ReadPageGuard::Data() const { return pool_->FrameData(frame_); }

page_id_t ReadPageGuard::PageId() const { return pool_->FramePageId(frame_); }

WritePageGuard::WritePageGuard(BufferPool* pool, frame_id_t frame,
                               std::unique_lock<std::shared_mutex>&& latch)
    : pool_(pool), frame_(frame), latch_(std::move(latch)) {}

WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept
    : pool_(other.pool_),
      frame_(other.frame_),
      latch_(std::move(other.latch_)) {
  other.pool_ = nullptr;
}

WritePageGuard& WritePageGuard::operator=(WritePageGuard&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    frame_ = other.frame_;
    latch_ = std::move(other.latch_);
    other.pool_ = nullptr;
  }
  return *this;
}

WritePageGuard::~WritePageGuard() { Release(); }

void WritePageGuard::Release() {
  if (pool_ == nullptr) return;
  latch_.unlock();
  pool_->Unpin(frame_);
  pool_ = nullptr;
}

std::byte* WritePageGuard::MutableData() {
  pool_->MarkDirty(frame_);
  return pool_->FrameData(frame_);
}

const std::byte* WritePageGuard::Data() const {
  return pool_->FrameData(frame_);
}

page_id_t WritePageGuard::PageId() const { return pool_->FramePageId(frame_); }

// -------------------------------------------------------------- BufferPool

BufferPool::BufferPool(LogManager& log, const BufferConfig& config) : log_(log) {
  const uint32_t frame_count = config.frames == 0 ? 1 : config.frames;
  frames_.reserve(frame_count);
  for (uint32_t i = 0; i < frame_count; ++i) {
    frames_.push_back(std::make_unique<Frame>());
    free_list_.push_back(i);
  }
  if (config.replacer != "clock" && !config.replacer.empty()) {
    MINIDB_LOG_WARN("BUF") << "unknown replacer '" << config.replacer
                           << "', using clock";
  }
  replacer_ = std::make_unique<ClockReplacer>(frame_count);
}

void BufferPool::RegisterFileLocked(
    const std::shared_ptr<DiskManager>& disk) {
  if (disk == nullptr) return;
  files_[disk->file_id()] = disk;
}

Result<frame_id_t> BufferPool::TakeFreeFrameLocked() {
  if (!free_list_.empty()) {
    const frame_id_t frame = free_list_.front();
    free_list_.pop_front();
    return frame;
  }
  std::optional<frame_id_t> victim = replacer_->Evict();
  if (!victim.has_value()) {
    return Status(StatusCode::kNoSpace,
                  "buffer pool is full: all frames are pinned");
  }
  const frame_id_t frame = *victim;
  Frame& victim_frame = *frames_[frame];
  MINIDB_LOG_DEBUG("BUF") << "EVICT frame=" << frame << " file="
                          << (victim_frame.disk ? victim_frame.disk->name() : "-")
                          << " pid=" << victim_frame.page_id
                          << " dirty=" << (victim_frame.dirty ? 1 : 0);
  if (victim_frame.dirty) {
    const Status status = WriteBackLocked(frame);
    if (!status.ok()) {
      replacer_->SetEvictable(frame, true);
      return status;
    }
  }
  if (victim_frame.disk != nullptr) {
    page_table_.erase(
        PageKey{victim_frame.disk->file_id(), victim_frame.page_id});
  }
  victim_frame.disk.reset();
  victim_frame.page_id = kInvalidPageId;
  victim_frame.pin_count = 0;
  victim_frame.dirty = false;
  ++stats_.evict;
  return frame;
}

Result<frame_id_t> BufferPool::AcquireFrameLocked(
    const std::shared_ptr<DiskManager>& disk, page_id_t page_id,
    bool read_from_disk) {
  if (disk == nullptr) {
    return Status(StatusCode::kInvalidArgument, "page file is NULL");
  }
  Frame* frame = nullptr;
  frame_id_t frame_id = 0;
  const PageKey key{disk->file_id(), page_id};

  auto found = page_table_.find(key);
  if (found != page_table_.end()) {
    frame_id = found->second;
    frame = frames_[frame_id].get();
    if (frame->pin_count == 0) replacer_->SetEvictable(frame_id, false);
    ++frame->pin_count;
    replacer_->RecordAccess(frame_id);
    ++stats_.hit;
  } else {
    auto result = TakeFreeFrameLocked();
    if (!result.ok()) return result.status();
    frame_id = result.value();
    frame = frames_[frame_id].get();
    frame->disk = disk;
    frame->page_id = page_id;
    frame->pin_count = 1;
    frame->dirty = false;
    page_table_[key] = frame_id;
    RegisterFileLocked(disk);
    replacer_->SetEvictable(frame_id, false);
    replacer_->RecordAccess(frame_id);
    ++stats_.miss;
    MINIDB_LOG_DEBUG("BUF") << "MISS file=" << disk->name() << " pid=" << page_id
                            << " -> frame=" << frame_id;

    if (read_from_disk) {
      const Status status = disk->ReadPage(page_id, frame->data);
      if (!status.ok()) {
        // Give the frame back as a free frame; it holds no valid page.
        page_table_.erase(key);
        frame->disk.reset();
        frame->page_id = kInvalidPageId;
        frame->pin_count = 0;
        replacer_->Remove(frame_id);
        free_list_.push_back(frame_id);
        return status;
      }
    } else {
      std::memset(frame->data, 0, kPageSize);
    }
  }
  return frame_id;
}

Result<ReadPageGuard> BufferPool::FetchRead(
    const std::shared_ptr<DiskManager>& disk, page_id_t page_id) {
  frame_id_t frame_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = AcquireFrameLocked(disk, page_id, true);
    if (!result.ok()) return result.status();
    frame_id = result.value();
  }
  return ReadPageGuard(this, frame_id,
                       std::shared_lock<std::shared_mutex>(
                           frames_[frame_id]->latch));
}

Result<WritePageGuard> BufferPool::FetchWrite(
    const std::shared_ptr<DiskManager>& disk, page_id_t page_id) {
  frame_id_t frame_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = AcquireFrameLocked(disk, page_id, true);
    if (!result.ok()) return result.status();
    frame_id = result.value();
  }
  return WritePageGuard(this, frame_id,
                        std::unique_lock<std::shared_mutex>(
                            frames_[frame_id]->latch));
}

Result<WritePageGuard> BufferPool::NewPage(
    const std::shared_ptr<DiskManager>& disk, page_id_t page_id, PageType type,
    segment_id_t segment_id) {
  if (disk == nullptr) {
    return Status(StatusCode::kInvalidArgument, "page file is NULL");
  }
  const PageKey key{disk->file_id(), page_id};
  frame_id_t frame_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = page_table_.find(key);
    if (found != page_table_.end()) {
      frame_id = found->second;
      Frame& frame = *frames_[frame_id];
      if (frame.pin_count != 0) {
        return Status(StatusCode::kNoSpace,
                      "cannot reinitialize pinned page " +
                          std::to_string(page_id));
      }
      page_table_.erase(found);
      replacer_->Remove(frame_id);
      frame.disk = disk;
      frame.page_id = page_id;
      frame.pin_count = 1;
      frame.dirty = true;
      std::memset(frame.data, 0, kPageSize);
      InitPageHeader(frame.data, type, page_id, segment_id);
      page_table_[key] = frame_id;
      RegisterFileLocked(disk);
      replacer_->SetEvictable(frame_id, false);
      replacer_->RecordAccess(frame_id);
      ++stats_.new_page;
    } else {
      auto result = TakeFreeFrameLocked();
      if (!result.ok()) return result.status();
      frame_id = result.value();
      Frame& frame = *frames_[frame_id];
      frame.disk = disk;
      frame.page_id = page_id;
      frame.pin_count = 1;
      frame.dirty = true;
      std::memset(frame.data, 0, kPageSize);
      InitPageHeader(frame.data, type, page_id, segment_id);
      page_table_[key] = frame_id;
      RegisterFileLocked(disk);
      replacer_->SetEvictable(frame_id, false);
      replacer_->RecordAccess(frame_id);
      ++stats_.new_page;  // not a miss: nothing is read from disk
      MINIDB_LOG_DEBUG("BUF") << "NEW file=" << disk->name() << " pid=" << page_id
                              << " type=" << static_cast<int>(type)
                              << " -> frame=" << frame_id;
    }
  }
  return WritePageGuard(this, frame_id,
                        std::unique_lock<std::shared_mutex>(
                            frames_[frame_id]->latch));
}

Status BufferPool::WriteBackLocked(frame_id_t frame_id) {
  Frame& frame = *frames_[frame_id];
  if (!frame.dirty || frame.page_id == kInvalidPageId ||
      frame.disk == nullptr) {
    return Status::Ok();
  }
  log_.FlushUntil(ReadU64(frame.data, kPageLsnOffset));
  const Status status = frame.disk->WritePage(frame.page_id, frame.data);
  if (!status.ok()) return status;
  frame.dirty = false;
  ++stats_.flush;
  MINIDB_LOG_DEBUG("BUF") << "FLUSH file=" << frame.disk->name()
                          << " pid=" << frame.page_id;
  return Status::Ok();
}

Status BufferPool::FlushPage(const std::shared_ptr<DiskManager>& disk,
                             page_id_t page_id) {
  if (disk == nullptr) return Status::Ok();
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = page_table_.find(PageKey{disk->file_id(), page_id});
  if (found == page_table_.end()) return Status::Ok();
  return WriteBackLocked(found->second);
}

Status BufferPool::FlushAll() {
  std::vector<std::shared_ptr<DiskManager>> files;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t frame = 0; frame < frames_.size(); ++frame) {
      const Status status = WriteBackLocked(static_cast<frame_id_t>(frame));
      if (!status.ok()) return status;
    }
    files.reserve(files_.size());
    for (const auto& entry : files_) files.push_back(entry.second);
  }
  for (const auto& disk : files) {
    const Status status = disk->Sync();
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

void BufferPool::DiscardPage(const std::shared_ptr<DiskManager>& disk,
                             page_id_t page_id) {
  if (disk == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = page_table_.find(PageKey{disk->file_id(), page_id});
  if (found == page_table_.end()) return;
  const frame_id_t frame_id = found->second;
  Frame& frame = *frames_[frame_id];
  if (frame.pin_count != 0) return;
  replacer_->Remove(frame_id);
  page_table_.erase(found);
  frame.disk.reset();
  frame.page_id = kInvalidPageId;
  frame.dirty = false;
  free_list_.push_back(frame_id);
}

Status BufferPool::DiscardFile(uint32_t file_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : page_table_) {
    if (entry.first.file_id != file_id) continue;
    if (frames_[entry.second]->pin_count != 0) {
      return Status(StatusCode::kNoSpace,
                    "cannot discard a file with pinned pages");
    }
  }
  for (auto it = page_table_.begin(); it != page_table_.end();) {
    if (it->first.file_id != file_id) {
      ++it;
      continue;
    }
    const frame_id_t frame_id = it->second;
    Frame& frame = *frames_[frame_id];
    replacer_->Remove(frame_id);
    frame.disk.reset();
    frame.page_id = kInvalidPageId;
    frame.dirty = false;
    free_list_.push_back(frame_id);
    it = page_table_.erase(it);
  }
  files_.erase(file_id);
  return Status::Ok();
}

std::vector<FrameInfo> BufferPool::Frames() {
  std::vector<FrameInfo> frames;
  std::lock_guard<std::mutex> lock(mutex_);
  for (frame_id_t id = 0; id < frames_.size(); ++id) {
    Frame& frame = *frames_[id];
    if (frame.disk == nullptr || frame.page_id == kInvalidPageId) continue;
    FrameInfo info;
    info.frame = id;
    info.file = frame.disk->name();
    info.page_id = frame.page_id;
    info.pin_count = frame.pin_count;
    info.dirty = frame.dirty;
    // Never wait for a page latch while holding mutex_ (design doc 11.5).
    if (frame.latch.try_lock_shared()) {
      info.page_type = GetPageType(frame.data);
      frame.latch.unlock_shared();
    }
    frames.push_back(std::move(info));
  }
  return frames;
}

void BufferPool::Unpin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Frame& frame = *frames_[frame_id];
  if (frame.pin_count > 0) --frame.pin_count;
  if (frame.pin_count == 0) replacer_->SetEvictable(frame_id, true);
}

void BufferPool::MarkDirty(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_[frame_id]->dirty = true;
}

}  // namespace minidb
