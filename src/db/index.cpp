// SQL index DDL, persistent discovery and tuple-to-index maintenance.
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "common/util.h"
#include "db/database.h"

namespace minidb {
namespace {
std::string IndexFileName(const std::string& name) {
  // Quoted SQL identifiers can contain slashes; never use them as paths.
  std::ostringstream out;
  out << "i_" << std::hex << std::setfill('0');
  for (unsigned char c : name) out << std::setw(2) << static_cast<unsigned>(c);
  return out.str() + ".idx";
}
}  // namespace

Status Database::LoadIndexes() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(config_.path) / "indexes";
  std::error_code ec;
  const bool exists = fs::exists(dir, ec);
  if (ec) return Status(StatusCode::kIoError, ec.message());
  if (!exists) return Status::Ok();
  fs::directory_iterator it(dir, ec), end;
  if (ec) return Status(StatusCode::kIoError, ec.message());
  for (; it != end; it.increment(ec)) {
    if (ec) return Status(StatusCode::kIoError, ec.message());
    if (it->path().extension() != ".idx") continue;
    auto opened = BPlusTree::Open(*pool_, it->path().string());
    if (!opened.ok()) return opened.status();
    auto tree = opened.TakeValue();
    const auto& m = tree->metadata();
    auto table = catalog_->GetTable(m.table);
    if (!table.ok() || table.value()->table_id != m.table_id ||
        table.value()->is_system ||
        m.column >= table.value()->schema.num_columns())
      return Status(StatusCode::kCorruption,
                    "index has invalid table/column: " + m.name);
    const auto& column = table.value()->schema.GetColumn(m.column);
    if (column.type != m.type || column.length != m.length ||
        it->path().filename() != IndexFileName(m.name) ||
        indexes_.count(m.name))
      return Status(StatusCode::kCorruption,
                    "index metadata mismatch: " + m.name);
    indexes_.emplace(m.name, std::move(tree));
  }
  if (ec) return Status(StatusCode::kIoError, ec.message());
  return Status::Ok();
}

Status Database::ExecuteCreateIndex(Statement* statement, Transaction* txn,
                                    QueryResult* result) {
  namespace fs = std::filesystem;
  const std::string name = ToLower(statement->index_name);
  if (indexes_.count(name)) {
    if (statement->if_not_exists) {
      result->message = "index " + name + " already exists, skipped";
      return Status::Ok();
    }
    return Status(StatusCode::kAlreadyExists, "index already exists: " + name);
  }
  // The hex file name must fit typical NAME_MAX (255 bytes).
  if (name.empty() || name.size() > 100)
    return Status(StatusCode::kInvalidArgument,
                  "index name must be 1..100 bytes");
  auto found = catalog_->GetTable(statement->table_name);
  if (!found.ok()) return found.status();
  TableInfo* table = found.value();
  if (table->is_system)
    return Status(StatusCode::kInvalidArgument, "cannot index a system table");
  const int ordinal = table->schema.IndexOf(statement->index_column);
  if (ordinal < 0)
    return Status(StatusCode::kNotFound, "index column does not exist");
  const auto& column = table->schema.GetColumn(ordinal);
  IndexMetadata m{name,
                  table->name,
                  table->table_id,
                  static_cast<uint32_t>(ordinal),
                  column.type,
                  column.length};
  auto s = BPlusTree::ValidateMetadata(m);
  if (!s.ok()) return s;
  const fs::path path =
      fs::path(config_.path) / "indexes" / IndexFileName(name);
  const std::string temporary = path.string() + ".building";
  std::error_code ec;
  // Only unfinished builds use this suffix; completed indexes are .idx files.
  fs::remove(temporary, ec);
  if (ec) return Status(StatusCode::kIoError, ec.message());
  auto created = BPlusTree::Create(*pool_, temporary, m);
  if (!created.ok()) return created.status();
  auto tree = created.TakeValue();
  const auto cleanup = [&](Status failure) {
    pool_->DiscardFile(tree->disk()->file_id());
    tree.reset();
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return failure;
  };
  size_t sort_runs = 0, merge_passes = 0;
  {
    BPlusTree::BulkLoader loader(*tree, config_.index_sort_memory);
    auto iterator = table->heap->Scan(txn);
    RID rid;
    Tuple tuple;
    while (iterator.Next(&rid, &tuple)) {
      auto values =
          Tuple::Decode(table->schema, tuple.data().data(), tuple.size());
      if (!values.ok()) return cleanup(values.status());
      s = loader.Add(values.value()[ordinal], rid);
      if (!s.ok()) return cleanup(s);
    }
    if (!iterator.status().ok()) return cleanup(iterator.status());
    s = loader.Finish();
    if (!s.ok()) return cleanup(s);
    sort_runs = loader.runs();
    merge_passes = loader.merge_passes() + 1;
  }
  const auto count = tree->size();
  s = pool_->FlushAll();
  if (!s.ok()) return cleanup(s);
  s = pool_->DiscardFile(tree->disk()->file_id());
  if (!s.ok()) return cleanup(s);
  tree.reset();
  fs::rename(temporary, path, ec);
  if (ec) return Status(StatusCode::kIoError, "publish index: " + ec.message());
  auto opened = BPlusTree::Open(*pool_, path.string());
  if (!opened.ok()) return opened.status();
  indexes_.emplace(name, opened.TakeValue());
  result->message =
      "CREATE INDEX " + name + " (B+ tree, " + std::to_string(count) +
      " entries, " +
      (sort_runs ? "external sort: " + std::to_string(sort_runs) +
                       " runs merged in " + std::to_string(merge_passes) +
                       (merge_passes == 1 ? " pass" : " passes")
                 : std::string("sorted in memory")) +
      ")";
  return Status::Ok();
}

