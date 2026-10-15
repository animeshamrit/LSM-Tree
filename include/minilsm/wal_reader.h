#pragma once

#include <string>
#include <vector>

#include "minilsm/status.h"
#include "minilsm/wal_record.h"

namespace minilsm {

struct WalReadResult {
  std::vector<WalRecord> records;  // the good prefix, in order
  size_t valid_bytes = 0;          // where the last good record ends
  size_t file_size = 0;
  bool clean = true;               // false if we stopped on a bad record
};

// Returns NotFound if the file doesn't exist, IOError if it can't be read.
// A corrupt record is NOT an error here: it ends the good prefix
// and sets result->clean = false.
Status ReadWal(const std::string& path, WalReadResult* result);

}  // namespace minilsm