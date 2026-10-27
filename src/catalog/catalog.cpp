#include "catalog/catalog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

#include "common/logger.h"
#include "common/util.h"
#include "space/segment.h"

namespace minidb {
namespace {

std::vector<Value> DecodeRow(const TableHeap& heap, const Tuple& tuple) {
  auto decoded = Tuple::Decode(heap.schema(), tuple.data().data(), tuple.size());
  if (!decoded.ok()) return {};
  return decoded.TakeValue();
}

std::string SanitizeFileStem(const std::string& table_name) {
  std::string stem;
  stem.reserve(table_name.size());
  for (const char ch : table_name) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0 || ch == '_') {
      stem.push_back(static_cast<char>(std::tolower(uch)));
    } else {
      stem.push_back('_');
    }
  }
  if (stem.empty()) stem = "table";
  if (stem.size() > 48) stem.resize(48);
  return stem;
}

bool IsSafeFileName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  const std::filesystem::path path(name);
  return !path.has_parent_path() && path.filename().string() == name;
}

}  // namespace

Schema Catalog::SysTablesSchema() {
  std::vector<Column> columns;
  columns.push_back({"table_id", TypeId::kInt32, 0, 0, false});
  columns.push_back({"name", TypeId::kVarchar, 64, 0, false});
  columns.push_back({"data_file", TypeId::kVarchar, 128, 0, false});
  columns.push_back({"fsm_file", TypeId::kVarchar, 128, 0, false});
  columns.push_back({"segment_header_page", TypeId::kInt32, 0, 0, false});
  columns.push_back({"column_count", TypeId::kInt32, 0, 0, false});
  columns.push_back({"is_system", TypeId::kInt32, 0, 0, false});
  return Schema::Make(std::move(columns)).value();
}

Schema Catalog::SysColumnsSchema() {
  std::vector<Column> columns;
  columns.push_back({"table_id", TypeId::kInt32, 0, 0, false});
  columns.push_back({"ordinal", TypeId::kInt32, 0, 0, false});
  columns.push_back({"name", TypeId::kVarchar, 64, 0, false});
  columns.push_back({"type", TypeId::kInt32, 0, 0, false});
  columns.push_back({"length", TypeId::kInt32, 0, 0, false});
  columns.push_back({"scale", TypeId::kInt32, 0, 0, false});
  columns.push_back({"nullable", TypeId::kInt32, 0, 0, false});
  return Schema::Make(std::move(columns)).value();
}

std::string Catalog::FilePath(const std::string& file_name) const {
  return (std::filesystem::path(directory_) / file_name).string();
}

std::string Catalog::ChooseFileName(const std::string& table_name,
                                    const std::string& suffix,
                                    uint32_t table_id) const {
  const std::string stem = SanitizeFileStem(table_name);
  std::string candidate = stem + suffix;
  if (!std::filesystem::exists(FilePath(candidate))) return candidate;
  candidate = stem + "_" + std::to_string(table_id) + suffix;
  uint32_t counter = 1;
  while (std::filesystem::exists(FilePath(candidate))) {
    candidate = stem + "_" + std::to_string(table_id) + "_" +
                std::to_string(counter++) + suffix;
  }
  return candidate;
}

