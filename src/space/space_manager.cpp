#include "space/space_manager.h"

#include "common/logger.h"

namespace minidb {

Result<extent_id_t> SpaceManager::AllocateExtent(Transaction* /*txn*/) {
  auto guard_result = buffer_pool_.FetchWrite(disk_, kBitmapPageId);
  if (!guard_result.ok()) return guard_result.status();
  WritePageGuard guard = guard_result.TakeValue();

  std::byte* page = guard.MutableData();
  if (GetPageType(page) != PageType::kExtentBitmap ||
      GetPageId(page) != kBitmapPageId) {
    return Status(StatusCode::kCorruption, "extent bitmap page is invalid");
  }

  std::byte* bitmap = page + kBitmapOffset;
  const size_t bit_count = BitmapBitCount();
  const size_t hint = ReadU32(page, kBitmapAllocHintOffset) % bit_count;
  extent_id_t extent = kInvalidExtentId;
  for (size_t i = 0; i < bit_count; ++i) {
    const size_t bit = (hint + i) % bit_count;
    if (!TestBit(bitmap, bit)) {
      extent = static_cast<extent_id_t>(bit);
      SetBit(bitmap, bit, true);
      WriteU32(page, kBitmapAllocHintOffset,
               static_cast<uint32_t>((bit + 1) % bit_count));
      break;
    }
  }
  if (extent == kInvalidExtentId) {
    return Status(StatusCode::kNoSpace, "extent bitmap is full");
  }
  guard = WritePageGuard();  // release before touching the file header

  if (extent >= disk_->header().file_extents) {
    const Status status = disk_->ExtendTo(extent + 1);
    if (!status.ok()) return status;
    const Status persist = disk_->PersistHeader();
    if (!persist.ok()) return persist;
  }

  MINIDB_LOG_DEBUG("SPACE") << "file=" << disk_->file_id()
                            << " ALLOC extent=" << extent
                            << " file_extents=" << disk_->header().file_extents;
  return extent;
}

Status SpaceManager::FreeExtent(Transaction* /*txn*/, extent_id_t extent) {
  if (extent == 0 || extent == kInvalidExtentId) {
    return Status(StatusCode::kInvalidArgument,
                  "cannot free the reserved extent");
  }
  auto guard_result = buffer_pool_.FetchWrite(disk_, kBitmapPageId);
  if (!guard_result.ok()) return guard_result.status();
  WritePageGuard guard = guard_result.TakeValue();
  std::byte* page = guard.MutableData();
  std::byte* bitmap = page + kBitmapOffset;
  const size_t bit_count = BitmapBitCount();
  if (extent >= bit_count) {
    return Status(StatusCode::kInvalidArgument, "extent id out of range");
  }
  SetBit(bitmap, extent, false);
  const size_t hint = ReadU32(page, kBitmapAllocHintOffset);
  if (extent < hint) WriteU32(page, kBitmapAllocHintOffset, extent);
  MINIDB_LOG_DEBUG("SPACE") << "file=" << disk_->file_id()
                            << " FREE extent=" << extent;
  return Status::Ok();
}

Result<std::unique_ptr<Segment>> SpaceManager::CreateSegment(
    Transaction* txn, segment_id_t segment_id, SegmentType type,
    extent_id_t first_extent, FreeSpaceMap& fsm) {
  return Segment::Create(*this, buffer_pool_, log_, segment_id, type, txn,
                         first_extent, fsm);
}

Result<std::unique_ptr<Segment>> SpaceManager::OpenSegment(
    page_id_t header_page, FreeSpaceMap& fsm) {
  return Segment::Open(*this, buffer_pool_, log_, header_page, fsm);
}

}  // namespace minidb