Status Database::DropIndex(const std::string& name) {
  auto it = indexes_.find(name);
  if (it == indexes_.end())
    return Status(StatusCode::kNotFound, "index not found: " + name);
  auto s = pool_->DiscardFile(it->second->disk()->file_id());
  if (!s.ok()) return s;
  std::error_code ec;
  std::filesystem::remove(it->second->disk()->path(), ec);
  if (ec) return Status(StatusCode::kIoError, "remove index: " + ec.message());
  indexes_.erase(it);
  return Status::Ok();
}

Status Database::ExecuteDropIndex(Statement* statement, QueryResult* result) {
  const auto name = ToLower(statement->index_name);
  if (statement->if_exists && !indexes_.count(name)) {
    result->message = "index " + name + " does not exist, skipped";
    return Status::Ok();
  }
  auto s = DropIndex(name);
  if (s.ok()) result->message = "DROP INDEX " + name;
  return s;
}

Status Database::DropTableIndexes(const TableInfo& table) {
  std::vector<std::string> names;
  for (const auto& [name, tree] : indexes_)
    if (tree->metadata().table_id == table.table_id) names.push_back(name);
  for (const auto& name : names) {
    auto s = DropIndex(name);
    if (!s.ok()) return s;
  }
  return Status::Ok();
}

bool Database::HasIndexes(const TableInfo& table) const {
  for (const auto& [name, tree] : indexes_)
    if (tree->metadata().table_id == table.table_id) return true;
  return false;
}

BPlusTree* Database::FindIndex(const TableInfo& table, size_t column) {
  for (auto& [name, tree] : indexes_)
    if (tree->metadata().table_id == table.table_id &&
        tree->metadata().column == column)
      return tree.get();
  return nullptr;
}

Status Database::MaintainIndexes(const TableInfo& table, RID rid,
                                 const Tuple* before, const Tuple* after) {
  if (!HasIndexes(table)) return Status::Ok();
  std::vector<Value> old_values, new_values;
  if (before) {
    auto decoded =
        Tuple::Decode(table.schema, before->data().data(), before->size());
    if (!decoded.ok()) return decoded.status();
    old_values = decoded.TakeValue();
  }
  if (after) {
    auto decoded =
        Tuple::Decode(table.schema, after->data().data(), after->size());
    if (!decoded.ok()) return decoded.status();
    new_values = decoded.TakeValue();
  }
  for (auto& [name, tree] : indexes_) {
    const auto& m = tree->metadata();
    if (m.table_id != table.table_id) continue;
    if (before && after &&
        old_values[m.column].IsNull() == new_values[m.column].IsNull() &&
        CompareValues(old_values[m.column], new_values[m.column]) == 0)
      continue;
    Status s;
    if (before) s = tree->Erase(old_values[m.column], rid);
    if (s.ok() && after) s = tree->Insert(new_values[m.column], rid);
    if (!s.ok()) {
      // Heap writes have already succeeded (the existing engine has no undo).
      // Remove derived indexes so subsequent queries use authoritative heap
      // data.
      const auto cause = s.message();
      const auto dropped = DropTableIndexes(table);
      return Status(
          s.code(),
          "index maintenance failed: " + cause +
              (dropped.ok()
                   ? "; table indexes removed, rebuild with CREATE INDEX"
                   : "; index cleanup failed: " + dropped.message()));
    }
  }
  return Status::Ok();
}
}  // namespace minidb
