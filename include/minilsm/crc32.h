#pragma once

#include <cstddef>
#include <cstdint>

namespace minilsm {

// Standard CRC-32 (the same one used by zip, gzip, and PNG).
uint32_t Crc32(const char* data, size_t n);

}  // namespace minilsm