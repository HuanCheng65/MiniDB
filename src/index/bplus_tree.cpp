#include "index/bplus_tree.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>

namespace minidb {
namespace {
// Node page layout after the 32-byte common header.
constexpr size_t kCountOffset = 32;       // u16 number of slots
constexpr size_t kNextOffset = 36;        // u32 next leaf (leaves)
constexpr size_t kFirstChildOffset = 40;  // u32 leftmost child (internal)
constexpr size_t kNodeData = 64;          // first slot
constexpr size_t kRidBytes = 6;
constexpr size_t kChildBytes = 4;
constexpr size_t kMaxKeyBytes = 4000 + 2;
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

// Strings compare with PAD SPACE semantics, exactly like CompareValues.
std::string_view TrimmedRight(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return text;
}
int64_t IntegerOf(const Value& key) {
  return key.kind == ValueKind::kInt32 ? key.int32_value : key.int64_value;
}
int CompareRids(RID a, RID b) {
  if (a.page_id != b.page_id) return a.page_id < b.page_id ? -1 : 1;
  return a.slot < b.slot ? -1 : a.slot > b.slot ? 1 : 0;
}
// Entries order by key, then RID, so duplicate keys stay distinct and sorted.
int CompareEntries(const IndexEntry& a, const IndexEntry& b) {
  const int key = CompareValues(a.key, b.key);
  return key ? key : CompareRids(a.rid, b.rid);
}
// A sort record is an index entry in the order it is added: the key as a slot
// stores it but without padding ([u16 length][bytes], or an int64), then the
// RID. Records are what the bulk loader sorts, spills and merges.
size_t RecordBytes(bool strings, const std::byte* record) {
  return (strings ? 2 + ReadU16(record, 0) : sizeof(int64_t)) + kRidBytes;
}
std::string_view RecordString(const std::byte* record) {
  return {reinterpret_cast<const char*>(record + 2), ReadU16(record, 0)};
}
int64_t RecordInteger(const std::byte* record) {
  int64_t value;
  std::memcpy(&value, record, sizeof(value));
  return value;
}
RID RecordRid(bool strings, const std::byte* record) {
  const std::byte* rid = record + RecordBytes(strings, record) - kRidBytes;
  return {ReadU32(rid, 0), ReadU16(rid, 4)};
}
// (key, RID) order, agreeing with CompareEntries.
int CompareRecords(bool strings, const std::byte* a, const std::byte* b) {
  int key;
  if (strings) {
    key = TrimmedRight(RecordString(a)).compare(TrimmedRight(RecordString(b)));
  } else {
    const int64_t x = RecordInteger(a), y = RecordInteger(b);
    key = x < y ? -1 : x > y ? 1 : 0;
  }
  return key ? key : CompareRids(RecordRid(strings, a), RecordRid(strings, b));
}

// One sorted run in the bulk loader's temporary file, read through a buffer.
// All runs share one file handle, so the number of runs is not limited by
// open files.
class RunReader {
 public:
  RunReader(uint64_t offset, uint64_t bytes, size_t buffer_bytes)
      : offset_(offset), left_(bytes), buffer_bytes_(buffer_bytes) {}

  // The record at the front of the run; null once the run is exhausted.
  const std::byte* current() const { return current_; }
  Status Advance(std::ifstream& file, bool strings) {
    current_ = nullptr;
    if (begin_ == end_ && left_ == 0) return Status::Ok();
    auto status = Fill(file, strings ? 2 : sizeof(int64_t));
    if (!status.ok()) return status;
    const size_t size = RecordBytes(strings, buffer_.data() + begin_);
    status = Fill(file, size);
    if (!status.ok()) return status;
    current_ = buffer_.data() + begin_;
    begin_ += size;
    return Status::Ok();
  }

 private:
  // Makes at least `need` unread bytes available at begin_.
  Status Fill(std::ifstream& file, size_t need) {
    if (end_ - begin_ >= need) return Status::Ok();
    if (buffer_.empty()) buffer_.resize(buffer_bytes_);
    std::memmove(buffer_.data(), buffer_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
    const size_t wanted =
        static_cast<size_t>(std::min<uint64_t>(buffer_.size() - end_, left_));
    file.seekg(static_cast<std::streamoff>(offset_));
    file.read(reinterpret_cast<char*>(buffer_.data() + end_),
              static_cast<std::streamsize>(wanted));
    if (!file || end_ + wanted < need)
      return Status(StatusCode::kIoError, "cannot read index sort run");
    offset_ += wanted;
    left_ -= wanted;
    end_ += wanted;
    return Status::Ok();
  }

  uint64_t offset_;  // of the next unread byte in the file
  uint64_t left_;    // bytes of this run not yet read from the file
  size_t buffer_bytes_;
  std::vector<std::byte> buffer_;
  size_t begin_ = 0, end_ = 0;
  const std::byte* current_ = nullptr;
};

// Merges sorted runs: a heap orders the runs by their front record.
class RunMerger {
 public:
  RunMerger(std::ifstream& file, bool strings, std::vector<RunReader> runs)
      : file_(file), strings_(strings), runs_(std::move(runs)) {}

