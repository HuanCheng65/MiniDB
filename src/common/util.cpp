#include "common/util.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace minidb {
namespace {

// Howard Hinnant's civil calendar algorithms.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t* year, unsigned* month, unsigned* day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  *year = y + (m <= 2);
  *month = m;
  *day = d;
}

bool ParseUnsigned(std::string_view text, size_t* pos, int* out) {
  int value = 0;
  bool any = false;
  while (*pos < text.size() && std::isdigit(static_cast<unsigned char>(text[*pos]))) {
    any = true;
    value = value * 10 + (text[*pos] - '0');
    ++*pos;
  }
  if (!any) return false;
  *out = value;
  return true;
}

}  // namespace

std::string ToLower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

std::string TrimRight(std::string text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  return text;
}

std::string Trim(std::string text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

Result<int64_t> ParseDecimal(std::string_view text, uint8_t scale) {
  std::string s = Trim(std::string(text));
  if (s.empty()) return Status(StatusCode::kInvalidArgument, "empty decimal");

  bool negative = false;
  size_t pos = 0;
  if (s[pos] == '+' || s[pos] == '-') {
    negative = s[pos] == '-';
    ++pos;
  }

  std::string digits;
  std::string fraction;
  bool saw_dot = false;
  for (; pos < s.size(); ++pos) {
    const char c = s[pos];
    if (c == '.') {
      if (saw_dot) {
        return Status(StatusCode::kInvalidArgument, "invalid decimal: " + s);
      }
      saw_dot = true;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      (saw_dot ? fraction : digits).push_back(c);
    } else if (c == 'e' || c == 'E') {
      return Status(StatusCode::kInvalidArgument,
                    "scientific notation is not supported by DECIMAL: " + s);
    } else {
      return Status(StatusCode::kInvalidArgument, "invalid decimal: " + s);
    }
  }
  if (digits.empty() && fraction.empty()) {
    return Status(StatusCode::kInvalidArgument, "invalid decimal: " + s);
  }
  if (fraction.size() > scale) {
    // Accept extra digits only when they are exact trailing zeroes.
    for (size_t i = scale; i < fraction.size(); ++i) {
      if (fraction[i] != '0') {
        return Status(StatusCode::kInvalidArgument,
                      "decimal has more fractional digits than scale " +
                          std::to_string(scale) + ": " + s);
      }
    }
    fraction.resize(scale);
  }
  fraction.resize(scale, '0');

  __int128 value = 0;
  for (char c : digits) {
    value = value * 10 + (c - '0');
    if (value > (static_cast<__int128>(1) << 63)) {
      return Status(StatusCode::kInvalidArgument, "decimal out of range: " + s);
    }
  }
  for (char c : fraction) {
    value = value * 10 + (c - '0');
    if (value > (static_cast<__int128>(1) << 63)) {
      return Status(StatusCode::kInvalidArgument, "decimal out of range: " + s);
    }
  }

  if (negative) value = -value;
  if (value < std::numeric_limits<int64_t>::min() ||
      value > std::numeric_limits<int64_t>::max()) {
    return Status(StatusCode::kInvalidArgument, "decimal out of range: " + s);
  }
  return static_cast<int64_t>(value);
}

std::string FormatDecimal(int64_t value, uint8_t scale) {
  uint64_t magnitude = value < 0
                           ? static_cast<uint64_t>(-(value + 1)) + 1
                           : static_cast<uint64_t>(value);
  std::string digits = std::to_string(magnitude);
  std::string result;
  if (value < 0) result.push_back('-');
  if (scale == 0) return result + digits;

  if (digits.size() <= scale) {
    digits.insert(digits.begin(), scale - digits.size() + 1, '0');
  }
  const size_t point = digits.size() - scale;
  result.append(digits.substr(0, point));
  result.push_back('.');
  result.append(digits.substr(point));
  return result;
}

Result<int32_t> ParseDate(std::string_view text) {
  std::string s = Trim(std::string(text));
  size_t pos = 0;
  int year = 0;
  int month = 0;
  int day = 0;
  if (!ParseUnsigned(s, &pos, &year) || pos >= s.size() || s[pos] != '-') {
    return Status(StatusCode::kInvalidArgument, "invalid date: " + s);
  }
  ++pos;
  if (!ParseUnsigned(s, &pos, &month) || pos >= s.size() || s[pos] != '-') {
    return Status(StatusCode::kInvalidArgument, "invalid date: " + s);
  }
  ++pos;
  if (!ParseUnsigned(s, &pos, &day) || pos != s.size()) {
    return Status(StatusCode::kInvalidArgument, "invalid date: " + s);
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return Status(StatusCode::kInvalidArgument, "invalid date: " + s);
  }
  return static_cast<int32_t>(DaysFromCivil(year, month, day));
}

std::string FormatDate(int32_t days) {
  int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02u",
                static_cast<long long>(year), month, day);
  return buffer;
}

std::string FormatRid(RID rid) {
  return std::to_string(rid.page_id) + ":" + std::to_string(rid.slot);
}

Result<RID> ParseRid(std::string_view text) {
  const size_t colon = text.find(':');
  const auto parse = [](std::string_view part, uint64_t limit,
                        uint64_t* out) -> bool {
    if (part.empty() || part.size() > 10) return false;
    uint64_t value = 0;
    for (char c : part) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value > limit) return false;
    *out = value;
    return true;
  };
  uint64_t page = 0;
  uint64_t slot = 0;
  if (colon == std::string_view::npos ||
      !parse(text.substr(0, colon), kInvalidPageId - 1ULL, &page) ||
      !parse(text.substr(colon + 1), UINT16_MAX, &slot)) {
    return Status(StatusCode::kInvalidArgument,
                  "bad RID '" + std::string(text) + "', expected page:slot");
  }
  return RID{static_cast<page_id_t>(page), static_cast<slot_id_t>(slot)};
}

std::string FormatDouble(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  // Shortest text that reads back as the same double (what PostgreSQL 12+
  // prints by default): 4192.4, not 4192.39999999999964.
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

}  // namespace minidb
