#include "index/bplus_tree.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <queue>
#include <set>
#include <sstream>

namespace minidb {
namespace {
constexpr size_t kNodeData = 64;
constexpr uint32_t kIndexVersion = 1;
Status Corrupt(const std::string& detail) {
  return Status(StatusCode::kCorruption, "B+ tree: " + detail);
}
bool StringType(TypeId type) {
  return type == TypeId::kChar || type == TypeId::kVarchar;
}
std::string DotEscape(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n')
      out += "\\n";
    else if (c < 32 || c == 127)
      out += '?';
    else
      out += static_cast<char>(c);
  }
  return out;
}
}  // namespace

BPlusTree::BPlusTree(BufferPool& pool, DiskManager::Ptr disk,
                     IndexMetadata metadata)
    : pool_(pool), disk_(std::move(disk)), metadata_(std::move(metadata)) {
  key_bytes_ = StringType(metadata_.type) ? metadata_.length + 2 : 8;
  // Use a common, even capacity for leaves and internal nodes. Internal
  // entries additionally need a child page ID; the first child is in header.
  capacity_ = ((kPageSize - kNodeData) / (key_bytes_ + 6 + 4)) & ~size_t{1};
}

Status BPlusTree::ValidateMetadata(const IndexMetadata& m) {
  if (m.type != TypeId::kInt32 && m.type != TypeId::kInt64 &&
      !StringType(m.type))
    return Status(StatusCode::kNotSupported,
                  "indexes support INTEGER, BIGINT, CHAR and VARCHAR");
  if (StringType(m.type) && (m.length == 0 || m.length > 4000))
    return Status(StatusCode::kNotSupported,
                  "index string length must be 1..4000");
  if (m.name.empty() || m.table.empty() ||
      m.name.size() + m.table.size() > 8000)
    return Status(StatusCode::kInvalidArgument,
                  "invalid index/table name length");
  return Status::Ok();
}

Result<std::unique_ptr<BPlusTree>> BPlusTree::Create(
    BufferPool& pool, const std::string& path, const IndexMetadata& metadata) {
  auto status = ValidateMetadata(metadata);
  if (!status.ok()) return status;
  auto disk = DiskManager::Create(path, FileKind::kIndex, metadata.table_id);
  if (!disk.ok()) return disk.status();
  auto tree = std::unique_ptr<BPlusTree>(
      new BPlusTree(pool, disk.TakeValue(), metadata));
  Node root;
  root.id = tree->root_;
  status = tree->Write(root);
  if (!status.ok()) return status;
  status = tree->SaveMetadata();
  if (!status.ok()) return status;
  return tree;
}

Result<std::unique_ptr<BPlusTree>> BPlusTree::Open(BufferPool& pool,
                                                   const std::string& path) {
  auto disk = DiskManager::Open(path, FileKind::kIndex);
  if (!disk.ok()) return disk.status();
  auto guard = pool.FetchRead(disk.value(), 1);
  if (!guard.ok()) return guard.status();
  const auto* p = guard.value().Data();
  if (GetPageType(p) != PageType::kBtreeMeta || ReadU32(p, 32) != kIndexVersion)
    return Corrupt("bad metadata version");
  IndexMetadata m;
  m.table_id = disk.value()->header().table_id;
  m.column = ReadU32(p, 36);
  m.type = static_cast<TypeId>(ReadU32(p, 40));
  m.length = ReadU16(p, 44);
  const size_t names = ReadU16(p, 46), table = ReadU16(p, 48);
  if (names + table > 8000) return Corrupt("bad name length");
  m.name.assign(reinterpret_cast<const char*>(p + 80), names);
  m.table.assign(reinterpret_cast<const char*>(p + 80 + names), table);
  auto status = ValidateMetadata(m);
  if (!status.ok()) return Corrupt(status.message());
  auto tree =
      std::unique_ptr<BPlusTree>(new BPlusTree(pool, disk.TakeValue(), m));
  tree->root_ = ReadU32(p, 52);
  tree->next_page_ = ReadU32(p, 56);
  tree->free_head_ = ReadU32(p, 60);
  tree->entries_ = ReadU64(p, 64);
  if (tree->next_page_ < 3 || tree->next_page_ > tree->disk_->PageCount() ||
      tree->root_ < 2 || tree->root_ >= tree->next_page_ ||
      (tree->free_head_ != kInvalidPageId &&
       (tree->free_head_ < 2 || tree->free_head_ >= tree->next_page_)))
    return Corrupt("invalid root/allocation metadata");
  return tree;
}

