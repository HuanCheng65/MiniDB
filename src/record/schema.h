// Type system and schema layout (design doc chapter 6).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace minidb {

// Persisted in sys_columns.type.
enum class TypeId : int32_t {
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kDate = 4,
  kChar = 5,
  kVarchar = 6,
  kDecimal = 7,
};

struct Column {
  std::string name;
  TypeId type = TypeId::kInt32;
  uint16_t length = 0;  // CHAR/VARCHAR length, DECIMAL precision
  uint8_t scale = 0;    // DECIMAL scale
  bool nullable = true;
};

class Schema {
 public:
  static constexpr uint16_t kInvalidIndex = 0xFFFF;
  static constexpr uint32_t kMaxRecordSize = 8100;

  Schema() = default;

  static Result<Schema> Make(std::vector<Column> columns);

  size_t num_columns() const { return columns_.size(); }
  const Column& GetColumn(size_t index) const { return columns_[index]; }
  const std::vector<Column>& columns() const { return columns_; }
  int IndexOf(std::string_view name) const;
  std::string TypeName(size_t index) const;

  static bool IsFixedType(TypeId type);
  static uint16_t FixedTypeSize(TypeId type, uint16_t length);
  uint16_t fixed_size() const { return fixed_size_; }
  uint16_t null_bitmap_size() const { return null_bitmap_size_; }
  uint16_t fixed_offset(size_t column) const { return fixed_offsets_[column]; }
  uint16_t var_index(size_t column) const { return var_indices_[column]; }
  uint16_t var_entry_offset(size_t column) const { return var_offsets_[column]; }
  size_t var_count() const { return var_count_; }
  uint32_t MaxRecordSize() const;

 private:
  Status BuildLayout();

  std::vector<Column> columns_;
  std::vector<uint16_t> fixed_offsets_;
  std::vector<uint16_t> var_indices_;
  std::vector<uint16_t> var_offsets_;
  uint16_t fixed_size_ = 0;
  uint16_t null_bitmap_size_ = 0;
  size_t var_count_ = 0;
};

}  // namespace minidb
