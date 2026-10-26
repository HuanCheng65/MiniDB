#include "minidb/minidb.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "db/database.h"

using minidb::Config;
using minidb::Database;
using minidb::QueryResult;
using minidb::TypeId;
using minidb::Value;
using minidb::ValueKind;
using minidb::ValueToString;

struct mydb_db {
  std::unique_ptr<Database> database;
  std::string error;
  std::string message;
  int changes = 0;
};

struct mydb_stmt {
  std::unique_ptr<QueryResult> result;
  size_t next_row = 0;
  size_t current_row = std::numeric_limits<size_t>::max();
  std::vector<std::string> text;
  int changes = 0;
};

namespace {

thread_local std::string g_last_error;

void SetError(mydb_db* db, const std::string& message) {
  if (db != nullptr) db->error = message;
  g_last_error = message;
}

void StoreErrorMessage(char** errmsg, const std::string& message) {
  if (errmsg == nullptr) return;
  char* copy = static_cast<char*>(std::malloc(message.size() + 1));
  if (copy == nullptr) {
    *errmsg = nullptr;
    return;
  }
  std::memcpy(copy, message.c_str(), message.size() + 1);
  *errmsg = copy;
}

int TypeForColumn(TypeId type) {
  switch (type) {
    case TypeId::kInt32:
    case TypeId::kInt64:
      return MYDB_TYPE_INT;
    case TypeId::kDouble:
      return MYDB_TYPE_DOUBLE;
    case TypeId::kDate:
      return MYDB_TYPE_DATE;
    case TypeId::kDecimal:
      return MYDB_TYPE_DECIMAL;
    case TypeId::kChar:
    case TypeId::kVarchar:
      return MYDB_TYPE_TEXT;
  }
  return MYDB_TYPE_NULL;
}

const Value* CurrentValue(const mydb_stmt* stmt, int column) {
  if (stmt == nullptr || stmt->result == nullptr) return nullptr;
  if (stmt->current_row >= stmt->result->rows.size()) return nullptr;
  if (column < 0 ||
      static_cast<size_t>(column) >= stmt->result->columns.size()) {
    return nullptr;
  }
  return &stmt->result->rows[stmt->current_row]
              [static_cast<size_t>(column)];
}

void FormatCurrentRow(mydb_stmt* stmt) {
  const size_t count = stmt->result->columns.size();
  stmt->text.assign(count, std::string());
  if (stmt->current_row >= stmt->result->rows.size()) return;
  const std::vector<Value>& row = stmt->result->rows[stmt->current_row];
  for (size_t i = 0; i < count && i < row.size(); ++i) {
    stmt->text[i] = ValueToString(row[i]);
  }
}

}  // namespace

