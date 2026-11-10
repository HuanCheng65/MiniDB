// Disk-backed B+ tree. Only visited nodes are decoded; all I/O uses BufferPool.
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
  Status Insert(const Value& key, RID rid);
  Status Erase(const Value& key, RID rid);
  // Operators: =, <, <=, >, >=, !=, <>. Visitor can stop early (LIMIT).
  Status Scan(const std::string& op, const Value& key,
              const std::function<bool(const IndexEntry&)>& visitor);
  Result<std::vector<RID>> Find(const Value& key);
  Result<IndexStats> Validate();
  Result<std::string> Draw(size_t max_nodes = 128);
  Result<std::string> Dot(size_t max_nodes = 128);

  const IndexMetadata& metadata() const { return metadata_; }
  const DiskManager::Ptr& disk() const { return disk_; }
  uint64_t size() const { return entries_; }

 private:
  struct Node {
    page_id_t id = kInvalidPageId;
    bool leaf = true;
    page_id_t next = kInvalidPageId;
    std::vector<IndexEntry> keys;
    std::vector<page_id_t> children;
  };
  struct Split {
    IndexEntry separator;
    page_id_t right;
  };
  BPlusTree(BufferPool& pool, DiskManager::Ptr disk, IndexMetadata metadata);
  static int Compare(const IndexEntry& a, const IndexEntry& b);
  Status CheckKey(const Value& key) const;
  Result<Node> Read(page_id_t id);
  Status Write(const Node& node);
  Status SaveMetadata();
  Result<page_id_t> Allocate();
  Status Free(page_id_t id);
  Result<std::optional<Split>> InsertNode(page_id_t id,
                                          const IndexEntry& entry);
  Status EraseNode(page_id_t id, const IndexEntry& entry);
  Status Rebalance(Node* parent, size_t child);
  Result<page_id_t> FindLeaf(const IndexEntry& entry);

  BufferPool& pool_;
  DiskManager::Ptr disk_;
  IndexMetadata metadata_;
  size_t key_bytes_ = 0;
  size_t capacity_ = 0;
  page_id_t root_ = 2;
  page_id_t next_page_ = 3;
  page_id_t free_head_ = kInvalidPageId;
  uint64_t entries_ = 0;
};

}  // namespace minidb