Result<std::unique_ptr<Catalog>> Catalog::Bootstrap(
    const std::string& directory, BufferPool& buffer_pool, LogManager& log) {
  auto catalog = std::unique_ptr<Catalog>(
      new Catalog(directory, buffer_pool, log));

  Transaction txn(1);
  const Schema tables_schema = SysTablesSchema();
  const Schema columns_schema = SysColumnsSchema();

  auto sys_tables = catalog->CreateRelation(
      &txn, kSysTablesId, "sys_tables", "sys_tables.dat", "sys_tables.fsm",
      tables_schema, /*is_system=*/true);
  if (!sys_tables.ok()) return sys_tables.status();
  auto sys_columns = catalog->CreateRelation(
      &txn, kSysColumnsId, "sys_columns", "sys_columns.dat",
      "sys_columns.fsm", columns_schema, /*is_system=*/true);
  if (!sys_columns.ok()) return sys_columns.status();

  catalog->sys_tables_heap_ = sys_tables.value()->heap.get();
  catalog->sys_columns_heap_ = sys_columns.value()->heap.get();
  catalog->next_table_id_ = kSysColumnsId + 1;

  Status status = catalog->InsertSysTableRow(&txn, *sys_tables.value(),
                                             sys_tables.value()->segment->header_page());
  if (!status.ok()) return status;
  status = catalog->InsertSysTableRow(&txn, *sys_columns.value(),
                                      sys_columns.value()->segment->header_page());
  if (!status.ok()) return status;

  status = catalog->InsertSysColumnRows(&txn, kSysTablesId, tables_schema);
  if (!status.ok()) return status;
  status = catalog->InsertSysColumnRows(&txn, kSysColumnsId, columns_schema);
  if (!status.ok()) return status;

  txn.SetState(TxnState::kCommitted);
  MINIDB_LOG_INFO("CAT") << "catalog bootstrapped in " << directory;
  return catalog;
}

Result<std::unique_ptr<Catalog>> Catalog::Open(const std::string& directory,
                                               BufferPool& buffer_pool,
                                               LogManager& log) {
  auto catalog =
      std::unique_ptr<Catalog>(new Catalog(directory, buffer_pool, log));

  Transaction txn(1);
  auto sys_tables = catalog->OpenRelation(
      &txn, kSysTablesId, "sys_tables", "sys_tables.dat", "sys_tables.fsm",
      0, SysTablesSchema(), /*is_system=*/true);
  if (!sys_tables.ok()) return sys_tables.status();
  auto sys_columns = catalog->OpenRelation(
      &txn, kSysColumnsId, "sys_columns", "sys_columns.dat",
      "sys_columns.fsm", 0, SysColumnsSchema(), /*is_system=*/true);
  if (!sys_columns.ok()) return sys_columns.status();

  catalog->sys_tables_heap_ = sys_tables.value()->heap.get();
  catalog->sys_columns_heap_ = sys_columns.value()->heap.get();

  const Status status = catalog->LoadSystemCatalog();
  if (!status.ok()) return status;
  MINIDB_LOG_INFO("CAT") << "catalog opened in " << directory << " with "
                         << catalog->tables_.size() << " relation(s)";
  return catalog;
}

Result<TableInfo*> Catalog::OpenRelation(Transaction* /*txn*/,
                                         uint32_t table_id,
                                         std::string name,
                                         std::string data_file,
                                         std::string fsm_file,
                                         page_id_t segment_page, Schema schema,
                                         bool is_system) {
  if (!IsSafeFileName(data_file) || !IsSafeFileName(fsm_file)) {
    return Status(StatusCode::kCorruption,
                  "catalog contains an unsafe relation file name");
  }

  auto data_disk =
      DiskManager::Open(FilePath(data_file), FileKind::kData);
  if (!data_disk.ok()) return data_disk.status();
  auto fsm_disk = DiskManager::Open(FilePath(fsm_file), FileKind::kFsm);
  if (!fsm_disk.ok()) return fsm_disk.status();

  auto info = std::make_unique<TableInfo>();
  info->table_id = table_id;
  info->name = std::move(name);
  info->is_system = is_system;
  info->data_file = std::move(data_file);
  info->fsm_file = std::move(fsm_file);
  info->schema = std::move(schema);
  info->data_disk = data_disk.TakeValue();
  info->fsm_disk = fsm_disk.TakeValue();
  info->fsm =
      std::make_shared<FreeSpaceMap>(info->fsm_disk, buffer_pool_);
  info->space = std::make_unique<SpaceManager>(info->data_disk, buffer_pool_,
                                               log_);

  // Segment headers are recorded in sys_tables. The system relations are
  // opened first, so their header pages come from the data file header.
  const page_id_t actual_segment_page =
      segment_page == 0 ? info->data_disk->header().segment_header_page
                        : segment_page;
  if (actual_segment_page == kInvalidPageId || actual_segment_page == 0) {
    return Status(StatusCode::kCorruption,
                  "relation " + info->name + " has no segment header page");
  }

  auto segment =
      info->space->OpenSegment(actual_segment_page, *info->fsm);
  if (!segment.ok()) return segment.status();
  info->segment = std::shared_ptr<Segment>(segment.TakeValue().release());
  info->heap = std::make_unique<TableHeap>(info->segment, info->schema,
                                           buffer_pool_, log_);

  TableInfo* raw = info.get();
  tables_[info->name] = std::move(info);
  return raw;
}

