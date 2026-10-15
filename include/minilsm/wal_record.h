#pragma once

// #include <cstddef>
// #include <cstdint>
#include <string>

#include "minilsm/status.h"

namespace minilsm {

struct WalRecord {
  enum class Type : uint8_t { kPut = 1, kDelete = 2 };

  Type type = Type::kPut;
  std::string key;
  std::string value;  // empty for kDelete
};

// Appends the encoded record to *dst.
void EncodeWalRecord(const WalRecord& record, std::string* dst);

// Reads one record from src starting at *offset.
// On success, fills *record and advances *offset past it.
// On failure, returns Corruption and leaves *offset unchanged.
Status DecodeWalRecord(const std::string& src, size_t* offset,
                       WalRecord* record);

}  // namespace minilsm