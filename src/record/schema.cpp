#include "record/schema.h"

#include "common/util.h"

namespace minidb {
namespace {

constexpr uint16_t kRecordHeaderSize = 18;

}  // namespace

bool Schema::IsFixedType(TypeId type) {
  return type != TypeId::kVarchar;
}

uint16_t Schema::FixedTypeSize(TypeId type, uint16_t length) {
  switch (type) {
    case TypeId::kInt32:
    case TypeId::kDate:
      return 4;
    case TypeId::kInt64:
    case TypeId::kDouble:
    case TypeId::kDecimal:
      return 8;
    case TypeId::kChar:
      return length;
    case TypeId::kVarchar:
      return 0;
  }
  return 0;
}

Result<Schema> Schema::Make(std::vector<Column> columns) {
  Schema schema;
  schema.columns_ = std::move(columns);
  const Status status = schema.BuildLayout();
  if (!status.ok()) return status;
  return schema;
}

Status Schema::BuildLayout() {
  if (columns_.empty()) {
    return Status(StatusCode::kInvalidArgument, "table must have columns");
  }
  const size_t n = columns_.size();
  fixed_offsets_.assign(n, 0);
  var_indices_.assign(n, kInvalidIndex);
  var_offsets_.assign(n, 0);
  fixed_size_ = 0;
  var_count_ = 0;
  // Accumulate in 32 bits: many wide CHAR columns would wrap a uint16_t and
  // slip past the record-size check below.
  uint32_t fixed_total = 0;
  uint32_t max_total = kRecordHeaderSize + static_cast<uint32_t>((n + 7) / 8);

  for (size_t i = 0; i < n; ++i) {
    const Column& column = columns_[i];
    if (column.name.empty()) {
      return Status(StatusCode::kInvalidArgument, "column name is empty");
    }
    switch (column.type) {
      case TypeId::kInt32:
      case TypeId::kInt64:
      case TypeId::kDouble:
      case TypeId::kDate:
        break;
      case TypeId::kDecimal:
        if (column.length < 1 || column.length > 18) {
          return Status(StatusCode::kInvalidArgument,
                        "DECIMAL precision must be in 1..18 for column " +
                            column.name);
        }
        if (column.scale > column.length) {
          return Status(StatusCode::kInvalidArgument,
                        "DECIMAL scale exceeds precision for column " +
                            column.name);
        }
        break;
      case TypeId::kChar:
      case TypeId::kVarchar:
        if (column.length < 1 || column.length > 8000) {
          return Status(StatusCode::kInvalidArgument,
                        "CHAR/VARCHAR length must be in 1..8000 for column " +
                            column.name);
        }
        break;
      default:
        return Status(StatusCode::kInvalidArgument,
                      "unsupported column type for " + column.name);
    }

    if (IsFixedType(column.type)) {
      const uint32_t size = FixedTypeSize(column.type, column.length);
      fixed_offsets_[i] = static_cast<uint16_t>(fixed_total);
      fixed_total += size;
      max_total += size;
    } else {
      var_indices_[i] = static_cast<uint16_t>(var_count_);
      var_offsets_[i] = static_cast<uint16_t>(2 * var_count_);
      ++var_count_;
      max_total += 2u + column.length;
    }
    if (max_total > kMaxRecordSize) {
      return Status(StatusCode::kInvalidArgument,
                    "record can exceed the 8100-byte page limit");
    }
  }
  fixed_size_ = static_cast<uint16_t>(fixed_total);

  null_bitmap_size_ = static_cast<uint16_t>((n + 7) / 8);
  if (MaxRecordSize() > kMaxRecordSize) {
    return Status(StatusCode::kInvalidArgument,
                  "record can exceed the 8100-byte page limit");
  }
  return Status::Ok();
}

int Schema::IndexOf(std::string_view name) const {
  const std::string needle = ToLower(std::string(name));
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (ToLower(columns_[i].name) == needle) return static_cast<int>(i);
  }
  return -1;
}

std::string Schema::TypeName(size_t index) const {
  const Column& column = columns_[index];
  switch (column.type) {
    case TypeId::kInt32:
      return "INTEGER";
    case TypeId::kInt64:
      return "BIGINT";
    case TypeId::kDouble:
      return "DOUBLE";
    case TypeId::kDate:
      return "DATE";
    case TypeId::kChar:
      return "CHAR(" + std::to_string(column.length) + ")";
    case TypeId::kVarchar:
      return "VARCHAR(" + std::to_string(column.length) + ")";
    case TypeId::kDecimal:
      return "DECIMAL(" + std::to_string(column.length) + "," +
             std::to_string(column.scale) + ")";
  }
  return "UNKNOWN";
}

uint32_t Schema::MaxRecordSize() const {
  uint32_t size = kRecordHeaderSize + null_bitmap_size_ + fixed_size_ +
                  static_cast<uint32_t>(var_count_) * 2;
  for (const Column& column : columns_) {
    if (column.type == TypeId::kVarchar) size += column.length;
  }
  return size;
}

}  // namespace minidb
