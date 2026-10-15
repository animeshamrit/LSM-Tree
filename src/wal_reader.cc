#include "minilsm/wal_reader.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace minilsm {

Status ReadWal(const std::string& path, WalReadResult* result) {
  *result = WalReadResult();

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return Status::NotFound("no wal file: " + path);
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Status::IOError("cannot open " + path);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  if (in.bad()) {
    return Status::IOError("read failed: " + path);
  }
  const std::string bytes = ss.str();
  result->file_size = bytes.size();

  size_t offset = 0;
  while (offset < bytes.size()) {
    WalRecord rec;
    if (!DecodeWalRecord(bytes, &offset, &rec).ok()) {
      result->clean = false;
      break;
    }
    result->records.push_back(std::move(rec));
  }
  result->valid_bytes = offset;
  return Status::OK();
}

}  // namespace minilsm