// Tuple encoding/decoding (design doc 6.3).
#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

#include "common/status.h"
#include "common/types.h"
#include "record/schema.h"
#include "record/value.h"

namespace minidb {

class Tuple {
 public:
  static constexpr size_t kHeaderSize = 18;  // txn_id(8) + undo_ptr(8) + info(2)

  Tuple() = default;

  static Result<Tuple> FromValues(const Schema& schema,
                                  const std::vector<Value>& values);
  static Result<std::vector<Value>> Decode(const Schema& schema,
                                           const std::byte* data,
                                           size_t length);
  static Tuple FromRaw(std::vector<std::byte> data) {
    Tuple tuple;
    tuple.data_ = std::move(data);
    return tuple;
  }

  const std::vector<std::byte>& data() const { return data_; }
  std::vector<std::byte>& mutable_data() { return data_; }
  size_t size() const { return data_.size(); }
  bool empty() const { return data_.empty(); }

  void Stamp(txn_id_t txn_id) {
    if (data_.size() < kHeaderSize) return;
    std::memcpy(data_.data(), &txn_id, sizeof(txn_id));
  }

 private:
  std::vector<std::byte> data_;
};

}  // namespace minidb
