#include "record/tuple.h"

#include <cstring>
#include <limits>

namespace minidb {
namespace {

constexpr size_t kTxnIdOffset = 0;
constexpr size_t kUndoPtrOffset = 8;
constexpr size_t kInfoOffset = 16;
constexpr uint16_t kInfoDeleted = 0x0001;

void WriteFixed(std::byte* data, const Column& column, const Value& value) {
  switch (column.type) {
    case TypeId::kInt32: {
      int32_t v = value.kind == ValueKind::kInt32 ? value.int32_value
                                                  : static_cast<int32_t>(value.int64_value);
      std::memcpy(data, &v, sizeof(v));
      break;
    }
    case TypeId::kInt64: {
      int64_t v = value.kind == ValueKind::kInt64 ? value.int64_value
                                                  : value.int32_value;
      std::memcpy(data, &v, sizeof(v));
      break;
    }
    case TypeId::kDouble: {
      double v = value.kind == ValueKind::kDouble
                     ? value.double_value
                     : static_cast<double>(value.kind == ValueKind::kInt32
                                               ? value.int32_value
                                               : value.int64_value);
      std::memcpy(data, &v, sizeof(v));
      break;
    }
    case TypeId::kDecimal: {
      int64_t v = value.int64_value;
      std::memcpy(data, &v, sizeof(v));
      break;
    }
    case TypeId::kDate: {
      int32_t v = value.int32_value;
      std::memcpy(data, &v, sizeof(v));
      break;
    }
    case TypeId::kChar: {
      std::memset(data, ' ', column.length);
      const std::string& s = value.string_value;
      std::memcpy(data, s.data(), s.size());
      break;
    }
    case TypeId::kVarchar:
      break;
  }
}

Status ValidateValue(const Column& column, const Value& value) {
  if (value.kind == ValueKind::kNull) {
    if (!column.nullable) {
      return Status(StatusCode::kInvalidArgument,
                    "NULL value for NOT NULL column " + column.name);
    }
    return Status::Ok();
  }
  switch (column.type) {
    case TypeId::kInt32:
      if (value.kind == ValueKind::kInt32) return Status::Ok();
      if (value.kind == ValueKind::kInt64 &&
          value.int64_value >= std::numeric_limits<int32_t>::min() &&
          value.int64_value <= std::numeric_limits<int32_t>::max()) {
        return Status::Ok();
      }
      return Status(StatusCode::kTypeMismatch,
                    "type mismatch for INTEGER column " + column.name);
    case TypeId::kInt64:
      if (value.kind == ValueKind::kInt64 || value.kind == ValueKind::kInt32) {
        return Status::Ok();
      }
      return Status(StatusCode::kTypeMismatch,
                    "type mismatch for BIGINT column " + column.name);
    case TypeId::kDouble:
      if (value.kind == ValueKind::kDouble || value.kind == ValueKind::kInt32 ||
          value.kind == ValueKind::kInt64) {
        return Status::Ok();
      }
      return Status(StatusCode::kTypeMismatch,
                    "type mismatch for DOUBLE column " + column.name);
    case TypeId::kDecimal:
      if (value.kind != ValueKind::kDecimal) {
        return Status(StatusCode::kTypeMismatch,
                      "type mismatch for DECIMAL column " + column.name);
      }
      if (value.scale != column.scale) {
        return Status(StatusCode::kTypeMismatch,
                      "decimal scale mismatch for column " + column.name);
      }
      return Status::Ok();
    case TypeId::kDate:
      if (value.kind != ValueKind::kDate) {
        return Status(StatusCode::kTypeMismatch,
                      "type mismatch for DATE column " + column.name);
      }
      return Status::Ok();
    case TypeId::kChar: {
      if (value.kind != ValueKind::kString) {
        return Status(StatusCode::kTypeMismatch,
                      "type mismatch for CHAR column " + column.name);
      }
      if (value.string_value.size() > column.length) {
        return Status(StatusCode::kInvalidArgument,
                      "value too long for CHAR(" +
                          std::to_string(column.length) + ") column " +
                          column.name);
      }
      return Status::Ok();
    }
    case TypeId::kVarchar: {
      if (value.kind != ValueKind::kString) {
        return Status(StatusCode::kTypeMismatch,
                      "type mismatch for VARCHAR column " + column.name);
      }
      if (value.string_value.size() > column.length) {
        return Status(StatusCode::kInvalidArgument,
                      "value too long for VARCHAR(" +
                          std::to_string(column.length) + ") column " +
                          column.name);
      }
      return Status::Ok();
    }
  }
  return Status::Ok();
}

}  // namespace

Result<Tuple> Tuple::FromValues(const Schema& schema,
                                const std::vector<Value>& values) {
  if (values.size() != schema.num_columns()) {
    return Status(StatusCode::kInvalidArgument,
                  "expected " + std::to_string(schema.num_columns()) +
                      " values, got " + std::to_string(values.size()));
  }
  for (size_t i = 0; i < values.size(); ++i) {
    const Status status = ValidateValue(schema.GetColumn(i), values[i]);
    if (!status.ok()) return status;
  }

  const size_t null_bytes = schema.null_bitmap_size();
  const size_t fixed_size = schema.fixed_size();
  const size_t var_count = schema.var_count();
  const size_t fixed_start = kHeaderSize + null_bytes;
  const size_t var_offset_start = fixed_start + fixed_size;
  const size_t var_data_start = var_offset_start + var_count * 2;

  size_t var_bytes = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    if (schema.GetColumn(i).type == TypeId::kVarchar &&
        values[i].kind != ValueKind::kNull) {
      var_bytes += values[i].string_value.size();
    }
  }
  const size_t total = var_data_start + var_bytes;
  if (total > Schema::kMaxRecordSize) {
    return Status(StatusCode::kInvalidArgument,
                  "tuple exceeds maximum record size");
  }