Result<TableInfo*> Catalog::CreateRelation(Transaction* txn, uint32_t table_id,
                                           std::string name,
                                           std::string data_file,
                                           std::string fsm_file, Schema schema,
                                           bool is_system) {
  const std::string data_path = FilePath(data_file);
  const std::string fsm_path = FilePath(fsm_file);

  auto data_disk = DiskManager::Create(data_path, FileKind::kData, table_id);
  if (!data_disk.ok()) return data_disk.status();
  auto fsm_disk = DiskManager::Create(fsm_path, FileKind::kFsm, table_id);
  if (!fsm_disk.ok()) {
    std::error_code ec;
    std::filesystem::remove(data_path, ec);
    return fsm_disk.status();
  }

  auto info = std::make_unique<TableInfo>();
  info->table_id = table_id;
  info->name = std::move(name);
  info->is_system = is_system;
  info->data_file = std::move(data_file);
  info->fsm_file = std::move(fsm_file);
  info->schema = std::move(schema);
  info->data_disk = data_disk.TakeValue();
  info->fsm_disk = fsm_disk.TakeValue();
  info->fsm =
      std::make_shared<FreeSpaceMap>(info->fsm_disk, buffer_pool_);
  info->space = std::make_unique<SpaceManager>(info->data_disk, buffer_pool_,
                                               log_);

  const auto cleanup_files = [&]() {
    buffer_pool_.DiscardFile(info->fsm_disk->file_id());
    buffer_pool_.DiscardFile(info->data_disk->file_id());
    std::error_code ec;
    std::filesystem::remove(fsm_path, ec);
    std::filesystem::remove(data_path, ec);
  };

  auto extent = info->space->AllocateExtent(txn);
  if (!extent.ok()) {
    cleanup_files();
    return extent.status();
  }
  auto segment = info->space->CreateSegment(
      txn, static_cast<segment_id_t>(table_id), SegmentType::kHeap,
      extent.value(), *info->fsm);
  if (!segment.ok()) {
    cleanup_files();
    return segment.status();
  }
  info->segment = std::shared_ptr<Segment>(segment.TakeValue().release());
  info->heap = std::make_unique<TableHeap>(info->segment, info->schema,
                                           buffer_pool_, log_);

  info->data_disk->header().segment_header_page = info->segment->header_page();
  info->data_disk->header().clean_shutdown = 0;
  info->fsm_disk->header().clean_shutdown = 0;
  Status status = info->data_disk->PersistHeader();
  if (!status.ok()) {
    cleanup_files();
    return status;
  }
  status = info->fsm_disk->PersistHeader();
  if (!status.ok()) {
    cleanup_files();
    return status;
  }

  TableInfo* raw = info.get();
  tables_[info->name] = std::move(info);
  return raw;
}

