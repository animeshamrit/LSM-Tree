#include "minilsm/memtable.h"

namespace minilsm {

void MemTable::Put(const std::string& key, const std::string& value) {
  table_[key] = Entry{false, value};
}

void MemTable::Delete(const std::string& key) {
  table_[key] = Entry{true, ""};
}

MemTable::GetResult MemTable::Get(const std::string& key,
                                  std::string* value) const {
  auto it = table_.find(key);
  if (it == table_.end()) {
    return GetResult::kNotPresent;
  }
  if (it->second.is_tombstone) {
    return GetResult::kDeleted;
  }
  if(value!= nullptr)
    *value = it->second.value;
  return GetResult::kFound;
}

void MemTable::clear() {
  table_.clear();
}

size_t MemTable::approximate_size() const {
  size_t total_size = 0;
  for (const auto& kv : table_) {
    total_size += kv.first.size() + kv.second.value.size() + 1;  // +1 for tombstone flag
  }
  return total_size;
}

std::vector<SstEntry> MemTable::Snapshot() const{
  std::vector<SstEntry> entries;
  entries.reserve(table_.size());

  for (const auto& kv : table_) {
    SstEntry entry;
    entry.is_tombstone = kv.second.is_tombstone;
    entry.key = kv.first;
    entry.value = kv.second.is_tombstone ? std::string() : kv.second.value;
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace minilsm