  // The next record in (key, RID) order, valid until the following call;
  // null when every run is exhausted.
  Status Next(const std::byte** record) {
    if (!started_) {
      started_ = true;
      for (size_t i = 0; i < runs_.size(); ++i) {
        auto status = Push(i);
        if (!status.ok()) return status;
      }
    } else if (!heap_.empty()) {
      // The record handed out last time has been consumed: move its run on.
      std::pop_heap(heap_.begin(), heap_.end(), later_);
      const size_t run = heap_.back();
      heap_.pop_back();
      auto status = Push(run);
      if (!status.ok()) return status;
    }
    *record = heap_.empty() ? nullptr : runs_[heap_.front()].current();
    return Status::Ok();
  }

 private:
  Status Push(size_t run) {
    auto status = runs_[run].Advance(file_, strings_);
    if (!status.ok() || !runs_[run].current()) return status;
    heap_.push_back(run);
    std::push_heap(heap_.begin(), heap_.end(), later_);
    return Status::Ok();
  }

  std::ifstream& file_;
  bool strings_;
  std::vector<RunReader> runs_;
  std::vector<size_t> heap_;  // run indexes, smallest front record on top
  std::function<bool(size_t, size_t)> later_ = [this](size_t a, size_t b) {
    return CompareRecords(strings_, runs_[a].current(), runs_[b].current()) > 0;
  };
  bool started_ = false;
};

// Read buffer of one run during a merge: an eighth of the sort budget, but
// at least two of the longest records and at most 64 KB.
size_t MergeBufferBytes(size_t memory_bytes) {
  return std::clamp<size_t>(memory_bytes / 8, 8 * 1024, 64 * 1024);
}

// Splits `count` items into the fewest groups of at most `target` items, then
// drops groups while an even split would leave them below `minimum`.
size_t GroupCount(size_t count, size_t target, size_t minimum) {
  size_t groups = (count + target - 1) / target;
  while (groups > 1 && count / groups < minimum) --groups;
  return groups;
}
}  // namespace

// A working copy of one node page. Slots are fixed width and keep their
// on-disk encoding, [key | RID | right child (internal only)], so a search
// compares bytes in place and an insert or erase is one memmove; nothing is
// decoded unless a caller asks for an entry. Slot i of an internal node holds
// separator i and the child to its right; the leftmost child is in the header.
//
// It is a copy, not a pinned page, so the tree never holds more than one
// buffer frame at a time and works with the smallest pools.
class BPlusTree::Node {
 public:
  explicit Node(const BPlusTree& tree) : tree_(tree) {}

  // Makes this an empty node for page `id`.
  void Reset(page_id_t id, bool leaf) {
    id_ = id;
    leaf_ = leaf;
    std::memset(raw_, 0, kNodeData);
    set_next(kInvalidPageId);
  }
  void CopyFrom(page_id_t id, bool leaf, const std::byte* page) {
    id_ = id;
    leaf_ = leaf;
    std::memcpy(raw_, page, kPageSize);
  }
  // The bytes after the common page header that are in use.
  void CopyBodyTo(std::byte* page) const {
    std::memcpy(page + kCountOffset, raw_ + kCountOffset,
                kNodeData - kCountOffset + size() * slot_bytes());
  }

  page_id_t id() const { return id_; }
  bool leaf() const { return leaf_; }
  size_t size() const { return ReadU16(raw_, kCountOffset); }
  page_id_t next() const { return ReadU32(raw_, kNextOffset); }
  void set_next(page_id_t next) { WriteU32(raw_, kNextOffset, next); }
  // Internal nodes have size() + 1 children.
  page_id_t child(size_t i) const {
    return i == 0 ? ReadU32(raw_, kFirstChildOffset)
                  : ReadU32(Slot(i - 1), tree_.key_bytes_ + kRidBytes);
  }
  void set_first_child(page_id_t child) {
    WriteU32(raw_, kFirstChildOffset, child);
  }

  IndexEntry entry(size_t i) const {
    const std::byte* slot = Slot(i);
    return {tree_.string_keys_ ? Value::String(std::string(StringKey(slot)))
                               : Value::Int64(IntegerKey(slot)),
            Rid(slot)};
  }
  std::vector<IndexEntry> entries() const {
    std::vector<IndexEntry> all;
    for (size_t i = 0; i < size(); ++i) all.push_back(entry(i));
    return all;
  }
  // Key of slot i against `key`, agreeing with CompareValues.
  int CompareKey(size_t i, const Value& key) const {
    const std::byte* slot = Slot(i);
    if (tree_.string_keys_) {
      const int cmp =
          TrimmedRight(StringKey(slot)).compare(TrimmedRight(key.string_value));
      return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
    }
    const int64_t a = IntegerKey(slot), b = IntegerOf(key);
    return a < b ? -1 : a > b ? 1 : 0;
  }
  // Slot i against `probe` in (key, RID) order.
  int Compare(size_t i, const IndexEntry& probe) const {
    const int key = CompareKey(i, probe.key);
    return key ? key : CompareRids(Rid(Slot(i)), probe.rid);
  }
  // First slot not less than `probe`: where it is, or belongs, in a leaf.
  size_t LowerBound(const IndexEntry& probe) const {
    size_t low = 0, high = size();
    while (low < high) {
      const size_t middle = (low + high) / 2;
      if (Compare(middle, probe) < 0)
        low = middle + 1;
      else
        high = middle;
    }
    return low;
  }
  // Child to descend into: a separator equal to `probe` sends it right.
  size_t ChildIndex(const IndexEntry& probe) const {
    size_t low = 0, high = size();
    while (low < high) {
      const size_t middle = (low + high) / 2;
      if (Compare(middle, probe) <= 0)
        low = middle + 1;
      else
        high = middle;
    }
    return low;
  }