Status Catalog::LoadSystemCatalog() {
  std::unordered_map<uint32_t, std::vector<Column>> columns_by_table;

  auto columns_iterator = sys_columns_heap_->Scan(nullptr);
  RID rid;
  Tuple tuple;
  while (columns_iterator.Next(&rid, &tuple)) {
    const std::vector<Value> row = DecodeRow(*sys_columns_heap_, tuple);
    if (row.size() != sys_columns_heap_->schema().num_columns()) {
      return Status(StatusCode::kCorruption, "bad sys_columns row");
    }
    const uint32_t table_id = static_cast<uint32_t>(row[0].int32_value);
    const int32_t ordinal = row[1].int32_value;
    if (ordinal < 0 || ordinal >= 4096) {
      return Status(StatusCode::kCorruption, "bad column ordinal");
    }
    auto& columns = columns_by_table[table_id];
    if (columns.size() <= static_cast<size_t>(ordinal)) {
      columns.resize(static_cast<size_t>(ordinal) + 1);
    }
    Column column;
    column.name = row[2].string_value;
    column.type = static_cast<TypeId>(row[3].int32_value);
    column.length = static_cast<uint16_t>(row[4].int32_value);
    column.scale = static_cast<uint8_t>(row[5].int32_value);
    column.nullable = row[6].int32_value != 0;
    if (column.name.empty()) {
      return Status(StatusCode::kCorruption, "empty column name in catalog");
    }
    columns[static_cast<size_t>(ordinal)] = std::move(column);
  }
  if (!columns_iterator.status().ok()) return columns_iterator.status();

  bool saw_sys_tables = false;
  bool saw_sys_columns = false;
  uint32_t max_table_id = kSysColumnsId;

  auto tables_iterator = sys_tables_heap_->Scan(nullptr);
  while (tables_iterator.Next(&rid, &tuple)) {
    const std::vector<Value> row = DecodeRow(*sys_tables_heap_, tuple);
    if (row.size() != sys_tables_heap_->schema().num_columns()) {
      return Status(StatusCode::kCorruption, "bad sys_tables row");
    }
    const uint32_t table_id = static_cast<uint32_t>(row[0].int32_value);
    const std::string table_name = ToLower(row[1].string_value);
    const std::string data_file = row[2].string_value;
    const std::string fsm_file = row[3].string_value;
    const page_id_t segment_page = static_cast<page_id_t>(row[4].int32_value);
    const int32_t column_count = row[5].int32_value;
    const bool is_system = row[6].int32_value != 0;

    if (table_id <= kSysColumnsId) {
      const bool expected_name =
          (table_id == kSysTablesId && table_name == "sys_tables") ||
          (table_id == kSysColumnsId && table_name == "sys_columns");
      if (!expected_name || !is_system) {
        return Status(StatusCode::kCorruption,
                      "bad system relation row in sys_tables");
      }
      saw_sys_tables |= table_id == kSysTablesId;
      saw_sys_columns |= table_id == kSysColumnsId;
      continue;
    }

    max_table_id = std::max(max_table_id, table_id);
    if (tables_.count(table_name) != 0) {
      return Status(StatusCode::kCorruption,
                    "duplicate relation " + table_name + " in catalog");
    }
    auto found = columns_by_table.find(table_id);
    if (found == columns_by_table.end()) {
      return Status(StatusCode::kCorruption,
                    "table " + table_name + " has no columns");
    }
    if (column_count != static_cast<int32_t>(found->second.size())) {
      return Status(StatusCode::kCorruption,
                    "column count mismatch for table " + table_name);
    }
    auto schema = Schema::Make(found->second);
    if (!schema.ok()) return schema.status();

    auto relation = OpenRelation(nullptr, table_id, table_name, data_file,
                                 fsm_file, segment_page, schema.TakeValue(),
                                 is_system);
    if (!relation.ok()) return relation.status();
  }
  if (!tables_iterator.status().ok()) return tables_iterator.status();

  if (!saw_sys_tables || !saw_sys_columns) {
    return Status(StatusCode::kCorruption,
                  "system relation rows are missing from sys_tables");
  }
  next_table_id_ = max_table_id + 1;
  return Status::Ok();
}

Status Catalog::InsertSysTableRow(Transaction* txn, const TableInfo& table,
                                  page_id_t segment_page) {
  const std::vector<Value> values = {
      Value::Int32(static_cast<int32_t>(table.table_id)),
      Value::String(table.name),
      Value::String(table.data_file),
      Value::String(table.fsm_file),
      Value::Int32(static_cast<int32_t>(segment_page)),
      Value::Int32(static_cast<int32_t>(table.schema.num_columns())),
      Value::Int32(table.is_system ? 1 : 0)};
  auto tuple = Tuple::FromValues(sys_tables_heap_->schema(), values);
  if (!tuple.ok()) return tuple.status();
  auto inserted = sys_tables_heap_->Insert(txn, tuple.value());
  return inserted.ok() ? Status::Ok() : inserted.status();
}

