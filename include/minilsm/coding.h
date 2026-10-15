#pragma once

#include <cstdint>
#include <string>

namespace minilsm {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  dst->append(buf, 8);
}

inline uint32_t GetFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline uint64_t GetFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

}  // namespace minilsm