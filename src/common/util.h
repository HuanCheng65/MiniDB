// Small self-contained helpers (no std::format dependency, design doc 2.1).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"

namespace minidb {

std::string ToLower(std::string text);
std::string TrimRight(std::string text);
std::string Trim(std::string text);

// Decimal values are stored as int64 scaled by 10^scale.
Result<int64_t> ParseDecimal(std::string_view text, uint8_t scale);
std::string FormatDecimal(int64_t value, uint8_t scale);

// DATE is stored as days since 1970-01-01 (design doc 6.1).
Result<int32_t> ParseDate(std::string_view text);
std::string FormatDate(int32_t days);

std::string FormatDouble(double value);

}  // namespace minidb
