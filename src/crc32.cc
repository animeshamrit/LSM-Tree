#include "minilsm/crc32.h"

namespace minilsm {

uint32_t Crc32(const char* data, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (int bit = 0; bit < 8; bit++) {
      const uint32_t mask = 0u - (crc & 1u);  // all ones if low bit set, else 0
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

}  // namespace minilsm