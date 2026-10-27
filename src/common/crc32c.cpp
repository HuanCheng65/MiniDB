#include "common/crc32c.h"

#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define MINIDB_CRC32C_X86 1
#endif

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

#ifdef MINIDB_CRC32C_X86
// Compiled for SSE4.2 but only called after a runtime CPU check, so the
// binary still runs on machines without the instruction (design doc G5).
__attribute__((target("sse4.2"))) uint32_t Crc32cHardware(
    uint32_t crc, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t state = ~crc;
  while (length >= sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, bytes, sizeof(word));
    state = _mm_crc32_u64(state, word);
    bytes += sizeof(word);
    length -= sizeof(word);
  }
  auto narrow = static_cast<uint32_t>(state);
  while (length > 0) {
    narrow = _mm_crc32_u8(narrow, *bytes);
    ++bytes;
    --length;
  }
  return ~narrow;
}
#endif

using CrcFunction = uint32_t (*)(uint32_t, const void*, size_t);

CrcFunction SelectImplementation() {
#ifdef MINIDB_CRC32C_X86
  __builtin_cpu_init();
  if (__builtin_cpu_supports("sse4.2")) return &Crc32cHardware;
#endif
  return &Crc32cPortable;
}

// Function-local static: safe even if a checksum is needed during another
// translation unit's static initialization.
CrcFunction Implementation() {
  static const CrcFunction implementation = SelectImplementation();
  return implementation;
}

}  // namespace

uint32_t Crc32cPortable(uint32_t crc, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t state = ~crc;
  for (size_t i = 0; i < length; ++i) {
    state = kTable.entries[(state ^ bytes[i]) & 0xFFu] ^ (state >> 8);
  }
  return ~state;
}

uint32_t Crc32cExtend(uint32_t crc, const void* data, size_t length) {
  return Implementation()(crc, data, length);
}

uint32_t Crc32c(const void* data, size_t length) {
  return Implementation()(0, data, length);
}

bool Crc32cIsHardwareAccelerated() {
#ifdef MINIDB_CRC32C_X86
  return Implementation() == &Crc32cHardware;
#else
  return false;
#endif
}

}  // namespace minidb