Status BPlusTree::SaveMetadata() {
  auto guard =
      pool_.NewPage(disk_, 1, PageType::kBtreeMeta, metadata_.table_id);
  if (!guard.ok()) return guard.status();
  auto* p = guard.value().MutableData();
  WriteU32(p, 32, kIndexVersion);
  WriteU32(p, 36, metadata_.column);
  WriteU32(p, 40, static_cast<uint32_t>(metadata_.type));
  WriteU16(p, 44, metadata_.length);
  WriteU16(p, 46, static_cast<uint16_t>(metadata_.name.size()));
  WriteU16(p, 48, static_cast<uint16_t>(metadata_.table.size()));
  WriteU32(p, 52, root_);
  WriteU32(p, 56, next_page_);
  WriteU32(p, 60, free_head_);
  WriteU64(p, 64, entries_);
  std::memcpy(p + 80, metadata_.name.data(), metadata_.name.size());
  std::memcpy(p + 80 + metadata_.name.size(), metadata_.table.data(),
              metadata_.table.size());
  return Status::Ok();
}

int BPlusTree::Compare(const IndexEntry& a, const IndexEntry& b) {
  const int key = CompareValues(a.key, b.key);
  if (key) return key;
  if (a.rid.page_id != b.rid.page_id)
    return a.rid.page_id < b.rid.page_id ? -1 : 1;
  return a.rid.slot < b.rid.slot ? -1 : a.rid.slot > b.rid.slot ? 1 : 0;
}

Status BPlusTree::CheckKey(const Value& key) const {
  if (key.IsNull()) return Status::Ok();
  if (StringType(metadata_.type)) {
    if (key.kind != ValueKind::kString)
      return Status(StatusCode::kTypeMismatch, "expected string index key");
    if (key.string_value.size() > metadata_.length)
      return Status(StatusCode::kInvalidArgument, "index key too long");
  } else if (key.kind != ValueKind::kInt32 && key.kind != ValueKind::kInt64) {
    return Status(StatusCode::kTypeMismatch, "expected integer index key");
  }
  return Status::Ok();
}

Result<BPlusTree::Node> BPlusTree::Read(page_id_t id) {
  if (id < 2 || id >= next_page_) return Corrupt("invalid node page ID");
  auto guard = pool_.FetchRead(disk_, id);
  if (!guard.ok()) return guard.status();
  const auto* p = guard.value().Data();
  const auto type = GetPageType(p);
  if (type != PageType::kBtreeLeaf && type != PageType::kBtreeInternal)
    return Corrupt("invalid node type");
  Node n;
  n.id = id;
  n.leaf = type == PageType::kBtreeLeaf;
  n.next = ReadU32(p, 36);
  const size_t count = ReadU16(p, 32);
  if (count > capacity_) return Corrupt("node over capacity");
  if (!n.leaf) n.children.push_back(ReadU32(p, 40));
  size_t off = kNodeData;
  for (size_t i = 0; i < count; ++i) {
    Value key;
    if (StringType(metadata_.type)) {
      const size_t len = ReadU16(p, off);
      if (len > metadata_.length) return Corrupt("invalid string key length");
      key = Value::String(
          std::string(reinterpret_cast<const char*>(p + off + 2), len));
    } else {
      int64_t value;
      std::memcpy(&value, p + off, 8);
      key = Value::Int64(value);
    }
    off += key_bytes_;
    n.keys.push_back({std::move(key), {ReadU32(p, off), ReadU16(p, off + 4)}});
    off += 6;
    if (!n.leaf) {
      n.children.push_back(ReadU32(p, off));
      off += 4;
    }
  }
  return n;
}