  Tuple tuple;
  tuple.data_.assign(total, std::byte{0});

  int64_t zero = 0;
  std::memcpy(tuple.data_.data() + kTxnIdOffset, &zero, sizeof(zero));
  std::memcpy(tuple.data_.data() + kUndoPtrOffset, &zero, sizeof(zero));
  uint16_t info = 0;
  std::memcpy(tuple.data_.data() + kInfoOffset, &info, sizeof(info));
  (void)kInfoDeleted;

  std::byte* base = tuple.data_.data();

  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i].kind == ValueKind::kNull) {
      base[kHeaderSize + i / 8] |= static_cast<std::byte>(1u << (i % 8));
    }
  }

  size_t var_cursor = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    const Column& column = schema.GetColumn(i);
    const Value& value = values[i];
    if (value.kind == ValueKind::kNull) continue;

    if (Schema::IsFixedType(column.type)) {
      WriteFixed(base + fixed_start + schema.fixed_offset(i), column, value);
    } else {
      const uint16_t entry = schema.var_entry_offset(i);
      var_cursor += value.string_value.size();
      const uint16_t end = static_cast<uint16_t>(var_cursor);
      std::memcpy(base + var_offset_start + entry, &end, sizeof(end));
      std::memcpy(base + var_data_start + var_cursor - value.string_value.size(),
                  value.string_value.data(), value.string_value.size());
    }
  }

  return tuple;
}

Result<std::vector<Value>> Tuple::Decode(const Schema& schema,
                                         const std::byte* data,
                                         size_t length) {
  if (length < kHeaderSize) {
    return Status(StatusCode::kCorruption, "tuple shorter than record header");
  }
  const size_t null_bytes = schema.null_bitmap_size();
  const size_t fixed_start = kHeaderSize + null_bytes;
  const size_t var_offset_start = fixed_start + schema.fixed_size();
  const size_t var_data_start = var_offset_start + schema.var_count() * 2;
  if (length < var_data_start) {
    return Status(StatusCode::kCorruption, "tuple shorter than schema layout");
  }

  std::vector<Value> values;
  values.reserve(schema.num_columns());
  uint16_t previous_var_end = 0;
  size_t var_index_seen = 0;

  for (size_t i = 0; i < schema.num_columns(); ++i) {
    const Column& column = schema.GetColumn(i);
    const bool is_null = (data[kHeaderSize + i / 8] &
                          static_cast<std::byte>(1u << (i % 8))) != std::byte{0};
    if (is_null) {
      if (!column.nullable) {
        return Status(StatusCode::kCorruption,
                      "NULL bit set on NOT NULL column " + column.name);
      }
      values.push_back(Value::Null());
      if (!Schema::IsFixedType(column.type)) ++var_index_seen;
      continue;
    }

    if (Schema::IsFixedType(column.type)) {
      const std::byte* cell = data + fixed_start + schema.fixed_offset(i);
      switch (column.type) {
        case TypeId::kInt32: {
          int32_t v;
          std::memcpy(&v, cell, sizeof(v));
          values.push_back(Value::Int32(v));
          break;
        }
        case TypeId::kInt64: {
          int64_t v;
          std::memcpy(&v, cell, sizeof(v));
          values.push_back(Value::Int64(v));
          break;
        }
        case TypeId::kDouble: {
          double v;
          std::memcpy(&v, cell, sizeof(v));
          values.push_back(Value::Double(v));
          break;
        }
        case TypeId::kDecimal: {
          int64_t v;
          std::memcpy(&v, cell, sizeof(v));
          values.push_back(Value::Decimal(v, column.scale));
          break;
        }
        case TypeId::kDate: {
          int32_t v;
          std::memcpy(&v, cell, sizeof(v));
          values.push_back(Value::Date(v));
          break;
        }
        case TypeId::kChar: {
          std::string text(reinterpret_cast<const char*>(cell), column.length);
          while (!text.empty() && text.back() == ' ') text.pop_back();
          values.push_back(Value::String(std::move(text)));
          break;
        }
        case TypeId::kVarchar:
          break;
      }
    } else {
      const uint16_t entry = schema.var_entry_offset(i);
      uint16_t end = 0;
      std::memcpy(&end, data + var_offset_start + entry, sizeof(end));
      if (end < previous_var_end || var_data_start + end > length) {
        return Status(StatusCode::kCorruption,
                      "invalid VARCHAR offset in tuple");
      }
      const size_t start = var_data_start + previous_var_end;
      const size_t var_length = end - previous_var_end;
      values.push_back(Value::String(std::string(
          reinterpret_cast<const char*>(data + start), var_length)));
      previous_var_end = end;
    }
    (void)var_index_seen;
  }

  return values;
}

}  // namespace minidb
