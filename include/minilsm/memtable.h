#pragma once

#include <map>
#include <string>
#include <vector>

#include "minilsm/sstable_format.h"

namespace minilsm {

class MemTable {
private:
  struct Entry {
    bool is_tombstone = false;
    std::string value = "";  // empty for tombstones
  };
  std::map<std::string, Entry> table_;
  
 public:
  enum class GetResult { kFound, kDeleted, kNotPresent };

  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key);
  GetResult Get(const std::string& key, std::string* value) const;
  void clear();
  size_t approximate_size() const;

  std::vector<SstEntry> Snapshot() const;

};

}  