Status BPlusTree::Write(const Node& n) {
  if (n.keys.size() > capacity_ ||
      (!n.leaf && n.children.size() != n.keys.size() + 1))
    return Status(StatusCode::kInternal, "invalid B+ node write");
  auto guard = pool_.NewPage(
      disk_, n.id, n.leaf ? PageType::kBtreeLeaf : PageType::kBtreeInternal,
      metadata_.table_id);
  if (!guard.ok()) return guard.status();
  auto* p = guard.value().MutableData();
  WriteU16(p, 32, static_cast<uint16_t>(n.keys.size()));
  WriteU32(p, 36, n.next);
  if (!n.leaf) WriteU32(p, 40, n.children[0]);
  size_t off = kNodeData;
  for (size_t i = 0; i < n.keys.size(); ++i) {
    const auto& entry = n.keys[i];
    if (StringType(metadata_.type)) {
      WriteU16(p, off, static_cast<uint16_t>(entry.key.string_value.size()));
      std::memcpy(p + off + 2, entry.key.string_value.data(),
                  entry.key.string_value.size());
    } else {
      const int64_t value = entry.key.kind == ValueKind::kInt32
                                ? entry.key.int32_value
                                : entry.key.int64_value;
      std::memcpy(p + off, &value, 8);
    }
    off += key_bytes_;
    WriteU32(p, off, entry.rid.page_id);
    WriteU16(p, off + 4, entry.rid.slot);
    off += 6;
    if (!n.leaf) {
      WriteU32(p, off, n.children[i + 1]);
      off += 4;
    }
  }
  return Status::Ok();
}

Result<page_id_t> BPlusTree::Allocate() {
  if (free_head_ != kInvalidPageId) {
    const page_id_t id = free_head_;
    auto guard = pool_.FetchRead(disk_, id);
    if (!guard.ok()) return guard.status();
    if (GetPageType(guard.value().Data()) != PageType::kBtreeFree)
      return Corrupt("bad free list");
    free_head_ = ReadU32(guard.value().Data(), 32);
    return id;
  }
  if (next_page_ == kInvalidPageId)
    return Status(StatusCode::kNoSpace, "index file full");
  if (next_page_ >= disk_->PageCount()) {
    auto status = disk_->ExtendTo(disk_->header().file_extents + 1);
    if (!status.ok()) return status;
    status = disk_->PersistHeader();
    if (!status.ok()) return status;
  }
  return next_page_++;
}

Status BPlusTree::Free(page_id_t id) {
  auto guard =
      pool_.NewPage(disk_, id, PageType::kBtreeFree, metadata_.table_id);
  if (!guard.ok()) return guard.status();
  WriteU32(guard.value().MutableData(), 32, free_head_);
  free_head_ = id;
  return Status::Ok();
}

Result<std::optional<BPlusTree::Split>> BPlusTree::InsertNode(
    page_id_t id, const IndexEntry& entry) {
  auto loaded = Read(id);
  if (!loaded.ok()) return loaded.status();
  Node n = loaded.TakeValue();
  if (n.leaf) {
    auto at = std::lower_bound(
        n.keys.begin(), n.keys.end(), entry,
        [](const auto& a, const auto& b) { return Compare(a, b) < 0; });
    if (at != n.keys.end() && Compare(*at, entry) == 0)
      return Status(StatusCode::kAlreadyExists, "index entry already exists");
    n.keys.insert(at, entry);
  } else {
    const size_t at = std::upper_bound(n.keys.begin(), n.keys.end(), entry,
                                       [](const auto& a, const auto& b) {
                                         return Compare(a, b) < 0;
                                       }) -
                      n.keys.begin();
    auto split = InsertNode(n.children[at], entry);
    if (!split.ok()) return split.status();
    if (!split.value()) return std::optional<Split>{};
    n.keys.insert(n.keys.begin() + at, split.value()->separator);
    n.children.insert(n.children.begin() + at + 1, split.value()->right);
  }
  if (n.keys.size() <= capacity_) {
    auto status = Write(n);
    if (!status.ok()) return status;
    return std::optional<Split>{};
  }
  auto allocated = Allocate();
  if (!allocated.ok()) return allocated.status();
  Node right;
  right.id = allocated.value();
  right.leaf = n.leaf;
  const size_t middle = n.keys.size() / 2;
  Split split{n.keys[middle], right.id};
  right.keys.assign(n.keys.begin() + middle + (n.leaf ? 0 : 1), n.keys.end());
  n.keys.resize(middle);
  if (n.leaf) {
    right.next = n.next;
    n.next = right.id;
  } else {
    right.children.assign(n.children.begin() + middle + 1, n.children.end());
    n.children.resize(middle + 1);
  }
  auto status = Write(right);
  if (!status.ok()) return status;
  status = Write(n);
  if (!status.ok()) return status;
  return std::optional<Split>{split};
}

