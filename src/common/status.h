// Status / Result used for all recoverable failures (design doc 4.2).
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace minidb {

enum class StatusCode {
  kOk = 0,
  kNotFound,
  kAlreadyExists,
  kInvalidArgument,
  kTypeMismatch,
  kNoSpace,
  kIoError,
  kCorruption,
  kNotSupported,
  kInternal,
  kAborted,
};

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const {
    if (ok()) return "OK";
    return message_.empty() ? "error" : message_;
  }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Status status) : status_(std::move(status)) {}

  bool ok() const { return value_.has_value(); }
  explicit operator bool() const { return ok(); }

  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T TakeValue() { return std::move(*value_); }

  const Status& status() const { return status_; }

  T ValueOr(T fallback) const { return value_.value_or(std::move(fallback)); }

 private:
  std::optional<T> value_;
  Status status_;
};

}  // namespace minidb