  // Inserts `entry` as slot `at`; in an internal node `right_child` becomes
  // child at + 1. A full node may take one more slot until it is split.
  void Insert(size_t at, const IndexEntry& entry,
              page_id_t right_child = kInvalidPageId) {
    std::byte* slot = Slot(at);
    std::memmove(slot + slot_bytes(), slot, (size() - at) * slot_bytes());
    set_size(size() + 1);
    SetKey(at, entry);
    if (!leaf_) WriteU32(slot, tree_.key_bytes_ + kRidBytes, right_child);
  }
  void Append(const IndexEntry& entry, page_id_t right_child = kInvalidPageId) {
    Insert(size(), entry, right_child);
  }
  // Appends slots [from, from + count) of `source`, a node of the same kind.
  void Append(const Node& source, size_t from, size_t count) {
    std::memcpy(Slot(size()), source.Slot(from), count * slot_bytes());
    set_size(size() + count);
  }
  // Replaces the key and RID of slot `at`, keeping its child.
  void SetKey(size_t at, const IndexEntry& entry) {
    std::byte* slot = Slot(at);
    if (tree_.string_keys_) {
      const std::string& text = entry.key.string_value;
      std::memset(slot, 0, tree_.key_bytes_);
      WriteU16(slot, 0, static_cast<uint16_t>(text.size()));
      std::memcpy(slot + 2, text.data(), text.size());
    } else {
      const int64_t value = IntegerOf(entry.key);
      std::memcpy(slot, &value, sizeof(value));
    }
    WriteU32(slot, tree_.key_bytes_, entry.rid.page_id);
    WriteU16(slot, tree_.key_bytes_ + 4, entry.rid.slot);
  }
  // Removes slot `at`: in an internal node, separator at and child at + 1.
  void Erase(size_t at) {
    std::byte* slot = Slot(at);
    std::memmove(slot, slot + slot_bytes(), (size() - at - 1) * slot_bytes());
    set_size(size() - 1);
  }
  void Truncate(size_t size) { set_size(size); }
  // Whether every stored string length fits the column (StringKey clamps a
  // bad one rather than failing, so Validate() checks them).
  bool KeyLengthsValid() const {
    for (size_t i = 0; tree_.string_keys_ && i < size(); ++i)
      if (ReadU16(Slot(i), 0) > tree_.metadata_.length) return false;
    return true;
  }

 private:
  size_t slot_bytes() const {
    return tree_.key_bytes_ + kRidBytes + (leaf_ ? 0 : kChildBytes);
  }
  std::byte* Slot(size_t i) { return raw_ + kNodeData + i * slot_bytes(); }
  const std::byte* Slot(size_t i) const {
    return raw_ + kNodeData + i * slot_bytes();
  }
  void set_size(size_t size) {
    WriteU16(raw_, kCountOffset, static_cast<uint16_t>(size));
  }
  std::string_view StringKey(const std::byte* slot) const {
    // Clamped so a damaged length can never read outside the slot.
    return {reinterpret_cast<const char*>(slot + 2),
            std::min<size_t>(ReadU16(slot, 0), tree_.metadata_.length)};
  }
  static int64_t IntegerKey(const std::byte* slot) {
    int64_t value;
    std::memcpy(&value, slot, sizeof(value));
    return value;
  }
  RID Rid(const std::byte* slot) const {
    return {ReadU32(slot, tree_.key_bytes_),
            ReadU16(slot, tree_.key_bytes_ + 4)};
  }