Status BPlusTree::Insert(const Value& key, RID rid) {
  auto status = CheckKey(key);
  if (!status.ok() || key.IsNull()) return status;
  if (rid.page_id == kInvalidPageId)
    return Status(StatusCode::kInvalidArgument, "invalid RID");
  auto split = InsertNode(root_, {key, rid});
  if (!split.ok()) return split.status();
  if (split.value()) {
    auto allocated = Allocate();
    if (!allocated.ok()) return allocated.status();
    Node root;
    root.id = allocated.value();
    root.leaf = false;
    root.keys = {split.value()->separator};
    root.children = {root_, split.value()->right};
    status = Write(root);
    if (!status.ok()) return status;
    root_ = root.id;
  }
  ++entries_;
  return SaveMetadata();
}

Status BPlusTree::Rebalance(Node* parent, size_t at) {
  auto loaded = Read(parent->children[at]);
  if (!loaded.ok()) return loaded.status();
  Node child = loaded.TakeValue();
  if (child.keys.size() >= capacity_ / 2) return Status::Ok();
  // Prefer borrowing from the left, then right; otherwise merge into left.
  if (at > 0) {
    auto sibling = Read(parent->children[at - 1]);
    if (!sibling.ok()) return sibling.status();
    Node left = sibling.TakeValue();
    if (left.keys.size() > capacity_ / 2) {
      if (child.leaf) {
        child.keys.insert(child.keys.begin(), left.keys.back());
        parent->keys[at - 1] = child.keys.front();
      } else {
        child.keys.insert(child.keys.begin(), parent->keys[at - 1]);
        parent->keys[at - 1] = left.keys.back();
        child.children.insert(child.children.begin(), left.children.back());
        left.children.pop_back();
      }
      left.keys.pop_back();
      auto s = Write(left);
      if (!s.ok()) return s;
      return Write(child);
    }
  }
  if (at + 1 < parent->children.size()) {
    auto sibling = Read(parent->children[at + 1]);
    if (!sibling.ok()) return sibling.status();
    Node right = sibling.TakeValue();
    if (right.keys.size() > capacity_ / 2) {
      if (child.leaf) {
        child.keys.push_back(right.keys.front());
        right.keys.erase(right.keys.begin());
        parent->keys[at] = right.keys.front();
      } else {
        child.keys.push_back(parent->keys[at]);
        parent->keys[at] = right.keys.front();
        right.keys.erase(right.keys.begin());
        child.children.push_back(right.children.front());
        right.children.erase(right.children.begin());
      }
      auto s = Write(right);
      if (!s.ok()) return s;
      return Write(child);
    }
  }
  const size_t left_at = at > 0 ? at - 1 : at;
  auto l = Read(parent->children[left_at]);
  auto r = Read(parent->children[left_at + 1]);
  if (!l.ok()) return l.status();
  if (!r.ok()) return r.status();
  Node left = l.TakeValue(), right = r.TakeValue();
  if (!left.leaf) left.keys.push_back(parent->keys[left_at]);
  left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
  left.children.insert(left.children.end(), right.children.begin(),
                       right.children.end());
  left.next = right.next;
  auto s = Write(left);
  if (!s.ok()) return s;
  s = Free(right.id);
  if (!s.ok()) return s;
  parent->keys.erase(parent->keys.begin() + left_at);
  parent->children.erase(parent->children.begin() + left_at + 1);
  return Status::Ok();
}