Status Catalog::InsertSysColumnRows(Transaction* txn, uint32_t table_id,
                                    const Schema& schema) {
  for (size_t i = 0; i < schema.num_columns(); ++i) {
    const Column& column = schema.GetColumn(i);
    auto tuple = Tuple::FromValues(
        sys_columns_heap_->schema(),
        {Value::Int32(static_cast<int32_t>(table_id)),
         Value::Int32(static_cast<int32_t>(i)), Value::String(column.name),
         Value::Int32(static_cast<int32_t>(column.type)),
         Value::Int32(column.length), Value::Int32(column.scale),
         Value::Int32(column.nullable ? 1 : 0)});
    if (!tuple.ok()) return tuple.status();
    auto inserted = sys_columns_heap_->Insert(txn, tuple.value());
    if (!inserted.ok()) return inserted.status();
  }
  return Status::Ok();
}

Result<TableInfo*> Catalog::CreateTable(Transaction* txn, std::string name,
                                        Schema schema) {
  name = ToLower(std::move(name));
  if (name.empty()) {
    return Status(StatusCode::kInvalidArgument, "table name is empty");
  }
  if (name.rfind("sys_", 0) == 0) {
    return Status(StatusCode::kInvalidArgument,
                  "table names starting with sys_ are reserved");
  }
  if (tables_.count(name) != 0) {
    return Status(StatusCode::kAlreadyExists,
                  "table " + name + " already exists");
  }
  if (schema.num_columns() == 0) {
    return Status(StatusCode::kInvalidArgument,
                  "CREATE TABLE requires at least one column");
  }
  std::vector<std::string> seen;
  for (const Column& column : schema.columns()) {
    const std::string column_name = ToLower(column.name);
    if (std::find(seen.begin(), seen.end(), column_name) != seen.end()) {
      return Status(StatusCode::kInvalidArgument,
                    "duplicate column " + column_name);
    }
    seen.push_back(column_name);
  }

  const uint32_t table_id = next_table_id_;
  const std::string data_file = ChooseFileName(name, ".dat", table_id);
  const std::string fsm_file = ChooseFileName(name, ".fsm", table_id);

  auto relation = CreateRelation(txn, table_id, name, data_file, fsm_file,
                                 schema, /*is_system=*/false);
  if (!relation.ok()) return relation.status();
  TableInfo* table = relation.value();

  Status status = InsertSysTableRow(txn, *table, table->segment->header_page());
  if (status.ok()) {
    status = InsertSysColumnRows(txn, table_id, table->schema);
  }
  if (!status.ok()) {
    const Status rollback = DropTable(txn, name);
    if (!rollback.ok()) {
      MINIDB_LOG_ERROR("CAT") << "rollback of CREATE TABLE " << name
                              << " failed: " << rollback.message();
    }
    return status;
  }

  ++next_table_id_;
  MINIDB_LOG_INFO("CAT") << "CREATE TABLE " << name << " (id=" << table_id
                         << ", data=" << data_file << ", fsm=" << fsm_file
                         << ", segment_page=" << table->segment->header_page()
                         << ")";
  return table;
}

Result<TableInfo*> Catalog::GetTable(std::string_view name) {
  const std::string key = ToLower(std::string(name));
  auto found = tables_.find(key);
  if (found == tables_.end()) {
    return Status(StatusCode::kNotFound, "table " + key + " does not exist");
  }
  return found->second.get();
}