  const BPlusTree& tree_;
  page_id_t id_ = kInvalidPageId;
  bool leaf_ = true;
  // One page plus room for the extra slot a full node holds before a split.
  std::byte raw_[kPageSize + kMaxKeyBytes + kRidBytes + kChildBytes];
};

BPlusTree::BPlusTree(BufferPool& pool, DiskManager::Ptr disk,
                     IndexMetadata metadata)
    : pool_(pool), disk_(std::move(disk)), metadata_(std::move(metadata)) {
  string_keys_ = StringType(metadata_.type);
  key_bytes_ = string_keys_ ? metadata_.length + 2 : 8;
  // Use a common, even capacity for leaves and internal nodes. Internal
  // entries additionally need a child page ID; the first child is in header.
  capacity_ =
      ((kPageSize - kNodeData) / (key_bytes_ + kRidBytes + kChildBytes)) &
      ~size_t{1};
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
  Node root(*tree);
  root.Reset(tree->root_, true);
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

Status BPlusTree::Read(page_id_t id, Node* node) {
  if (id < 2 || id >= next_page_) return Corrupt("invalid node page ID");
  auto guard = pool_.FetchRead(disk_, id);
  if (!guard.ok()) return guard.status();
  const auto* p = guard.value().Data();
  const auto type = GetPageType(p);
  if (type != PageType::kBtreeLeaf && type != PageType::kBtreeInternal)
    return Corrupt("invalid node type");
  if (ReadU16(p, kCountOffset) > capacity_)
    return Corrupt("node over capacity");
  node->CopyFrom(id, type == PageType::kBtreeLeaf, p);
  return Status::Ok();
}

Status BPlusTree::Write(const Node& node) {
  // The working copy has room for one slot more than a page: never let an
  // unsplit node reach the disk.
  if (node.size() > capacity_)
    return Status(StatusCode::kInternal, "invalid B+ node write");
  auto guard = pool_.NewPage(
      disk_, node.id(),
      node.leaf() ? PageType::kBtreeLeaf : PageType::kBtreeInternal,
      metadata_.table_id);
  if (!guard.ok()) return guard.status();
  node.CopyBodyTo(guard.value().MutableData());
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
  Node n(*this);
  auto status = Read(id, &n);
  if (!status.ok()) return status;
  if (n.leaf()) {
    const size_t at = n.LowerBound(entry);
    if (at < n.size() && n.Compare(at, entry) == 0)
      return Status(StatusCode::kAlreadyExists, "index entry already exists");
    n.Insert(at, entry);
  } else {
    const size_t at = n.ChildIndex(entry);
    auto split = InsertNode(n.child(at), entry);
    if (!split.ok()) return split.status();
    if (!split.value()) return std::optional<Split>{};
    n.Insert(at, split.value()->separator, split.value()->right);
  }
  if (n.size() <= capacity_) {
    status = Write(n);
    if (!status.ok()) return status;
    return std::optional<Split>{};
  }
  // Split: the upper half moves to a new right sibling. A leaf keeps the
  // separator as the sibling's first entry; an internal node moves it up.
  auto allocated = Allocate();
  if (!allocated.ok()) return allocated.status();
  Node right(*this);
  right.Reset(allocated.value(), n.leaf());
  const size_t middle = n.size() / 2;
  Split split{n.entry(middle), right.id()};
  if (n.leaf()) {
    right.Append(n, middle, n.size() - middle);
    right.set_next(n.next());
    n.set_next(right.id());
  } else {
    right.set_first_child(n.child(middle + 1));
    right.Append(n, middle + 1, n.size() - middle - 1);
  }
  n.Truncate(middle);
  status = Write(right);
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
    Node root(*this);
    root.Reset(allocated.value(), false);
    root.set_first_child(root_);
    root.Append(split.value()->separator, split.value()->right);
    status = Write(root);
    if (!status.ok()) return status;
    root_ = root.id();
  }
  ++entries_;
  return SaveMetadata();
}

BPlusTree::BulkLoader::BulkLoader(BPlusTree& tree, size_t memory_bytes)
    : tree_(tree),
      // Record offsets are 32-bit, which caps one in-memory batch.
      memory_bytes_(std::min<size_t>(memory_bytes, size_t{1} << 31)),
      temp_path_(tree.disk_->path() + ".sort") {}

BPlusTree::BulkLoader::~BulkLoader() {
  std::error_code ignored;
  if (spilled_runs_ == 0) return;
  std::filesystem::remove(temp_path_, ignored);
  std::filesystem::remove(temp_path_ + ".merge", ignored);
}

Status BPlusTree::BulkLoader::Add(const Value& key, RID rid) {
  auto status = tree_.CheckKey(key);
  if (!status.ok() || key.IsNull()) return status;
  if (rid.page_id == kInvalidPageId)
    return Status(StatusCode::kInvalidArgument, "invalid RID");
  // The budget is split between record bytes and record offsets and both
  // buffers are allocated once, so memory never exceeds it: when the next
  // record does not fit, the batch is spilled first.
  if (arena_.capacity() == 0) {
    arena_.reserve(std::max(memory_bytes_ / 4 * 3, kMaxKeyBytes + kRidBytes));
    offsets_.reserve(std::max<size_t>(memory_bytes_ / 4 / sizeof(uint32_t), 1));
  }
  const size_t key_bytes =
      tree_.string_keys_ ? 2 + key.string_value.size() : sizeof(int64_t);
  if (arena_.size() + key_bytes + kRidBytes > arena_.capacity() ||
      offsets_.size() == offsets_.capacity()) {
    status = Spill();
    if (!status.ok()) return status;
  }
  offsets_.push_back(static_cast<uint32_t>(arena_.size()));
  const auto append = [&](const void* data, size_t size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    arena_.insert(arena_.end(), bytes, bytes + size);
  };
  if (tree_.string_keys_) {
    const auto length = static_cast<uint16_t>(key.string_value.size());
    append(&length, sizeof(length));
    append(key.string_value.data(), length);
  } else {
    const int64_t value = IntegerOf(key);
    append(&value, sizeof(value));
  }
  append(&rid.page_id, sizeof(rid.page_id));
  append(&rid.slot, sizeof(rid.slot));
  ++count_;
  return Status::Ok();
}

void BPlusTree::BulkLoader::Sort() {
  std::sort(offsets_.begin(), offsets_.end(), [&](uint32_t a, uint32_t b) {
    return CompareRecords(tree_.string_keys_, arena_.data() + a,
                          arena_.data() + b) < 0;
  });
}

// Writes the buffered records to the temporary file as one sorted run.
Status BPlusTree::BulkLoader::Spill() {
  Sort();
  std::ofstream file(
      temp_path_,
      std::ios::binary | (runs_.empty() ? std::ios::trunc : std::ios::app));
  uint64_t bytes = 0;
  for (const uint32_t offset : offsets_) {
    const std::byte* record = arena_.data() + offset;
    const size_t size = RecordBytes(tree_.string_keys_, record);
    file.write(reinterpret_cast<const char*>(record),
               static_cast<std::streamsize>(size));
    bytes += size;
  }
  file.close();
  if (!file) return Status(StatusCode::kIoError, "cannot write index sort run");
  runs_.push_back({spilled_bytes_, bytes});
  ++spilled_runs_;
  spilled_bytes_ += bytes;
  arena_.clear();
  offsets_.clear();
  return Status::Ok();
}

Status BPlusTree::BulkLoader::Finish() {
  if (tree_.entries_ != 0)
    return Status(StatusCode::kInvalidArgument,
                  "bulk load needs an empty tree");
  if (count_ == 0) return Status::Ok();
  if (runs_.empty()) {
    // Everything fit in memory: one sort, no temporary file.
    Sort();
    size_t next = 0;
    return Build([&](const std::byte** record) {
      *record =
          next < offsets_.size() ? arena_.data() + offsets_[next++] : nullptr;
      return Status::Ok();
    });
  }
  if (!offsets_.empty()) {
    auto status = Spill();
    if (!status.ok()) return status;
  }
  arena_ = {};
  offsets_ = {};

  // The budget also bounds the merge: it pays for one read buffer per run, so
  // when there are more runs than buffers, groups of runs are first merged
  // into longer runs until one final merge can feed the build.
  const size_t buffer_bytes = MergeBufferBytes(memory_bytes_);
  const size_t fan_in = std::max<size_t>(memory_bytes_ / buffer_bytes, 2);
  while (runs_.size() > fan_in) {
    auto status = MergePass(fan_in, buffer_bytes);
    if (!status.ok()) return status;
  }
  std::ifstream file(temp_path_, std::ios::binary);
  std::vector<RunReader> readers;
  for (const Run& run : runs_)
    readers.emplace_back(run.offset, run.bytes, buffer_bytes);
  RunMerger merger(file, tree_.string_keys_, std::move(readers));
  return Build([&](const std::byte** record) { return merger.Next(record); });
}

// Rewrites the temporary file with every `fan_in` consecutive runs merged
// into one.
Status BPlusTree::BulkLoader::MergePass(size_t fan_in, size_t buffer_bytes) {
  const bool strings = tree_.string_keys_;
  const std::string merged_path = temp_path_ + ".merge";
  std::vector<Run> merged;
  {
    std::ifstream input(temp_path_, std::ios::binary);
    std::ofstream output(merged_path, std::ios::binary | std::ios::trunc);
    uint64_t offset = 0;
    for (size_t first = 0; first < runs_.size(); first += fan_in) {
      std::vector<RunReader> readers;
      for (size_t i = first; i < std::min(first + fan_in, runs_.size()); ++i)
        readers.emplace_back(runs_[i].offset, runs_[i].bytes, buffer_bytes);
      RunMerger merger(input, strings, std::move(readers));
      uint64_t bytes = 0;
      for (;;) {
        const std::byte* record = nullptr;
        auto status = merger.Next(&record);
        if (!status.ok()) return status;
        if (!record) break;
        const size_t size = RecordBytes(strings, record);
        output.write(reinterpret_cast<const char*>(record),
                     static_cast<std::streamsize>(size));
        bytes += size;
      }
      merged.push_back({offset, bytes});
      offset += bytes;
    }
    output.close();
    if (!output)
      return Status(StatusCode::kIoError, "cannot write index sort run");
  }
  std::error_code error;
  std::filesystem::rename(merged_path, temp_path_, error);
  if (error) return Status(StatusCode::kIoError, error.message());
  runs_ = std::move(merged);
  ++merge_passes_;
  return Status::Ok();
}

// Writes the tree bottom-up from the sorted record stream `next`. The entry
// count is known, so the size of every node on every level is planned up
// front and only one node per level is in memory: a leaf fills, is written,
// and hands its page and lowest entry to the level above, which fills the
// same way.
Status BPlusTree::BulkLoader::Build(
    const std::function<Status(const std::byte**)>& next) {
  BPlusTree& tree = tree_;
  // Leave an eighth of each node free so the first inserts do not split.
  const size_t fill = tree.capacity_ - tree.capacity_ / 8;
  struct Level {
    explicit Level(const BPlusTree& tree) : node(tree) {}
    Node node;          // the node being filled
    size_t items = 0;   // entries (leaves) or children (internal) on the level
    size_t nodes = 0;   // nodes the items are spread over
    size_t index = 0;   // which node is being filled
    size_t filled = 0;  // items in it so far
    IndexEntry lowest;  // its lowest entry: the separator its parent stores
    page_id_t reserved = kInvalidPageId;  // page for the next node, if chosen
  };
  std::vector<std::unique_ptr<Level>> levels;
  for (size_t items = count_;; items = levels.back()->nodes) {
    auto level = std::make_unique<Level>(tree);
    level->items = items;
    level->nodes = levels.empty()
                       ? GroupCount(items, fill, tree.capacity_ / 2)
                       : GroupCount(items, fill + 1, tree.capacity_ / 2 + 1);
    levels.push_back(std::move(level));
    if (levels.back()->nodes == 1) break;  // the root
  }
  levels[0]->reserved = tree.root_;  // reuse the empty root page of Create()

  // Puts one item into the node being filled on level `at`; a full node is
  // written and becomes an item of the level above.
  std::function<Status(size_t, const IndexEntry&, page_id_t)> add;
  add = [&](size_t at, const IndexEntry& entry, page_id_t child) -> Status {
    Level& level = *levels[at];
    const bool leaf = at == 0;
    if (level.filled == 0) {
      page_id_t id = level.reserved;
      if (id == kInvalidPageId) {
        auto allocated = tree.Allocate();
        if (!allocated.ok()) return allocated.status();
        id = allocated.value();
      }
      level.reserved = kInvalidPageId;
      level.node.Reset(id, leaf);
      level.lowest = entry;
    }
    if (leaf)
      level.node.Append(entry);
    else if (level.filled == 0)
      level.node.set_first_child(child);
    else
      level.node.Append(entry, child);
    const size_t planned =
        level.items / level.nodes + (level.index < level.items % level.nodes);
    if (++level.filled < planned) return Status::Ok();

    if (leaf && level.index + 1 < level.nodes) {
      // A leaf links to its successor, so that page is chosen now.
      auto allocated = tree.Allocate();
      if (!allocated.ok()) return allocated.status();
      level.reserved = allocated.value();
      level.node.set_next(level.reserved);
    }
    auto status = tree.Write(level.node);
    if (!status.ok()) return status;
    level.filled = 0;
    ++level.index;
    if (at + 1 == levels.size()) {
      tree.root_ = level.node.id();
      return Status::Ok();
    }
    return add(at + 1, level.lowest, level.node.id());
  };

  const bool strings = tree.string_keys_;
  IndexEntry entry;
  entry.key = strings ? Value::String("") : Value::Int64(0);
  std::vector<std::byte> previous;
  uint64_t loaded = 0;
  for (;; ++loaded) {
    const std::byte* record = nullptr;
    auto status = next(&record);
    if (!status.ok()) return status;
    if (!record) break;
    if (!previous.empty() &&
        CompareRecords(strings, previous.data(), record) >= 0)
      return Status(StatusCode::kAlreadyExists, "index entry already exists");
    previous.assign(record, record + RecordBytes(strings, record));
    if (strings)
      entry.key.string_value = RecordString(record);
    else
      entry.key.int64_value = RecordInteger(record);
    entry.rid = RecordRid(strings, record);
    status = add(0, entry, kInvalidPageId);
    if (!status.ok()) return status;
  }
  if (loaded != count_)
    return Status(StatusCode::kInternal, "index sort lost entries");
  tree.entries_ = count_;
  return tree.SaveMetadata();
}

Status BPlusTree::BulkLoad(std::vector<IndexEntry> entries,
                           size_t memory_bytes) {
  if (entries_ != 0)
    return Status(StatusCode::kInvalidArgument,
                  "bulk load needs an empty tree");
  BulkLoader loader(*this, memory_bytes);
  for (const IndexEntry& entry : entries) {
    auto status = loader.Add(entry.key, entry.rid);
    if (!status.ok()) return status;
  }
  return loader.Finish();
}

// Restores the minimum fill of `parent`'s child `at` after an erase left it
// short: borrow one entry from the left sibling, else from the right, else
// merge with a sibling, which takes a separator out of `parent`.
Status BPlusTree::Rebalance(Node* parent, size_t at) {
  const size_t minimum = capacity_ / 2;
  Node child(*this), left(*this), right(*this);
  auto s = Read(parent->child(at), &child);
  if (!s.ok()) return s;
  const bool has_left = at > 0, has_right = at < parent->size();
  if (has_left) {
    s = Read(parent->child(at - 1), &left);
    if (!s.ok()) return s;
    if (left.size() > minimum) {
      const size_t last = left.size() - 1;
      if (child.leaf()) {
        child.Insert(0, left.entry(last));
        parent->SetKey(at - 1, child.entry(0));
      } else {
        // Rotate through the parent: its separator comes down, the left
        // sibling's last separator goes up and its last child moves over.
        child.Insert(0, parent->entry(at - 1), child.child(0));
        child.set_first_child(left.child(last + 1));
        parent->SetKey(at - 1, left.entry(last));
      }
      left.Truncate(last);
      s = Write(left);
      if (!s.ok()) return s;
      return Write(child);
    }
  }
  if (has_right) {
    s = Read(parent->child(at + 1), &right);
    if (!s.ok()) return s;
    if (right.size() > minimum) {
      if (child.leaf()) {
        child.Append(right.entry(0));
        right.Erase(0);
        parent->SetKey(at, right.entry(0));
      } else {
        child.Append(parent->entry(at), right.child(0));
        parent->SetKey(at, right.entry(0));
        right.set_first_child(right.child(1));
        right.Erase(0);
      }
      s = Write(right);
      if (!s.ok()) return s;
      return Write(child);
    }
  }
  // Merge the right node of the pair into the left one.
  Node& into = has_left ? left : child;
  const Node& from = has_left ? child : right;
  const size_t separator = has_left ? at - 1 : at;
  if (into.leaf())
    into.set_next(from.next());
  else
    into.Append(parent->entry(separator), from.child(0));
  into.Append(from, 0, from.size());
  s = Write(into);
  if (!s.ok()) return s;
  s = Free(from.id());
  if (!s.ok()) return s;
  parent->Erase(separator);
  return Status::Ok();
}

// `remaining` receives the number of slots left in node `id`. Only nodes that
// change are written: the leaf, and ancestors whose child had to be rebalanced.
Status BPlusTree::EraseNode(page_id_t id, const IndexEntry& entry,
                            size_t* remaining) {
  Node n(*this);
  auto s = Read(id, &n);
  if (!s.ok()) return s;
  if (n.leaf()) {
    const size_t at = n.LowerBound(entry);
    if (at == n.size() || n.Compare(at, entry) != 0)
      return Status(StatusCode::kNotFound, "index entry not found");
    n.Erase(at);
  } else {
    const size_t at = n.ChildIndex(entry);
    size_t child_size = 0;
    s = EraseNode(n.child(at), entry, &child_size);
    *remaining = n.size();
    if (!s.ok() || child_size >= capacity_ / 2) return s;
    s = Rebalance(&n, at);
    if (!s.ok()) return s;
  }
  *remaining = n.size();
  return Write(n);
}

Status BPlusTree::Erase(const Value& key, RID rid) {
  auto s = CheckKey(key);
  if (!s.ok() || key.IsNull()) return s;
  size_t root_size = 0;
  s = EraseNode(root_, {key, rid}, &root_size);
  if (!s.ok()) return s;
  if (root_size == 0) {
    // An internal root left with a single child hands the root to that child.
    Node root(*this);
    s = Read(root_, &root);
    if (!s.ok()) return s;
    if (!root.leaf()) {
      const auto old = root_;
      root_ = root.child(0);
      s = Free(old);
      if (!s.ok()) return s;
    }
  }
  --entries_;
  return SaveMetadata();
}

Result<page_id_t> BPlusTree::FindLeaf(const IndexEntry* entry) {
  page_id_t id = root_;
  Node n(*this);
  // Bounded so a corrupt child pointer cannot loop forever.
  for (size_t depth = 0; depth < 64; ++depth) {
    auto status = Read(id, &n);
    if (!status.ok()) return status;
    if (n.leaf()) return id;
    id = n.child(entry ? n.ChildIndex(*entry) : 0);
  }
  return Corrupt("tree depth/cycle");
}

Status BPlusTree::Scan(CompareOp op, const Value& key,
                       const std::function<bool(const IndexEntry&)>& visitor) {
  if (key.IsNull()) return Status::Ok();  // NULL compares true with nothing
  if (string_keys_
          ? key.kind != ValueKind::kString
          : key.kind != ValueKind::kInt32 && key.kind != ValueKind::kInt64)
    return Status(StatusCode::kTypeMismatch, "wrong index bound type");
  // Operators bounded below start at the key's leaf, the rest at the leftmost
  // leaf; operators bounded above stop at the first entry past the key.
  const bool bounded_below =
      op == CompareOp::kEq || op == CompareOp::kGt || op == CompareOp::kGe;
  const bool bounded_above =
      op == CompareOp::kEq || op == CompareOp::kLt || op == CompareOp::kLe;
  const IndexEntry lowest{key, {0, 0}};
  auto leaf = FindLeaf(bounded_below ? &lowest : nullptr);
  if (!leaf.ok()) return leaf.status();
  Node n(*this);
  for (page_id_t id = leaf.value(); id != kInvalidPageId; id = n.next()) {
    // The copy is not pinned, so the visitor is free to use the buffer pool.
    auto status = Read(id, &n);
    if (!status.ok()) return status;
    for (size_t i = bounded_below ? n.LowerBound(lowest) : 0; i < n.size();
         ++i) {
      const int cmp = n.CompareKey(i, key);
      if (Satisfies(cmp, op)) {
        if (!visitor(n.entry(i))) return Status::Ok();
      } else if (bounded_above && cmp >= 0) {
        return Status::Ok();
      }
    }
  }
  return Status::Ok();
}

Result<std::vector<RID>> BPlusTree::Find(const Value& key) {
  std::vector<RID> result;
  auto s = Scan(CompareOp::kEq, key, [&](const IndexEntry& entry) {
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
    Node node(*this);
    auto status = Read(id, &node);
    if (!status.ok()) return status;
    if (!node.KeyLengthsValid()) return Corrupt("invalid string key length");
    const std::vector<IndexEntry> keys = node.entries();
    ++stats.nodes;
    if (id != root_ && keys.size() < capacity_ / 2)
      return Corrupt("underfull node");
    if (id == root_ && !node.leaf() && keys.empty())
      return Corrupt("empty internal root");
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i && CompareEntries(keys[i - 1], keys[i]) >= 0)
        return Corrupt("unsorted keys");
      if (low && CompareEntries(keys[i], *low) < 0)
        return Corrupt("key below fence");
      if (high && CompareEntries(keys[i], *high) >= 0)
        return Corrupt("key above fence");
    }
    if (node.leaf()) {
      if (stats.height && stats.height != depth)
        return Corrupt("unbalanced leaf depths");
      stats.height = depth;
      ++stats.leaves;
      stats.entries += keys.size();
      leaves.push_back(id);
      for (const auto& e : keys) {
        if (previous && CompareEntries(*previous, e) >= 0)
          return Corrupt("leaf order");
        previous = e;
      }
    } else {
      for (size_t i = 0; i <= keys.size(); ++i) {
        auto s =
            visit(node.child(i), depth + 1,
                  i ? std::optional<IndexEntry>(keys[i - 1]) : low,
                  i < keys.size() ? std::optional<IndexEntry>(keys[i]) : high);
        if (!s.ok()) return s;
      }
    }
    return Status::Ok();
  };
  auto s = visit(root_, 1, {}, {});
  if (!s.ok()) return s;
  if (stats.entries != entries_) return Corrupt("entry count mismatch");
  for (size_t i = 0; i < leaves.size(); ++i) {
    Node n(*this);
    auto status = Read(leaves[i], &n);
    if (!status.ok()) return status;
    if (n.next() != (i + 1 < leaves.size() ? leaves[i + 1] : kInvalidPageId))
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
    Node n(*this);
    auto loaded = Read(id, &n);
    if (!loaded.ok()) return loaded;
    const std::vector<IndexEntry> keys = n.entries();
    out << "  p" << id << " [label=\"" << (n.leaf() ? "Leaf" : "Internal")
        << " p" << id << " (" << keys.size() << " keys)";
    for (size_t i = 0; i < std::min<size_t>(keys.size(), 6); ++i) {
      const auto& e = keys[i];
      auto key = ValueToString(e.key);
      if (key.size() > 40) key = key.substr(0, 40) + "...";
      out << "\\n"
          << DotEscape(key) << " @" << e.rid.page_id << ":" << e.rid.slot;
    }
    if (keys.size() > 6) out << "\\n...";
    out << "\"];\n";
    for (size_t i = 0; !n.leaf() && i <= keys.size(); ++i) {
      out << "  p" << id << " -> p" << n.child(i) << ";\n";
      pending.push(n.child(i));
    }
    if (n.leaf() && n.next() != kInvalidPageId)
      out << "  p" << id << " -> p" << n.next()
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
    Node n(*this);
    auto loaded = Read(id, &n);
    if (!loaded.ok()) return loaded;
    const std::vector<IndexEntry> keys = n.entries();
    out << prefix << (last ? "`-- " : "|-- ")
        << (n.leaf() ? "LEAF " : "INTERNAL ") << "p" << id << " ["
        << keys.size() << "] ";
    for (size_t i = 0; i < std::min<size_t>(keys.size(), 4); ++i) {
      if (i) out << " | ";
      auto key = ValueToString(keys[i].key);
      if (key.size() > 32) key = key.substr(0, 32) + "...";
      out << std::quoted(DotEscape(key)) << "@" << keys[i].rid.page_id << ":"
          << keys[i].rid.slot;
    }
    if (keys.size() > 4) out << " | ...";
    if (n.leaf())
      out << " -> "
          << (n.next() == kInvalidPageId ? "END"
                                         : "p" + std::to_string(n.next()));
    out << "\n";
    for (size_t i = 0; !n.leaf() && i <= keys.size(); ++i) {
      auto status =
          draw(n.child(i), prefix + (last ? "    " : "|   "), i == keys.size());
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