Status BPlusTree::EraseNode(page_id_t id, const IndexEntry& entry) {
  auto loaded = Read(id);
  if (!loaded.ok()) return loaded.status();
  Node n = loaded.TakeValue();
  if (n.leaf) {
    auto at = std::lower_bound(
        n.keys.begin(), n.keys.end(), entry,
        [](const auto& a, const auto& b) { return Compare(a, b) < 0; });
    if (at == n.keys.end() || Compare(*at, entry) != 0)
      return Status(StatusCode::kNotFound, "index entry not found");
    n.keys.erase(at);
  } else {
    const size_t at = std::upper_bound(n.keys.begin(), n.keys.end(), entry,
                                       [](const auto& a, const auto& b) {
                                         return Compare(a, b) < 0;
                                       }) -
                      n.keys.begin();
    auto s = EraseNode(n.children[at], entry);
    if (!s.ok()) return s;
    s = Rebalance(&n, at);
    if (!s.ok()) return s;
  }
  return Write(n);
}

Status BPlusTree::Erase(const Value& key, RID rid) {
  auto s = CheckKey(key);
  if (!s.ok() || key.IsNull()) return s;
  s = EraseNode(root_, {key, rid});
  if (!s.ok()) return s;
  auto root = Read(root_);
  if (!root.ok()) return root.status();
  if (!root.value().leaf && root.value().keys.empty()) {
    const auto old = root_;
    root_ = root.value().children.front();
    s = Free(old);
    if (!s.ok()) return s;
  }
  --entries_;
  return SaveMetadata();
}

Result<page_id_t> BPlusTree::FindLeaf(const IndexEntry& entry) {
  page_id_t id = root_;
  for (size_t depth = 0; depth < 64; ++depth) {
    auto n = Read(id);
    if (!n.ok()) return n.status();
    if (n.value().leaf) return id;
    const size_t at =
        std::upper_bound(
            n.value().keys.begin(), n.value().keys.end(), entry,
            [](const auto& a, const auto& b) { return Compare(a, b) < 0; }) -
        n.value().keys.begin();
    id = n.value().children[at];
  }
  return Corrupt("tree depth/cycle");
}

Status BPlusTree::Scan(const std::string& op, const Value& key,
                       const std::function<bool(const IndexEntry&)>& visitor) {
  if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=" &&
      op != "!=" && op != "<>")
    return Status(StatusCode::kInvalidArgument, "unsupported index comparison");
  // Query bounds may be longer than the column; they need no on-disk encoding.
  if (!key.IsNull() &&
      (StringType(metadata_.type)
           ? key.kind != ValueKind::kString
           : key.kind != ValueKind::kInt32 && key.kind != ValueKind::kInt64))
    return Status(StatusCode::kTypeMismatch, "wrong index bound type");
  if (key.IsNull()) return Status::Ok();
  page_id_t id = root_;
  if (op == "=" || op == ">" || op == ">=") {
    auto leaf = FindLeaf({key, {0, 0}});
    if (!leaf.ok()) return leaf.status();
    id = leaf.value();
  } else {
    for (size_t depth = 0;; ++depth) {
      if (depth >= 64) return Corrupt("tree depth/cycle");
      auto n = Read(id);
      if (!n.ok()) return n.status();
      if (n.value().leaf) break;
      id = n.value().children.front();
    }
  }
  size_t visited = 0;
  while (id != kInvalidPageId) {
    if (++visited > next_page_) return Corrupt("leaf chain cycle");
    auto n = Read(id);
    if (!n.ok()) return n.status();
    if (!n.value().leaf) return Corrupt("non-leaf in leaf chain");
    for (const auto& e : n.value().keys) {
      const int cmp = CompareValues(e.key, key);
      if ((op == "=" && cmp > 0) || (op == "<" && cmp >= 0) ||
          (op == "<=" && cmp > 0))
        return Status::Ok();
      const bool match = op == "="    ? cmp == 0
                         : op == "<"  ? cmp < 0
                         : op == "<=" ? cmp <= 0
                         : op == ">"  ? cmp > 0
                         : op == ">=" ? cmp >= 0
                                      : cmp != 0;
      if (match && !visitor(e)) return Status::Ok();
    }
    id = n.value().next;
  }
  return Status::Ok();
}

