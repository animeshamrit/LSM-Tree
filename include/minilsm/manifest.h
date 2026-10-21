#pragma once

#include <string>
#include <vector>

#include "minilsm/status.h"

namespace minilsm {

class Manifest {
    public:
    static void EncodeManifestRecord(const Manifest& record, std::string* dst);
    static Status DecodeManifestRecord(const std::string& src, Manifest* record);
    
    const uint64_t get_max_sstable_filenum();
    void set_max_sstable_filenum(uint64_t max_filenum);

    const std::vector< uint64_t > get_sstable_filenum();
    void set_sstable_filenum(std::vector< uint64_t >filenum);

    private:
    uint64_t max_sstable_filenum = 0;
    std::vector< uint64_t > sstable_filenum;
};

// Appends the encoded record to *dst.


}  // namespace minilsm