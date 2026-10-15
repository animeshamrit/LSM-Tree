#pragma once

#include <string>
#include <vector>

#include "minilsm/status.h"
#include "minilsm/bloomfilter.h"

namespace minilsm {

struct SstEntry {
  bool is_tombstone = false;
  std::string key;
  std::string value;  // empty for tombstones
};

// entries must be sorted by key with no duplicates.
Status BuildSstable(const std::vector<SstEntry>& entries, std::string& bits, std::string* out);

// A parsed, verified SSTable held in memory.
class SstableView {
 public:
  enum class GetResult { kFound, kDeleted, kNotPresent };

  // Verifies magic and checksum, then builds the index.
  static Status Parse(std::string data, SstableView* view);
  static Status ParseBloomFilter(std::string data, BloomFilter* bloomfilter_);

  GetResult Get(const std::string& key, std::string* value) const;
  size_t entry_count() const { return index_.size(); }

 private:
  struct IndexItem {
    std::string key;
    uint64_t offset;
  };

  std::string data_;
  std::vector<IndexItem> index_;
};

}  // namespace minilsm