Result<std::vector<RID>> BPlusTree::Find(const Value& key) {
  std::vector<RID> result;
  auto s = Scan("=", key, [&](const IndexEntry& entry) {
    result.push_back(entry.rid);
    return true;
  });
  if (!s.ok()) return s;
  return result;
}

Result<IndexStats> BPlusTree::Validate() {
  IndexStats stats;
  stats.root = root_;
  stats.capacity = static_cast<uint32_t>(capacity_);
  std::set<page_id_t> seen;
  std::vector<page_id_t> leaves;
  std::optional<IndexEntry> previous;
  std::function<Status(page_id_t, uint32_t, std::optional<IndexEntry>,
                       std::optional<IndexEntry>)>
      visit;
  visit = [&](page_id_t id, uint32_t depth, std::optional<IndexEntry> low,
              std::optional<IndexEntry> high) -> Status {
    if (depth > 64 || !seen.insert(id).second)
      return Corrupt("duplicate child/cycle");
    auto n = Read(id);
    if (!n.ok()) return n.status();
    const auto& node = n.value();
    ++stats.nodes;
    if (id != root_ && node.keys.size() < capacity_ / 2)
      return Corrupt("underfull node");
    if (id == root_ && !node.leaf && node.keys.empty())
      return Corrupt("empty internal root");
    for (size_t i = 0; i < node.keys.size(); ++i) {
      if (i && Compare(node.keys[i - 1], node.keys[i]) >= 0)
        return Corrupt("unsorted keys");
      if (low && Compare(node.keys[i], *low) < 0)
        return Corrupt("key below fence");
      if (high && Compare(node.keys[i], *high) >= 0)
        return Corrupt("key above fence");
    }
    if (node.leaf) {
      if (stats.height && stats.height != depth)
        return Corrupt("unbalanced leaf depths");
      stats.height = depth;
      ++stats.leaves;
      stats.entries += node.keys.size();
      leaves.push_back(id);
      for (const auto& e : node.keys) {
        if (previous && Compare(*previous, e) >= 0)
          return Corrupt("leaf order");
        previous = e;
      }
    } else {
      for (size_t i = 0; i < node.children.size(); ++i) {
        auto s =
            visit(node.children[i], depth + 1,
                  i ? std::optional<IndexEntry>(node.keys[i - 1]) : low,
                  i < node.keys.size() ? std::optional<IndexEntry>(node.keys[i])
                                       : high);
        if (!s.ok()) return s;
      }
    }
    return Status::Ok();
  };
  auto s = visit(root_, 1, {}, {});
  if (!s.ok()) return s;
  if (stats.entries != entries_) return Corrupt("entry count mismatch");
  for (size_t i = 0; i < leaves.size(); ++i) {
    auto n = Read(leaves[i]);
    if (!n.ok()) return n.status();
    if (n.value().next !=
        (i + 1 < leaves.size() ? leaves[i + 1] : kInvalidPageId))
      return Corrupt("broken leaf link");
  }
  page_id_t free = free_head_;
  while (free != kInvalidPageId) {
    if (free < 2 || free >= next_page_ || !seen.insert(free).second)
      return Corrupt("free list cycle/overlap");
    auto guard = pool_.FetchRead(disk_, free);
    if (!guard.ok()) return guard.status();
    if (GetPageType(guard.value().Data()) != PageType::kBtreeFree)
      return Corrupt("free page type");
    free = ReadU32(guard.value().Data(), 32);
    ++stats.free_pages;
  }
  if (seen.size() != next_page_ - 2)
    return Corrupt("unreachable allocated page");
  return stats;
}

