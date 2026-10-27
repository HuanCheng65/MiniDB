#include "record/value.h"

#include <cmath>

#include "common/util.h"

namespace minidb {
namespace {

long double AsNumber(const Value& value) {
  switch (value.kind) {
    case ValueKind::kInt32:
      return value.int32_value;
    case ValueKind::kInt64:
      return static_cast<long double>(value.int64_value);
    case ValueKind::kDouble:
      return value.double_value;
    case ValueKind::kDecimal:
      return static_cast<long double>(value.int64_value) /
             std::pow(10.0L, value.scale);
    case ValueKind::kDate:
      return value.int32_value;
    case ValueKind::kString:
    case ValueKind::kNull:
      return 0.0L;
  }
  return 0.0L;
}

bool IsNumeric(ValueKind kind) {
  return kind == ValueKind::kInt32 || kind == ValueKind::kInt64 ||
         kind == ValueKind::kDouble || kind == ValueKind::kDecimal ||
         kind == ValueKind::kDate;
}

}  // namespace

std::string ValueToString(const Value& value) {
  switch (value.kind) {
    case ValueKind::kNull:
      return "NULL";
    case ValueKind::kInt32:
      return std::to_string(value.int32_value);
    case ValueKind::kInt64:
      return std::to_string(value.int64_value);
    case ValueKind::kDouble:
      return FormatDouble(value.double_value);
    case ValueKind::kDecimal:
      return FormatDecimal(value.int64_value, value.scale);
    case ValueKind::kDate:
      return FormatDate(value.int32_value);
    case ValueKind::kString:
      // CHAR padding is already stripped when a tuple is decoded; anything
      // left, such as trailing spaces in a VARCHAR, is data.
      return value.string_value;
  }
  return "";
}

int CompareValues(const Value& left, const Value& right) {
  if (left.kind == ValueKind::kNull || right.kind == ValueKind::kNull) {
    if (left.kind == right.kind) return 0;
    return left.kind == ValueKind::kNull ? -1 : 1;
  }
  if (IsNumeric(left.kind) && IsNumeric(right.kind)) {
    const long double a = AsNumber(left);
    const long double b = AsNumber(right);
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
  // Strings compare with PAD SPACE semantics (trailing spaces are not
  // significant), so CHAR(n) columns match unpadded literals.
  const std::string a = TrimRight(ValueToString(left));
  const std::string b = TrimRight(ValueToString(right));
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

}  // namespace minidb
