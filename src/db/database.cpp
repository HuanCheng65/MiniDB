#include "db/database.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include "common/logger.h"
#include "common/util.h"

namespace minidb {
namespace {

constexpr const char* kRidColumn = "rid";
constexpr size_t kRidProjection = static_cast<size_t>(-1);

Result<int64_t> LiteralToInteger(const Literal& literal) {
  switch (literal.kind) {
    case Literal::Kind::kInteger:
      return literal.integer;
    case Literal::Kind::kFloat: {
      if (!std::isfinite(literal.floating) ||
          std::floor(literal.floating) != literal.floating ||
          literal.floating <
              static_cast<double>(std::numeric_limits<int64_t>::min()) ||
          literal.floating >
              static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return Status(StatusCode::kTypeMismatch, "expected an integer value");
      }
      return static_cast<int64_t>(literal.floating);
    }
    case Literal::Kind::kString: {
      const char* begin = literal.text.c_str();
      char* end = nullptr;
      const long long value = std::strtoll(begin, &end, 10);
      if (end == begin || *end != '\0') {
        return Status(StatusCode::kTypeMismatch,
                      "cannot parse '" + literal.text + "' as integer");
      }
      return static_cast<int64_t>(value);
    }
    case Literal::Kind::kNull:
      return Status(StatusCode::kInvalidArgument, "expected an integer value");
  }
  return Status(StatusCode::kTypeMismatch, "expected an integer value");
}

Result<double> LiteralToDouble(const Literal& literal) {
  switch (literal.kind) {
    case Literal::Kind::kInteger:
      return static_cast<double>(literal.integer);
    case Literal::Kind::kFloat:
      return literal.floating;
    case Literal::Kind::kString: {
      const char* begin = literal.text.c_str();
      char* end = nullptr;
      const double value = std::strtod(begin, &end);
      if (end == begin || *end != '\0') {
        return Status(StatusCode::kTypeMismatch,
                      "cannot parse '" + literal.text + "' as number");
      }
      return value;
    }
    case Literal::Kind::kNull:
      return Status(StatusCode::kInvalidArgument, "expected a number");
  }
  return Status(StatusCode::kTypeMismatch, "expected a number");
}

Result<Value> LiteralToValue(const Literal& literal, const Column& column) {
  if (literal.kind == Literal::Kind::kNull) return Value::Null();
  switch (column.type) {
    case TypeId::kInt32: {
      auto value = LiteralToInteger(literal);
      if (!value.ok()) return value.status();
      if (value.value() < std::numeric_limits<int32_t>::min() ||
          value.value() > std::numeric_limits<int32_t>::max()) {
        return Status(StatusCode::kInvalidArgument, "integer out of range");
      }
      return Value::Int32(static_cast<int32_t>(value.value()));
    }
    case TypeId::kInt64: {
      auto value = LiteralToInteger(literal);
      if (!value.ok()) return value.status();
      return Value::Int64(value.value());
    }
    case TypeId::kDouble: {
      auto value = LiteralToDouble(literal);
      if (!value.ok()) return value.status();
      return Value::Double(value.value());
    }
    case TypeId::kDecimal: {
      auto value = ParseDecimal(literal.text, column.scale);
      if (!value.ok()) return value.status();
      return Value::Decimal(value.value(), column.scale);
    }
    case TypeId::kDate: {
      if (literal.kind != Literal::Kind::kString) {
        return Status(StatusCode::kTypeMismatch,
                      "DATE literals must be quoted strings");
      }
      auto value = ParseDate(literal.text);
      if (!value.ok()) return value.status();
      return Value::Date(value.value());
    }
    case TypeId::kChar:
    case TypeId::kVarchar:
      return Value::String(literal.text);
  }
  return Status(StatusCode::kTypeMismatch, "unsupported column type");
}

bool Matches(const Value& value, const std::string& op, const Value& literal) {
  if (value.IsNull() || literal.IsNull()) return false;
  const int cmp = CompareValues(value, literal);
  if (op == "=") return cmp == 0;
  if (op == "<>" || op == "!=") return cmp != 0;
  if (op == "<") return cmp < 0;
  if (op == "<=") return cmp <= 0;
  if (op == ">") return cmp > 0;
  if (op == ">=") return cmp >= 0;
  return false;
}

void AppendMessage(QueryResult* result, const std::string& message) {
  if (message.empty()) return;
  if (!result->message.empty()) result->message.push_back('\n');
  result->message += message;
}

}  // namespace

Result<std::unique_ptr<Database>> Database::Open(const Config& config) {
  Logger::Instance().SetLevel(Logger::ParseLevel(config.log_level));
  if (!config.log_file.empty()) {
    Logger::Instance().SetOutputFile(config.log_file);
  }
  if (config.path.empty()) {
    return Status(StatusCode::kInvalidArgument, "database path is empty");
  }

  namespace fs = std::filesystem;
  const fs::path directory(config.path);
  std::error_code ec;
  if (fs::exists(directory)) {
    if (!fs::is_directory(directory)) {
      return Status(StatusCode::kInvalidArgument,
                    "database path is not a folder: " + config.path);
    }
  } else {
    fs::create_directories(directory, ec);
    if (ec) {
      return Status(StatusCode::kIoError,
                    "cannot create database folder " + config.path + ": " +
                        ec.message());
    }
  }

  const fs::path sys_tables_data = directory / "sys_tables.dat";
  const fs::path sys_columns_data = directory / "sys_columns.dat";
  const fs::path sys_tables_fsm = directory / "sys_tables.fsm";
  const fs::path sys_columns_fsm = directory / "sys_columns.fsm";

  const bool has_tables = fs::exists(sys_tables_data);
  const bool has_columns = fs::exists(sys_columns_data);
  if (has_tables != has_columns) {
    return Status(StatusCode::kCorruption,
                  "database folder has an incomplete system catalog: " +
                      config.path);
  }
  const bool create_new = !has_tables;
  if (create_new) {
    if (!fs::is_empty(directory, ec) || ec) {
      return Status(StatusCode::kCorruption,
                    "database folder is not empty but has no system catalog: " +
                        config.path);
    }
  } else if (!fs::exists(sys_tables_fsm) || !fs::exists(sys_columns_fsm)) {
    return Status(StatusCode::kCorruption,
                  "database folder is missing a system FSM file: " +
                      config.path);
  }

  auto database = std::unique_ptr<Database>(new Database(config));
  database->log_ = std::make_unique<LogManager>();
  BufferConfig buffer_config;
  buffer_config.frames = config.buffer_pool_frames;
  buffer_config.replacer = config.replacer;
  database->pool_ =
      std::make_unique<BufferPool>(*database->log_, buffer_config);
  database->txns_ = std::make_unique<TxnManager>();

  if (create_new) {
    auto catalog = Catalog::Bootstrap(config.path, *database->pool_,
                                      *database->log_);
    if (!catalog.ok()) return catalog.status();
    database->catalog_ = catalog.TakeValue();
    MINIDB_LOG_INFO("DB") << "created new database folder " << config.path;
  } else {
    auto catalog =
        Catalog::Open(config.path, *database->pool_, *database->log_);
    if (!catalog.ok()) return catalog.status();
    database->catalog_ = catalog.TakeValue();
    MINIDB_LOG_INFO("DB") << "opened database folder " << config.path;
  }

  const Status status = database->catalog_->BeginSession(!create_new);
  if (!status.ok()) return status;
  return database;
}

Database::~Database() {
  if (!closed_) {
    const Status status = Close();
    if (!status.ok()) {
      MINIDB_LOG_ERROR("DB") << "error during close: " << status.message();
    }
  }
}

Status Database::Close() {
  if (closed_) return Status::Ok();
  closed_ = true;
  if (!catalog_ || !pool_ || !txns_) return Status::Ok();

  Transaction txn = txns_->Begin();
  Status status = catalog_->FlushAllSegments(&txn);
  if (!status.ok()) return status;
  txns_->Commit(&txn);

  status = pool_->FlushAll();
  if (!status.ok()) return status;

  status = catalog_->SetCleanShutdown(true);
  if (!status.ok()) return status;

  MINIDB_LOG_INFO("DB") << "database closed: hit=" << pool_->Stats().hit
                        << " miss=" << pool_->Stats().miss
                        << " hit_rate=" << pool_->Stats().HitRate() << "%";
  return Status::Ok();
}

Status Database::Execute(const std::string& sql, QueryResult* result) {
  auto parsed = ParseScript(sql);
  if (!parsed.ok()) return parsed.status();

  QueryResult combined;
  bool saw_query = false;
  for (Statement& statement : parsed.value()) {
    Transaction txn = txns_->Begin();
    QueryResult current;
    const Status status = ExecuteStatement(&statement, &txn, &current);
    if (!status.ok()) {
      txns_->Abort(&txn);
      return status;
    }
    txns_->Commit(&txn);

    combined.changes += current.changes;
    if (!current.columns.empty()) {
      combined.columns = current.columns;
      combined.rows = std::move(current.rows);
      saw_query = true;
    }
    AppendMessage(&combined, current.message);
  }
  (void)saw_query;
  if (result != nullptr) *result = std::move(combined);
  return Status::Ok();
}

Status Database::ExecuteStatement(Statement* statement, Transaction* txn,
                                  QueryResult* result) {
  switch (statement->type) {
    case StatementType::kCreateTable:
      return ExecuteCreateTable(statement, txn, result);
    case StatementType::kDropTable:
      return ExecuteDropTable(statement, txn, result);
    case StatementType::kInsert:
      return ExecuteInsert(statement, txn, result);
    case StatementType::kDelete:
      return ExecuteDelete(statement, txn, result);
    case StatementType::kUpdate:
      return ExecuteUpdate(statement, txn, result);
    case StatementType::kCopy:
      return ExecuteCopy(statement, txn, result);
    case StatementType::kPragma:
      return ExecutePragma(statement, txn, result);
    case StatementType::kSelect:
      return ExecuteSelect(statement, txn, result);
    case StatementType::kShowTables:
      return ExecuteShowTables(result);
    case StatementType::kDescribe:
      return ExecuteDescribe(statement, result);
    case StatementType::kEmpty:
      return Status::Ok();
  }
  return Status(StatusCode::kNotSupported, "unsupported statement");
}

Status Database::ExecuteCreateTable(Statement* statement, Transaction* txn,
                                    QueryResult* result) {
  const std::string name = ToLower(statement->table_name);
  const auto existing = catalog_->GetTable(name);
  if (existing.ok()) {
    if (statement->if_not_exists) {
      result->message = "table " + name + " already exists, skipped";
      return Status::Ok();
    }
    return Status(StatusCode::kAlreadyExists,
                  "table " + name + " already exists");
  }

  std::vector<Column> columns;
  columns.reserve(statement->columns.size());
  for (const ColumnSpec& spec : statement->columns) {
    Column column = spec.column;
    column.name = ToLower(column.name);
    columns.push_back(std::move(column));
  }
  auto schema = Schema::Make(std::move(columns));
  if (!schema.ok()) return schema.status();

  auto table = catalog_->CreateTable(txn, name, schema.TakeValue());
  if (!table.ok()) return table.status();
  result->message = "CREATE TABLE " + name + " (" +
                    std::to_string(table.value()->schema.num_columns()) +
                    " columns)";
  return Status::Ok();
}

Status Database::ExecuteDropTable(Statement* statement, Transaction* txn,
                                  QueryResult* result) {
  const std::string name = ToLower(statement->table_name);
  const auto table = catalog_->GetTable(name);
  if (!table.ok()) {
    if (statement->if_exists) {
      result->message = "table " + name + " does not exist, skipped";
      return Status::Ok();
    }
    return table.status();
  }
  if (table.value()->is_system) {
    return Status(StatusCode::kInvalidArgument,
                  "system table " + name + " cannot be dropped");
  }
  const Status status = catalog_->DropTable(txn, name);
  if (!status.ok()) return status;
  result->message = "DROP TABLE " + name;
  return Status::Ok();
}

Status Database::ExecuteInsert(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  auto table_result = catalog_->GetTable(statement->table_name);
  if (!table_result.ok()) return table_result.status();
  TableInfo* table = table_result.value();
  if (table->is_system) {
    return Status(StatusCode::kInvalidArgument,
                  "system table " + table->name + " is read-only");
  }
  const Schema& schema = table->schema;

  std::vector<int> mapping;
  if (statement->insert.columns.empty()) {
    mapping.resize(schema.num_columns());
    for (size_t i = 0; i < schema.num_columns(); ++i) {
      mapping[i] = static_cast<int>(i);
    }
  } else {
    for (const std::string& name : statement->insert.columns) {
      const int index = schema.IndexOf(name);
      if (index < 0) {
        return Status(StatusCode::kNotFound,
                      "column " + name + " does not exist in table " +
                          table->name);
      }
      for (int existing : mapping) {
        if (existing == index) {
          return Status(StatusCode::kInvalidArgument,
                        "column " + name + " specified twice");
        }
      }
      mapping.push_back(index);
    }
  }

  int inserted = 0;
  for (const std::vector<Literal>& row : statement->insert.rows) {
    if (row.size() != mapping.size()) {
      return Status(StatusCode::kInvalidArgument,
                    "INSERT row has " + std::to_string(row.size()) +
                        " values but " + std::to_string(mapping.size()) +
                        " columns were specified");
    }
    std::vector<Value> values(schema.num_columns(), Value::Null());
    for (size_t i = 0; i < row.size(); ++i) {
      const Column& column = schema.GetColumn(static_cast<size_t>(mapping[i]));
      auto value = LiteralToValue(row[i], column);
      if (!value.ok()) {
        return Status(value.status().code(),
                      value.status().message() + " (column " + column.name + ")");
      }
      values[static_cast<size_t>(mapping[i])] = value.TakeValue();
    }
    auto tuple = Tuple::FromValues(schema, values);
    if (!tuple.ok()) return tuple.status();
    auto rid = table->heap->Insert(txn, tuple.value());
    if (!rid.ok()) return rid.status();
    ++inserted;
  }
  result->changes = inserted;
  result->message = "INSERT " + std::to_string(inserted) + " row(s) into " +
                    table->name;
  return Status::Ok();
}

Status Database::ExecuteDelete(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  auto table_result = catalog_->GetTable(statement->table_name);
  if (!table_result.ok()) return table_result.status();
  TableInfo* table = table_result.value();
  if (table->is_system) {
    return Status(StatusCode::kInvalidArgument,
                  "system table " + table->name + " is read-only");
  }
  const Schema& schema = table->schema;
  const DeleteSpec& spec = statement->delete_spec;

  int where_index = -1;
  Value where_value;
  if (spec.has_where) {
    where_index = schema.IndexOf(spec.where_column);
    if (where_index < 0) {
      return Status(StatusCode::kNotFound,
                    "column " + spec.where_column + " does not exist");
    }
    auto converted =
        LiteralToValue(spec.where_value, schema.GetColumn(where_index));
    if (!converted.ok()) return converted.status();
    where_value = converted.TakeValue();
    if (where_value.IsNull()) {
      return Status(StatusCode::kInvalidArgument,
                    "WHERE NULL is not supported");
    }
  }

  // Collect first: deleting a page's last row frees the page, so mutating the
  // heap while the scan holds a guard on that page is not safe.
  std::vector<RID> matches;
  auto iterator = table->heap->Scan(txn);
  RID rid;
  Tuple tuple;
  while (iterator.Next(&rid, &tuple)) {
    if (where_index >= 0) {
      auto decoded = Tuple::Decode(schema, tuple.data().data(), tuple.size());
      if (!decoded.ok()) return decoded.status();
      const std::vector<Value>& values = decoded.value();
      if (!Matches(values[static_cast<size_t>(where_index)], spec.where_op,
                   where_value)) {
        continue;
      }
    }
    matches.push_back(rid);
  }
  if (!iterator.status().ok()) return iterator.status();

  int deleted = 0;
  for (const RID& target : matches) {
    const Status status = table->heap->Delete(txn, target);
    if (!status.ok()) return status;
    ++deleted;
  }
  result->changes = deleted;
  result->message = "DELETE " + std::to_string(deleted) + " row(s) from " +
                    table->name;
  return Status::Ok();
}

Status Database::ExecuteUpdate(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  auto table_result = catalog_->GetTable(statement->table_name);
  if (!table_result.ok()) return table_result.status();
  TableInfo* table = table_result.value();
  if (table->is_system) {
    return Status(StatusCode::kInvalidArgument,
                  "system table " + table->name + " is read-only");
  }
  const Schema& schema = table->schema;
  const UpdateSpec& spec = statement->update;

  std::vector<std::pair<size_t, Value>> assignments;
  for (const Assignment& assignment : spec.assignments) {
    const int index = schema.IndexOf(assignment.column);
    if (index < 0) {
      return Status(StatusCode::kNotFound,
                    "column " + assignment.column + " does not exist in table " +
                        table->name);
    }
    for (const auto& existing : assignments) {
      if (existing.first == static_cast<size_t>(index)) {
        return Status(StatusCode::kInvalidArgument,
                      "column " + assignment.column + " assigned twice");
      }
    }
    const Column& column = schema.GetColumn(static_cast<size_t>(index));
    auto value = LiteralToValue(assignment.value, column);
    if (!value.ok()) {
      return Status(value.status().code(),
                    value.status().message() + " (column " + column.name + ")");
    }
    assignments.emplace_back(static_cast<size_t>(index), value.TakeValue());
  }

  int where_index = -1;
  Value where_value;
  if (spec.has_where) {
    where_index = schema.IndexOf(spec.where_column);
    if (where_index < 0) {
      return Status(StatusCode::kNotFound,
                    "column " + spec.where_column + " does not exist");
    }
    auto converted =
        LiteralToValue(spec.where_value, schema.GetColumn(where_index));
    if (!converted.ok()) return converted.status();
    where_value = converted.TakeValue();
    if (where_value.IsNull()) {
      return Status(StatusCode::kInvalidArgument,
                    "WHERE NULL is not supported");
    }
  }

  // Collect first, like DELETE: an update may move a row to a later page,
  // and the scan must not see it twice (or hold a guard we need to write).
  std::vector<std::pair<RID, Tuple>> matches;
  auto iterator = table->heap->Scan(txn);
  RID rid;
  Tuple tuple;
  while (iterator.Next(&rid, &tuple)) {
    if (where_index >= 0) {
      auto decoded = Tuple::Decode(schema, tuple.data().data(), tuple.size());
      if (!decoded.ok()) return decoded.status();
      if (!Matches(decoded.value()[static_cast<size_t>(where_index)],
                   spec.where_op, where_value)) {
        continue;
      }
    }
    matches.emplace_back(rid, std::move(tuple));
  }
  if (!iterator.status().ok()) return iterator.status();

  int updated = 0;
  for (const auto& [target, old_tuple] : matches) {
    auto decoded =
        Tuple::Decode(schema, old_tuple.data().data(), old_tuple.size());
    if (!decoded.ok()) return decoded.status();
    std::vector<Value> values = decoded.TakeValue();
    for (const auto& [index, value] : assignments) values[index] = value;
    auto new_tuple = Tuple::FromValues(schema, values);
    if (!new_tuple.ok()) return new_tuple.status();
    auto status = table->heap->Update(txn, target, new_tuple.value());
    if (!status.ok()) return status.status();
    ++updated;
  }
  result->changes = updated;
  result->message = "UPDATE " + std::to_string(updated) + " row(s) in " +
                    table->name;
  return Status::Ok();
}

Status Database::ExecuteSelect(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  auto table_result = catalog_->GetTable(statement->table_name);
  if (!table_result.ok()) return table_result.status();
  TableInfo* table = table_result.value();
  const Schema& schema = table->schema;
  const SelectSpec& spec = statement->select;

  // "rid" is a pseudo-column ("page:slot", like PostgreSQL's ctid) unless the
  // table has a real column with that name.
  const bool has_rid_column = schema.IndexOf(kRidColumn) >= 0;
  const auto is_rid = [&](const std::string& name) {
    return !has_rid_column && ToLower(name) == kRidColumn;
  };

  std::vector<size_t> projection;
  if (spec.star) {
    for (size_t i = 0; i < schema.num_columns(); ++i) projection.push_back(i);
  } else {
    for (const std::string& name : spec.columns) {
      if (name == "*") {
        for (size_t i = 0; i < schema.num_columns(); ++i) projection.push_back(i);
        continue;
      }
      if (is_rid(name)) {
        projection.push_back(kRidProjection);
        continue;
      }
      const int index = schema.IndexOf(name);
      if (index < 0) {
        return Status(StatusCode::kNotFound,
                      "column " + name + " does not exist in table " +
                          table->name);
      }
      projection.push_back(static_cast<size_t>(index));
    }
  }
  for (size_t index : projection) {
    if (index == kRidProjection) {
      result->columns.push_back({kRidColumn, TypeId::kVarchar});
    } else {
      result->columns.push_back({schema.GetColumn(index).name,
                                 schema.GetColumn(index).type});
    }
  }
  const auto emit = [&](RID rid, const std::vector<Value>& values) {
    std::vector<Value> row;
    row.reserve(projection.size());
    for (size_t index : projection) {
      row.push_back(index == kRidProjection ? Value::String(FormatRid(rid))
                                            : values[index]);
    }
    result->rows.push_back(std::move(row));
  };

  // WHERE rid = 'page:slot' is a direct lookup instead of a scan.
  if (spec.has_where && is_rid(spec.where_column)) {
    if (spec.where_op != "=" ||
        spec.where_value.kind != Literal::Kind::kString) {
      return Status(StatusCode::kInvalidArgument,
                    "only rid = 'page:slot' is supported");
    }
    auto rid = ParseRid(spec.where_value.text);
    if (!rid.ok()) return rid.status();
    auto tuple = table->heap->Get(txn, rid.value());
    if (!tuple.ok()) {
      if (tuple.status().code() == StatusCode::kNotFound) {
        return Status::Ok();  // no such row: empty result
      }
      return tuple.status();
    }
    if (spec.limit == 0) return Status::Ok();
    auto decoded = Tuple::Decode(schema, tuple.value().data().data(),
                                 tuple.value().size());
    if (!decoded.ok()) return decoded.status();
    emit(rid.value(), decoded.value());
    return Status::Ok();
  }

  int where_index = -1;
  Value where_value;
  if (spec.has_where) {
    where_index = schema.IndexOf(spec.where_column);
    if (where_index < 0) {
      return Status(StatusCode::kNotFound,
                    "column " + spec.where_column + " does not exist");
    }
    auto converted =
        LiteralToValue(spec.where_value, schema.GetColumn(where_index));
    if (!converted.ok()) return converted.status();
    where_value = converted.TakeValue();
    if (where_value.IsNull()) {
      return Status(StatusCode::kInvalidArgument,
                    "WHERE NULL is not supported");
    }
  }

  int64_t emitted = 0;
  auto iterator = table->heap->Scan(txn);
  RID rid;
  Tuple tuple;
  while (iterator.Next(&rid, &tuple)) {
    if (spec.limit >= 0 && emitted >= spec.limit) break;
    auto decoded =
        Tuple::Decode(schema, tuple.data().data(), tuple.size());
    if (!decoded.ok()) return decoded.status();
    const std::vector<Value>& values = decoded.value();
    if (where_index >= 0 &&
        !Matches(values[static_cast<size_t>(where_index)], spec.where_op,
                 where_value)) {
      continue;
    }
    emit(rid, values);
    ++emitted;
  }
  if (!iterator.status().ok()) return iterator.status();
  return Status::Ok();
}

Status Database::ExecuteCopy(Statement* statement, Transaction* txn,
                             QueryResult* result) {
  auto table_result = catalog_->GetTable(statement->table_name);
  if (!table_result.ok()) return table_result.status();
  TableInfo* table = table_result.value();
  if (table->is_system) {
    return Status(StatusCode::kInvalidArgument,
                  "system table " + table->name + " is read-only");
  }
  const Schema& schema = table->schema;
  const CopySpec& spec = statement->copy;
  std::ifstream input(spec.path, std::ios::binary);
  if (!input) {
    return Status(StatusCode::kIoError, "cannot read " + spec.path);
  }

  const BufferStats before = pool_->Stats();
  const uint32_t pages_before = table->segment->page_count();
  const auto start = std::chrono::steady_clock::now();
  std::string line;
  std::vector<std::string> fields;
  int64_t line_number = 0;
  int64_t loaded = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    fields.clear();
    size_t begin = 0;
    for (;;) {
      const size_t end = line.find(spec.delimiter, begin);
      if (end == std::string::npos) {
        fields.push_back(line.substr(begin));
        break;
      }
      fields.push_back(line.substr(begin, end - begin));
      begin = end + 1;
    }
    // dbgen ends every line with a delimiter (design doc 14.2).
    if (fields.size() == schema.num_columns() + 1 && fields.back().empty()) {
      fields.pop_back();
    }
    const auto fail = [&](const Status& status) {
      return Status(status.code(),
                    spec.path + ":" + std::to_string(line_number) + ": " +
                        status.message() + " (" + std::to_string(loaded) +
                        " row(s) loaded before the error are kept)");
    };
    if (fields.size() != schema.num_columns()) {
      return fail(Status(StatusCode::kInvalidArgument,
                         "expected " + std::to_string(schema.num_columns()) +
                             " fields, got " + std::to_string(fields.size())));
    }
    std::vector<Value> values(schema.num_columns());
    for (size_t i = 0; i < fields.size(); ++i) {
      const Column& column = schema.GetColumn(i);
      if (fields[i].empty() && column.nullable) {
        values[i] = Value::Null();
        continue;
      }
      Literal literal;
      literal.kind = Literal::Kind::kString;
      literal.text = std::move(fields[i]);
      auto value = LiteralToValue(literal, column);
      if (!value.ok()) {
        return fail(Status(value.status().code(), value.status().message() +
                                                      " (column " +
                                                      column.name + ")"));
      }
      values[i] = value.TakeValue();
    }
    auto tuple = Tuple::FromValues(schema, values);
    if (!tuple.ok()) return fail(tuple.status());
    auto rid = table->heap->Insert(txn, tuple.value());
    if (!rid.ok()) return fail(rid.status());
    ++loaded;
    if (loaded % 100000 == 0) {
      MINIDB_LOG_INFO("LOAD") << table->name << " rows=" << loaded
                              << " pages=" << table->segment->page_count()
                              << " hit_rate="
                              << std::round(pool_->Stats().HitRate() * 100) / 100
                              << "%";
    }
  }
  if (input.bad()) return Status(StatusCode::kIoError, "error reading " + spec.path);

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  const BufferStats& after = pool_->Stats();
  const uint64_t hit = after.hit - before.hit;
  const uint64_t miss = after.miss - before.miss;
  std::ostringstream message;
  message.setf(std::ios::fixed);
  message.precision(3);
  message << "COPY " << loaded << " row(s) into " << table->name << " in "
          << seconds << " s";
  if (seconds > 0) {
    message << " (" << static_cast<int64_t>(loaded / seconds) << " rows/s)";
  }
  message << "; pages " << pages_before << " -> "
          << table->segment->page_count() << ", file "
          << table->data_disk->header().file_extents << " extent(s)"
          << "; buffer hit=" << hit << " miss=" << miss
          << " evict=" << after.evict - before.evict
          << " flush=" << after.flush - before.flush
          << " new_page=" << after.new_page - before.new_page;
  if (hit + miss > 0) {
    message.precision(2);
    message << " hit_rate=" << 100.0 * hit / (hit + miss) << "%";
  }
  MINIDB_LOG_INFO("LOAD") << message.str();
  result->changes = static_cast<int>(loaded);
  result->message = message.str();
  return Status::Ok();
}

Status Database::ExecuteShowTables(QueryResult* result) {
  result->columns.push_back({"table_name", TypeId::kVarchar});
  for (TableInfo* table : catalog_->Tables()) {
    result->rows.push_back({Value::String(table->name)});
  }
  return Status::Ok();
}

Status Database::ExecuteDescribe(Statement* statement, QueryResult* result) {
  auto table_result = catalog_->GetTable(statement->table_name);
  if (!table_result.ok()) return table_result.status();
  const TableInfo* table = table_result.value();
  result->columns.push_back({"column_name", TypeId::kVarchar});
  result->columns.push_back({"data_type", TypeId::kVarchar});
  result->columns.push_back({"nullable", TypeId::kInt32});
  for (size_t i = 0; i < table->schema.num_columns(); ++i) {
    const Column& column = table->schema.GetColumn(i);
    result->rows.push_back({Value::String(column.name),
                            Value::String(table->schema.TypeName(i)),
                            Value::Int32(column.nullable ? 1 : 0)});
  }
  return Status::Ok();
}

}  // namespace minidb