extern "C" {

int mydb_open(const char* path, const mydb_config* config, mydb_db** out_db) {
  if (out_db == nullptr) return MYDB_ERROR;
  *out_db = nullptr;
  if (path == nullptr) {
    SetError(nullptr, "database path is NULL");
    return MYDB_ERROR;
  }
  try {
    Config db_config;
    db_config.path = path;
    if (config != nullptr) {
      if (config->buffer_pool_frames > 0) {
        db_config.buffer_pool_frames =
            static_cast<uint32_t>(config->buffer_pool_frames);
      }
      if (config->replacer != nullptr) db_config.replacer = config->replacer;
      if (config->log_level != nullptr) db_config.log_level = config->log_level;
      if (config->log_file != nullptr) db_config.log_file = config->log_file;
    }
    auto database = Database::Open(db_config);
    if (!database.ok()) {
      SetError(nullptr, database.status().message());
      return MYDB_ERROR;
    }
    auto handle = std::make_unique<mydb_db>();
    handle->database = database.TakeValue();
    g_last_error.clear();
    *out_db = handle.release();
    return MYDB_OK;
  } catch (const std::exception& error) {
    SetError(nullptr, error.what());
    return MYDB_ERROR;
  } catch (...) {
    SetError(nullptr, "unknown error while opening database");
    return MYDB_ERROR;
  }
}

int mydb_close(mydb_db* db) {
  if (db == nullptr) return MYDB_ERROR;
  try {
    const minidb::Status status = db->database->Close();
    if (!status.ok()) {
      SetError(db, status.message());
      return MYDB_ERROR;
    }
    delete db;
    return MYDB_OK;
  } catch (const std::exception& error) {
    SetError(db, error.what());
    return MYDB_ERROR;
  } catch (...) {
    SetError(db, "unknown error while closing database");
    return MYDB_ERROR;
  }
}

int mydb_exec(mydb_db* db, const char* sql, char** errmsg) {
  if (errmsg != nullptr) *errmsg = nullptr;
  if (db == nullptr || sql == nullptr) {
    SetError(db, "invalid argument (NULL database or SQL)");
    StoreErrorMessage(errmsg, "invalid argument (NULL database or SQL)");
    return MYDB_ERROR;
  }
  try {
    QueryResult result;
    const minidb::Status status = db->database->Execute(sql, &result);
    if (!status.ok()) {
      SetError(db, status.message());
      StoreErrorMessage(errmsg, status.message());
      return MYDB_ERROR;
    }
    db->error.clear();
    db->changes = result.changes;
    db->message = result.message;
    return MYDB_OK;
  } catch (const std::exception& error) {
    SetError(db, error.what());
    StoreErrorMessage(errmsg, error.what());
    return MYDB_ERROR;
  } catch (...) {
    SetError(db, "unknown error while executing SQL");
    StoreErrorMessage(errmsg, "unknown error while executing SQL");
    return MYDB_ERROR;
  }
}

int mydb_prepare(mydb_db* db, const char* sql, mydb_stmt** out_stmt) {
  if (out_stmt == nullptr) return MYDB_ERROR;
  *out_stmt = nullptr;
  if (db == nullptr || sql == nullptr) {
    SetError(db, "invalid argument (NULL database or SQL)");
    return MYDB_ERROR;
  }
  try {
    auto result = std::make_unique<QueryResult>();
    const minidb::Status status = db->database->Execute(sql, result.get());
    if (!status.ok()) {
      SetError(db, status.message());
      return MYDB_ERROR;
    }
    auto stmt = std::make_unique<mydb_stmt>();
    stmt->changes = result->changes;
    stmt->result = std::move(result);
    stmt->text.assign(stmt->result->columns.size(), std::string());
    db->error.clear();
    db->changes = stmt->changes;
    db->message = stmt->result->message;
    *out_stmt = stmt.release();
    return MYDB_OK;
  } catch (const std::exception& error) {
    SetError(db, error.what());
    return MYDB_ERROR;
  } catch (...) {
    SetError(db, "unknown error while preparing SQL");
    return MYDB_ERROR;
  }
}

int mydb_step(mydb_stmt* stmt) {
  if (stmt == nullptr || stmt->result == nullptr) return MYDB_ERROR;
  if (stmt->next_row >= stmt->result->rows.size()) {
    stmt->current_row = std::numeric_limits<size_t>::max();
    FormatCurrentRow(stmt);
    return MYDB_DONE;
  }
  stmt->current_row = stmt->next_row++;
  FormatCurrentRow(stmt);
  return MYDB_ROW;
}

int mydb_column_count(mydb_stmt* stmt) {
  if (stmt == nullptr || stmt->result == nullptr) return 0;
  return static_cast<int>(stmt->result->columns.size());
}

const char* mydb_column_name(mydb_stmt* stmt, int column) {
  if (stmt == nullptr || stmt->result == nullptr || column < 0 ||
      static_cast<size_t>(column) >= stmt->result->columns.size()) {
    return nullptr;
  }
  return stmt->result->columns[static_cast<size_t>(column)].name.c_str();
}

int mydb_column_type(mydb_stmt* stmt, int column) {
  if (stmt == nullptr || stmt->result == nullptr || column < 0 ||
      static_cast<size_t>(column) >= stmt->result->columns.size()) {
    return MYDB_TYPE_NULL;
  }
  return TypeForColumn(
      stmt->result->columns[static_cast<size_t>(column)].type);
}

long long mydb_column_int64(mydb_stmt* stmt, int column) {
  const Value* value = CurrentValue(stmt, column);
  if (value == nullptr) return 0;
  switch (value->kind) {
    case ValueKind::kInt32:
      return value->int32_value;
    case ValueKind::kInt64:
    case ValueKind::kDecimal:
      return value->int64_value;
    case ValueKind::kDouble:
      return static_cast<long long>(value->double_value);
    case ValueKind::kDate:
      return value->int32_value;
    case ValueKind::kString:
      return std::strtoll(value->string_value.c_str(), nullptr, 10);
    case ValueKind::kNull:
      return 0;
  }
  return 0;
}

double mydb_column_double(mydb_stmt* stmt, int column) {
  const Value* value = CurrentValue(stmt, column);
  if (value == nullptr) return 0.0;
  switch (value->kind) {
    case ValueKind::kInt32:
      return value->int32_value;
    case ValueKind::kInt64:
      return static_cast<double>(value->int64_value);
    case ValueKind::kDouble:
      return value->double_value;
    case ValueKind::kDecimal:
      return static_cast<double>(value->int64_value) /
             static_cast<double>(std::pow(10.0, value->scale));
    case ValueKind::kDate:
      return value->int32_value;
    case ValueKind::kString:
      return std::strtod(value->string_value.c_str(), nullptr);
    case ValueKind::kNull:
      return 0.0;
  }
  return 0.0;
}

const char* mydb_column_text(mydb_stmt* stmt, int column) {
  if (stmt == nullptr || column < 0 ||
      static_cast<size_t>(column) >= stmt->text.size()) {
    return nullptr;
  }
  return stmt->text[static_cast<size_t>(column)].c_str();
}

void mydb_finalize(mydb_stmt* stmt) { delete stmt; }

int mydb_changes(mydb_db* db) { return db == nullptr ? 0 : db->changes; }

const char* mydb_message(mydb_db* db) {
  if (db == nullptr || db->message.empty()) return nullptr;
  return db->message.c_str();
}

const char* mydb_errmsg(mydb_db* db) {
  if (db == nullptr || db->error.empty()) return nullptr;
  return db->error.c_str();
}

const char* mydb_last_error(void) {
  if (g_last_error.empty()) return nullptr;
  return g_last_error.c_str();
}

void mydb_free(void* pointer) { std::free(pointer); }

const char* mydb_version(void) { return "MiniDB 0.1.0"; }

}  // extern "C"