Result<std::string> BPlusTree::Dot(size_t max_nodes) {
  std::ostringstream out;
  out << "digraph BPlusTree {\n  rankdir=TB;\n  node [shape=box, "
         "fontname=\"monospace\"];\n";
  std::queue<page_id_t> pending;
  std::set<page_id_t> seen;
  pending.push(root_);
  while (!pending.empty()) {
    const auto id = pending.front();
    pending.pop();
    if (!seen.insert(id).second) return Corrupt("cycle in visualization");
    if (seen.size() > max_nodes) {
      out << "  truncated [label=\"Display limited to " << max_nodes
          << " nodes\"];\n";
      break;
    }
    auto loaded = Read(id);
    if (!loaded.ok()) return loaded.status();
    const auto& n = loaded.value();
    out << "  p" << id << " [label=\"" << (n.leaf ? "Leaf" : "Internal") << " p"
        << id << " (" << n.keys.size() << " keys)";
    for (size_t i = 0; i < std::min<size_t>(n.keys.size(), 6); ++i) {
      const auto& e = n.keys[i];
      auto key = ValueToString(e.key);
      if (key.size() > 40) key = key.substr(0, 40) + "...";
      out << "\\n"
          << DotEscape(key) << " @" << e.rid.page_id << ":" << e.rid.slot;
    }
    if (n.keys.size() > 6) out << "\\n...";
    out << "\"];\n";
    for (auto child : n.children) {
      out << "  p" << id << " -> p" << child << ";\n";
      pending.push(child);
    }
    if (n.leaf && n.next != kInvalidPageId)
      out << "  p" << id << " -> p" << n.next
          << " [style=dashed, color=blue, constraint=false];\n";
  }
  out << "}\n";
  return out.str();
}
Result<std::string> BPlusTree::Draw(size_t max_nodes) {
  std::ostringstream out;
  out << "B+ tree " << metadata_.name << " (" << entries_ << " entries)\n";
  std::set<page_id_t> seen;
  std::function<Status(page_id_t, std::string, bool)> draw;
  draw = [&](page_id_t id, std::string prefix, bool last) -> Status {
    if (seen.size() >= max_nodes) {
      out << prefix << "... (display limit)\n";
      return Status::Ok();
    }
    if (!seen.insert(id).second) return Corrupt("cycle in visualization");
    auto loaded = Read(id);
    if (!loaded.ok()) return loaded.status();
    const auto& n = loaded.value();
    out << prefix << (last ? "`-- " : "|-- ")
        << (n.leaf ? "LEAF " : "INTERNAL ") << "p" << id << " ["
        << n.keys.size() << "] ";
    for (size_t i = 0; i < std::min<size_t>(n.keys.size(), 4); ++i) {
      if (i) out << " | ";
      auto key = ValueToString(n.keys[i].key);
      if (key.size() > 32) key = key.substr(0, 32) + "...";
      out << std::quoted(DotEscape(key)) << "@" << n.keys[i].rid.page_id << ":"
          << n.keys[i].rid.slot;
    }
    if (n.keys.size() > 4) out << " | ...";
    if (n.leaf)
      out << " -> "
          << (n.next == kInvalidPageId ? "END" : "p" + std::to_string(n.next));
    out << "\n";
    for (size_t i = 0; i < n.children.size(); ++i) {
      auto status = draw(n.children[i], prefix + (last ? "    " : "|   "),
                         i + 1 == n.children.size());
      if (!status.ok()) return status;
      if (seen.size() >= max_nodes) break;
    }
    return Status::Ok();
  };
  auto status = draw(root_, "", true);
  if (!status.ok()) return status;
  if (seen.size() >= max_nodes)
    out << "... display limited to " << max_nodes << " nodes\n";
  return out.str();
}
}  // namespace minidb
