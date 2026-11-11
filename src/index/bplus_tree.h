// Disk-backed B+ tree. Nodes are searched and edited in their on-disk form;
// all I/O uses BufferPool.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "buffer/buffer_pool.h"
#include "record/schema.h"
#include "record/value.h"

namespace minidb {

struct IndexMetadata {
  std::string name;
  std::string table;
  uint32_t table_id = 0;
  uint32_t column = 0;
  TypeId type = TypeId::kInt32;
  uint16_t length = 0;
};

struct IndexEntry {
  Value key;
  RID rid;
};

struct IndexStats {
  uint64_t entries = 0;
  uint32_t height = 0;
  uint32_t nodes = 0;
  uint32_t leaves = 0;
  uint32_t free_pages = 0;
  uint32_t capacity = 0;
  page_id_t root = kInvalidPageId;
};

class BPlusTree {
 public:
  static Status ValidateMetadata(const IndexMetadata& metadata);
  static Result<std::unique_ptr<BPlusTree>> Create(
      BufferPool& pool, const std::string& path, const IndexMetadata& metadata);
  static Result<std::unique_ptr<BPlusTree>> Open(BufferPool& pool,
                                                 const std::string& path);
  // Builds an empty tree from entries added in any order (CREATE INDEX).
  // Entries are sorted within a memory budget: each full batch is written to
  // a temporary file as a sorted run, the runs are merged, and the merged
  // stream is written bottom-up as nodes 7/8 full. Memory use does not grow
  // with the table, and it is far cheaper than one root-to-leaf insert per
  // entry.
  class BulkLoader {
   public:
    explicit BulkLoader(BPlusTree& tree,
                        size_t memory_bytes = kDefaultSortMemory);
    ~BulkLoader();
    // NULL keys are skipped, as in Insert.
    Status Add(const Value& key, RID rid);
    Status Finish();
    // Sorted runs written to disk so far; 0 while everything fits in memory.
    size_t runs() const { return spilled_runs_; }
    // Passes that merged groups of runs because there were more runs than
    // the budget has read buffers for; the final merge is not counted.
    size_t merge_passes() const { return merge_passes_; }

   private:
    struct Run {
      uint64_t offset, bytes;  // within the temporary file
    };
    void Sort();
    Status Spill();
    Status MergePass(size_t fan_in, size_t buffer_bytes);
    Status Build(const std::function<Status(const std::byte**)>& next);

    BPlusTree& tree_;
    const size_t memory_bytes_;
    const std::string temp_path_;
    std::vector<std::byte> arena_;   // buffered records, back to back
    std::vector<uint32_t> offsets_;  // of each record in arena_; sorted
    std::vector<Run> runs_;
    uint64_t spilled_bytes_ = 0;
    size_t spilled_runs_ = 0;
    size_t merge_passes_ = 0;
    uint64_t count_ = 0;
  };
  static constexpr size_t kDefaultSortMemory = 64 << 20;
  // BulkLoader over a ready-made list.
  Status BulkLoad(std::vector<IndexEntry> entries,
                  size_t memory_bytes = kDefaultSortMemory);
  Status Insert(const Value& key, RID rid);
  Status Erase(const Value& key, RID rid);
  // Visits matching entries in key order; the visitor returns false to stop
  // early (LIMIT).
  Status Scan(CompareOp op, const Value& key,
              const std::function<bool(const IndexEntry&)>& visitor);
  Result<std::vector<RID>> Find(const Value& key);
  Result<IndexStats> Validate();
  Result<std::string> Draw(size_t max_nodes = 128);
  Result<std::string> Dot(size_t max_nodes = 128);

  const IndexMetadata& metadata() const { return metadata_; }
  const DiskManager::Ptr& disk() const { return disk_; }
  uint64_t size() const { return entries_; }

 private:
  class Node;
  struct Split {
    IndexEntry separator;
    page_id_t right;
  };
  BPlusTree(BufferPool& pool, DiskManager::Ptr disk, IndexMetadata metadata);
  Status CheckKey(const Value& key) const;
  Status Read(page_id_t id, Node* node);
  Status Write(const Node& node);
  Status SaveMetadata();
  Result<page_id_t> Allocate();
  Status Free(page_id_t id);
  Result<std::optional<Split>> InsertNode(page_id_t id,
                                          const IndexEntry& entry);
  Status EraseNode(page_id_t id, const IndexEntry& entry, size_t* remaining);
  Status Rebalance(Node* parent, size_t child);
  // Leaf that would hold `entry`; the leftmost leaf when it is null.
  Result<page_id_t> FindLeaf(const IndexEntry* entry);

  BufferPool& pool_;
  DiskManager::Ptr disk_;
  IndexMetadata metadata_;
  bool string_keys_ = false;
  size_t key_bytes_ = 0;
  size_t capacity_ = 0;
  page_id_t root_ = 2;
  page_id_t next_page_ = 3;
  page_id_t free_head_ = kInvalidPageId;
  uint64_t entries_ = 0;
};

}  // namespace minidb
