// CRC32C (Castagnoli), used for page checksums (design doc 5.2).
#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

uint32_t Crc32c(const void* data, size_t length);

}  // namespace minidb
