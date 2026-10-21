#include "minilsm/manifest.h"
#include "minilsm/crc32.h"
#include "minilsm/coding.h"

namespace minilsm {

namespace {

constexpr size_t kCrcSize = 4;
constexpr size_t kHeaderSize = 4 + 8;  // crc + counter

}  // namespace

const uint64_t Manifest::get_max_sstable_filenum() {
    return max_sstable_filenum;
}

void Manifest::set_max_sstable_filenum(uint64_t max_filenum) {
    max_sstable_filenum = max_filenum;
}
const std::vector< uint64_t > Manifest::get_sstable_filenum() {
    return sstable_filenum;
}
void Manifest::set_sstable_filenum(std::vector< uint64_t >filenum) {
    sort(filenum.begin(), filenum.end());
    sstable_filenum = filenum;
}

void Manifest::EncodeManifestRecord(const Manifest& record, std::string* dst) {
  // Build the part the CRC covers first.
  std::string body;
  PutFixed64(&body, static_cast<uint64_t>(record.max_sstable_filenum));
  
  for(auto num : record.sstable_filenum){
    PutFixed64(&body, static_cast<uint64_t>(num));
  }

  PutFixed32(dst, Crc32(body.data(), body.size()));
  dst->append(body);
}

Status Manifest::DecodeManifestRecord(const std::string& src, Manifest* record) {

  *record = Manifest();

  const char* p = src.data();
  const char* body = p + kCrcSize;  

  if(src.size() < kHeaderSize)
  {
    return Status::Corruption("truncated record body");
  }
  const size_t remaining = src.size() - kHeaderSize;
  if (remaining % 8 != 0) {
    return Status::Corruption("truncated record body");
  }

  const uint32_t stored_crc = GetFixed32(p);
   const uint64_t max_sstable_filenum = GetFixed64(p + 4);

   // Checksum covers type + lengths + key + value.
  const size_t body_size = remaining + 8;
  if (Crc32(body, body_size) != stored_crc) {
    return Status::Corruption("checksum mismatch");
  }

  p += kHeaderSize;
  for(auto it = 0; it < remaining; it+=8){
    uint64_t sstable_filenum = GetFixed64(p);
    record->sstable_filenum.push_back(sstable_filenum);
    p+=8;
  }
  
  record->max_sstable_filenum = max_sstable_filenum;
  return Status::OK();
}

}  // namespace minilsm