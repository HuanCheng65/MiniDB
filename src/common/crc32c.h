// CRC32C (Castagnoli), used for page checksums (design doc 5.2).
#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

uint32_t Crc32c(const void* data, size_t length);

// Continues a CRC32C: Crc32cExtend(Crc32c(a), b) == Crc32c(a || b), and
// Crc32cExtend(0, data, n) == Crc32c(data, n).
uint32_t Crc32cExtend(uint32_t crc, const void* data, size_t length);

// Portable table-driven version; exposed so tests can compare it with the
// hardware path.
uint32_t Crc32cPortable(uint32_t crc, const void* data, size_t length);

// True when Crc32c() uses the SSE4.2 crc32 instruction.
bool Crc32cIsHardwareAccelerated();

}  // namespace minidb
