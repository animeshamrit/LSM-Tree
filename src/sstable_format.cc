#include "minilsm/sstable_format.h"

#include <algorithm>

#include "minilsm/coding.h"
#include "minilsm/crc32.h"

namespace minilsm {

namespace {

constexpr uint64_t kMagic = 0x4D494E494C534D31ull;  // "MINILSM1"
constexpr size_t kFooterSize = 8 + 8 + 4 + 8;  // index_offset, count, crc, magic
constexpr size_t kEntryHeader = 4 + 4 + 1;     // key_len, value_len, type
}  // namespace

Status BuildSstable(const std::vector<SstEntry>& entries, std::string& bits, std::string* out) {
  for (size_t i = 1; i < entries.size(); i++) {
    if (!(entries[i - 1].key < entries[i].key)) {
      return Status::InvalidArgument("entries must be sorted and unique");
    }
  }

  std::string data;
  std::vector<uint64_t> offsets;
  for (const SstEntry& e : entries) {
    offsets.push_back(data.size());
    PutFixed32(&data, static_cast<uint32_t>(e.key.size()));
    PutFixed32(&data, static_cast<uint32_t>(e.value.size()));
    data.push_back(e.is_tombstone ? 1 : 0);
    data.append(e.key);
    data.append(e.value);
  }

  const uint64_t index_offset = data.size();
  for (size_t i = 0; i < entries.size(); i++) {
    PutFixed32(&data, static_cast<uint32_t>(entries[i].key.size()));
    data.append(entries[i].key);
    PutFixed64(&data, offsets[i]);
  }

  const uint64_t bloomfilter_offset = data.size();
  data.append(bits);
  PutFixed64(&data, bloomfilter_offset);
  const uint32_t crc_bloomfilterBlock = Crc32(data.data() + bloomfilter_offset, data.size() - bloomfilter_offset);
  PutFixed32(&data, crc_bloomfilterBlock);

  // The CRC covers everything written so far, plus the two footer fields
  // that come before it.
  PutFixed64(&data, index_offset);
  PutFixed64(&data, entries.size());
  const uint32_t crc = Crc32(data.data(), data.size());
  PutFixed32(&data, crc);
  PutFixed64(&data, kMagic);

  *out = std::move(data);
  return Status::OK();
}

Status SstableView::Parse(std::string data, SstableView* view) {
  if (data.size() < kFooterSize) {
    return Status::Corruption("sstable too small");
  }
  const size_t footer_pos = data.size() - kFooterSize;
  const char* f = data.data() + footer_pos;

  if (GetFixed64(f + 8 + 8 + 4) != kMagic) {
    return Status::Corruption("bad magic");
  }
  const uint32_t stored_crc = GetFixed32(f + 8 + 8);
  if (Crc32(data.data(), footer_pos + 16) != stored_crc) {
    return Status::Corruption("checksum mismatch");
  }

  // Everything below is now trustworthy, but we still bounds-check
  // so a buggy writer can't make us read out of range.
  const uint64_t index_offset = GetFixed64(f);
  const uint64_t count = GetFixed64(f + 8);
  if (index_offset > footer_pos) {
    return Status::Corruption("bad index offset");
  }

  std::vector<IndexItem> index;
  size_t pos = static_cast<size_t>(index_offset);
  for (uint64_t i = 0; i < count; i++) {
    if (footer_pos - pos < 4) return Status::Corruption("bad index");
    const uint32_t klen = GetFixed32(data.data() + pos);
    pos += 4;
    if (footer_pos - pos < klen || footer_pos - pos - klen < 8) {
      return Status::Corruption("bad index");
    }
    IndexItem item;
    item.key.assign(data.data() + pos, klen);
    pos += klen;
    item.offset = GetFixed64(data.data() + pos);
    pos += 8;
    if (item.offset >= index_offset) return Status::Corruption("bad offset");
    index.push_back(std::move(item));
  }

  view->data_ = std::move(data);
  view->index_ = std::move(index);
  return Status::OK();
}

SstableView::GetResult SstableView::Get(const std::string& key,
                                        std::string* value) const {
  auto it = std::lower_bound(
      index_.begin(), index_.end(), key,
      [](const IndexItem& item, const std::string& k) { return item.key < k; });
  if (it == index_.end() || it->key != key) return GetResult::kNotPresent;

  const char* p = data_.data() + it->offset;
  const uint32_t klen = GetFixed32(p);
  const uint32_t vlen = GetFixed32(p + 4);
  const bool tombstone = p[8] != 0;
  if (tombstone) return GetResult::kDeleted;
  value->assign(p + kEntryHeader + klen, vlen);
  return GetResult::kFound;
}


}  // namespace minilsm