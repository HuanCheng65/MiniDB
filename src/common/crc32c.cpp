#include "common/crc32c.h"

namespace minidb {
namespace {

constexpr uint32_t kPolynomial = 0x82F63B78u;  // reflected CRC32C polynomial

struct CrcTable {
  uint32_t entries[256];
  constexpr CrcTable() : entries{} {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) ? (crc >> 1) ^ kPolynomial : crc >> 1;
      }
      entries[i] = crc;
    }
  }
};

constexpr CrcTable kTable{};

}  // namespace

uint32_t Crc32c(const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc = kTable.entries[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace minidb
