#include "minilsm/wal_record.h"
#include "minilsm/crc32.h"
#include "minilsm/coding.h"

namespace minilsm {

namespace {

constexpr size_t kCrcSize = 4;
constexpr size_t kHeaderSize = 4 + 1 + 4 + 4;  // crc + type + key_len + value_len

}  // namespace

void EncodeWalRecord(const WalRecord& record, std::string* dst) {
  // Build the part the CRC covers first.
  std::string body;
  body.push_back(static_cast<char>(record.type));
  PutFixed32(&body, static_cast<uint32_t>(record.key.size()));
  PutFixed32(&body, static_cast<uint32_t>(record.value.size()));
  body.append(record.key);
  body.append(record.value);

  PutFixed32(dst, Crc32(body.data(), body.size()));
  dst->append(body);
}

Status DecodeWalRecord(const std::string& src, size_t* offset, WalRecord* record) {
  const size_t pos = *offset;
  if (pos > src.size() || src.size() - pos < kHeaderSize) {
    return Status::Corruption("truncated record header");
  }

  const char* p = src.data() + pos;
  const uint32_t stored_crc = GetFixed32(p);
  const char* body = p + kCrcSize;  

  const uint32_t key_len = GetFixed32(p + 5);
  const uint32_t value_len = GetFixed32(p + 9);

  const size_t remaining = src.size() - pos - kHeaderSize;
  if (key_len > remaining || value_len > remaining - key_len) {
    return Status::Corruption("truncated record body");
  }

   // Checksum covers type + lengths + key + value.
  const size_t body_size = (kHeaderSize - kCrcSize)+ key_len + value_len;
  if (Crc32(body, body_size) != stored_crc) {
    return Status::Corruption("checksum mismatch");
  }

  const uint8_t type = static_cast<uint8_t>(*(body+0));
  if (type != static_cast<uint8_t>(WalRecord::Type::kPut) &&
      type != static_cast<uint8_t>(WalRecord::Type::kDelete)) {
    return Status::Corruption("unknown record type");
  }

  record->type = static_cast<WalRecord::Type>(type);
  record->key.assign(p + kHeaderSize, key_len);
  record->value.assign(p + kHeaderSize + key_len, value_len);
  *offset = pos + kHeaderSize + key_len + value_len;
  return Status::OK();
}

}  // namespace minilsm