Status Catalog::DropTable(Transaction* txn, std::string_view name) {
  const std::string key = ToLower(std::string(name));
  auto found = tables_.find(key);
  if (found == tables_.end()) {
    return Status(StatusCode::kNotFound, "table " + key + " does not exist");
  }
  if (found->second->is_system) {
    return Status(StatusCode::kInvalidArgument,
                  "system table " + key + " cannot be dropped");
  }
  const uint32_t table_id = found->second->table_id;
  const uint32_t data_file_id = found->second->data_disk->file_id();
  const uint32_t fsm_file_id = found->second->fsm_disk->file_id();
  const std::string data_path = FilePath(found->second->data_file);
  const std::string fsm_path = FilePath(found->second->fsm_file);

  std::vector<RID> table_rows;
  {
    auto iterator = sys_tables_heap_->Scan(nullptr);
    RID rid;
    Tuple tuple;
    while (iterator.Next(&rid, &tuple)) {
      const std::vector<Value> row = DecodeRow(*sys_tables_heap_, tuple);
      if (!row.empty() &&
          static_cast<uint32_t>(row[0].int32_value) == table_id) {
        table_rows.push_back(rid);
      }
    }
    if (!iterator.status().ok()) return iterator.status();
  }
  std::vector<RID> column_rows;
  {
    auto iterator = sys_columns_heap_->Scan(nullptr);
    RID rid;
    Tuple tuple;
    while (iterator.Next(&rid, &tuple)) {
      const std::vector<Value> row = DecodeRow(*sys_columns_heap_, tuple);
      if (!row.empty() &&
          static_cast<uint32_t>(row[0].int32_value) == table_id) {
        column_rows.push_back(rid);
      }
    }
    if (!iterator.status().ok()) return iterator.status();
  }

  for (const RID& rid : table_rows) {
    const Status status = sys_tables_heap_->Delete(txn, rid);
    if (!status.ok()) return status;
  }
  for (const RID& rid : column_rows) {
    const Status status = sys_columns_heap_->Delete(txn, rid);
    if (!status.ok()) return status;
  }

  const Status drop = found->second->segment->Drop(txn);
  if (!drop.ok()) return drop;

  tables_.erase(found);
  const Status discard_data = buffer_pool_.DiscardFile(data_file_id);
  const Status discard_fsm = buffer_pool_.DiscardFile(fsm_file_id);
  std::error_code ec;
  std::filesystem::remove(data_path, ec);
  std::filesystem::remove(fsm_path, ec);
  if (!discard_data.ok()) return discard_data;
  if (!discard_fsm.ok()) return discard_fsm;

  MINIDB_LOG_INFO("CAT") << "DROP TABLE " << key;
  return Status::Ok();
}

std::vector<TableInfo*> Catalog::Tables() const {
  std::vector<TableInfo*> result;
  result.reserve(tables_.size());
  for (const auto& entry : tables_) result.push_back(entry.second.get());
  std::sort(result.begin(), result.end(),
            [](const TableInfo* a, const TableInfo* b) {
              return a->name < b->name;
            });
  return result;
}

Status Catalog::FlushAllSegments(Transaction* txn) {
  for (auto& entry : tables_) {
    const Status status = entry.second->heap->Flush(txn);
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

Status Catalog::BeginSession(bool existing_database) {
  for (auto& entry : tables_) {
    TableInfo& table = *entry.second;
    if (existing_database && table.data_disk->header().clean_shutdown == 0) {
      MINIDB_LOG_WARN("DB") << "relation " << table.name
                            << " was not shut down cleanly";
    }
    table.data_disk->header().clean_shutdown = 0;
    Status status = table.data_disk->PersistHeader();
    if (!status.ok()) return status;
    table.fsm_disk->header().clean_shutdown = 0;
    status = table.fsm_disk->PersistHeader();
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

Status Catalog::SetCleanShutdown(bool clean) {
  for (auto& entry : tables_) {
    TableInfo& table = *entry.second;
    table.data_disk->header().clean_shutdown = clean ? 1 : 0;
    Status status = table.data_disk->PersistHeader();
    if (!status.ok()) return status;
    status = table.data_disk->Sync();
    if (!status.ok()) return status;

    table.fsm_disk->header().clean_shutdown = clean ? 1 : 0;
    status = table.fsm_disk->PersistHeader();
    if (!status.ok()) return status;
    status = table.fsm_disk->Sync();
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

}  // namespace minidb
