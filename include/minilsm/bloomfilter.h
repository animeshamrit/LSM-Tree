#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <functional>
#include <fstream>
#include <cstdint>
#include <algorithm>

#include "minilsm/status.h"


namespace minilsm {

struct BloomFilterView{
    std::vector<uint8_t> bits_;
    uint64_t num_bits_;
    uint64_t num_hashes_;
};

class BloomFilter {
    private:
        std::vector<uint8_t> bits_;
        uint64_t num_bits_;
        uint64_t num_hashes_;

        size_t hash1(const std::string& key) const;
        size_t hash2(const std::string& key) const;

    public:
        BloomFilter(size_t num_elements, double false_positive_rate = 0.01);
        BloomFilter(size_t num_bits, size_t num_hashes, std::vector<uint8_t> data)
        : num_bits_(num_bits), num_hashes_(num_hashes), bits_(std::move(data)) {}

        void add(const std::string& key);
        bool may_contain(const std::string &key) const;
        void serialize(std::string& out) const;
        static Status deserialize(std::string data, BloomFilterView* bloomfilter_);

        uint64_t sst_filenum = 1 ;
        
};
}

