// Runtime value representation used by tuples and the SQL front end.
#pragma once

#include <cstdint>
#include <string>

namespace minidb {

enum class ValueKind {
  kNull = 0,
  kInt32,
  kInt64,
  kDouble,
  kDecimal,
  kDate,
  kString,
};

struct Value {
  ValueKind kind = ValueKind::kNull;
  int32_t int32_value = 0;
  int64_t int64_value = 0;
  double double_value = 0.0;
  uint8_t scale = 0;
  std::string string_value;

  static Value Null() { return Value(); }
  static Value Int32(int32_t value) {
    Value v;
    v.kind = ValueKind::kInt32;
    v.int32_value = value;
    v.int64_value = value;
    return v;
  }
  static Value Int64(int64_t value) {
    Value v;
    v.kind = ValueKind::kInt64;
    v.int64_value = value;
    return v;
  }
  static Value Double(double value) {
    Value v;
    v.kind = ValueKind::kDouble;
    v.double_value = value;
    return v;
  }
  static Value Decimal(int64_t value, uint8_t scale) {
    Value v;
    v.kind = ValueKind::kDecimal;
    v.int64_value = value;
    v.scale = scale;
    return v;
  }
  static Value Date(int32_t days) {
    Value v;
    v.kind = ValueKind::kDate;
    v.int32_value = days;
    v.int64_value = days;
    return v;
  }
  static Value String(std::string value) {
    Value v;
    v.kind = ValueKind::kString;
    v.string_value = std::move(value);
    return v;
  }

  bool IsNull() const { return kind == ValueKind::kNull; }
};

std::string ValueToString(const Value& value);

// Returns <0, 0, >0. Numeric kinds compare as long double, strings
// lexicographically, dates by day count.
int CompareValues(const Value& left, const Value& right);

}  // namespace minidb
