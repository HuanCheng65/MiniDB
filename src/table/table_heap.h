// TableHeap: record-level insert/get/update/delete/scan (design doc chapter 9).
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "buffer/buffer_pool.h"
#include "common/status.h"
#include "common/types.h"
#include "log/log_manager.h"
#include "record/schema.h"
#include "record/tuple.h"
#include "space/segment.h"
#include "txn/transaction.h"

namespace minidb {

class TableHeap {
 public:
  class Iterator {
   public:
    Iterator() = default;

    // Returns true and fills *rid / *tuple for each visible record. Returns
    // false at the end of the table or on error; check status() afterwards.
    bool Next(RID* rid, Tuple* tuple);
    const Status& status() const { return status_; }

   private:
    friend class TableHeap;
    void Reset();

    TableHeap* heap_ = nullptr;
    std::vector<page_id_t> pages_;
    size_t page_index_ = 0;
    slot_id_t next_slot_ = 0;
    page_id_t cached_page_ = kInvalidPageId;
    std::optional<ReadPageGuard> guard_;
    Status status_;
  };

  TableHeap(std::shared_ptr<Segment> segment, Schema schema,
            BufferPool& buffer_pool, LogManager& log)
      : segment_(std::move(segment)),
        schema_(std::move(schema)),
        buffer_pool_(buffer_pool),
        log_(log) {}

  Result<RID> Insert(Transaction* txn, const Tuple& tuple);
  Status Delete(Transaction* txn, RID rid);
  Result<RID> Update(Transaction* txn, RID rid, const Tuple& tuple);
  // Follows a forwarding pointer. A moved-in copy's RID is not a user-visible
  // RID (scans never return it), so Get reports it as not found.
  Result<Tuple> Get(Transaction* txn, RID rid);
  Iterator Scan(Transaction* txn);

  Status Flush(Transaction* txn) { return segment_->Flush(txn); }

  const Schema& schema() const { return schema_; }
  Segment& segment() { return *segment_; }
  std::shared_ptr<Segment> segment_ptr() const { return segment_; }
  const std::shared_ptr<DiskManager>& disk() const {
    return segment_->data_disk();
  }

 private:
  Result<RID> InsertRecord(Transaction* txn, const std::vector<std::byte>& record,
                           uint8_t slot_flags);
  // Reads the record stored exactly at rid (no forwarding, no visibility).
  Result<Tuple> ReadAt(RID rid);
  Status DeleteAt(Transaction* txn, RID rid);
  // Follows at most one forwarding hop. *moved_in reports whether rid itself
  // is the moved-in copy of another record (such RIDs are not user-visible).
  Result<RID> ResolveForward(RID rid, int* hops, bool* moved_in = nullptr);

  std::shared_ptr<Segment> segment_;
  Schema schema_;
  BufferPool& buffer_pool_;
  LogManager& log_;
};

}  // namespace minidb
