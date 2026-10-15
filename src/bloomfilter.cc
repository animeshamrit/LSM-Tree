#include "minilsm/bloomfilter.h"
#include "minilsm/coding.h"
#include "minilsm/crc32.h"

#include <iostream>

namespace minilsm {

namespace {
    constexpr size_t kBloomFilterFooterSize = 8 + 4;
}

    BloomFilter::BloomFilter(size_t num_elements, double false_positive_rate) {
        if (num_elements == 0) num_elements = 1;
        
        // Mathematical formula for optimal bit size: m = - (n * ln(p)) / (ln(2)^2)
        num_bits_ = static_cast<size_t>(-static_cast<double>(num_elements) * std::log(false_positive_rate) / (std::log(2) * std::log(2)));
        
        // Mathematical formula for optimal hash count: k = (m / n) * ln(2)
        num_hashes_ = std::max(1.0, std::round((static_cast<double>(num_bits_) / num_elements) * std::log(2)));
        
        // Align bit count to a multiple of 8 (bytes)
        num_bits_ = ((num_bits_ + 7) / 8) * 8;
        bits_.resize(num_bits_ / 8, 0);
    }

    size_t BloomFilter::hash1(const std::string& key) const {
        return std::hash<std::string>{}(key);
        }

    size_t BloomFilter::hash2(const std::string& key) const {
        size_t h = 14695981039346656037ULL;
        for (char c : key) {
            h ^= static_cast<size_t>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }
    
    // Insert a key into the Bloom filter
    void BloomFilter::add(const std::string& key) {
        size_t h1 = hash1(key);
        size_t h2 = hash2(key);
        for (size_t i = 0; i < num_hashes_; ++i) {
            size_t combined_hash = (h1 + i * h2) % num_bits_;
            size_t byte_idx = combined_hash / 8;
            size_t bit_idx = combined_hash % 8;
            bits_[byte_idx] |= (1 << bit_idx);
        }
    }

    // Check if a key might be present
    bool BloomFilter::may_contain(const std::string& key) const {
        size_t h1 = hash1(key);
        size_t h2 = hash2(key);
        for (size_t i = 0; i < num_hashes_; ++i) {
            size_t combined_hash = (h1 + i * h2) % num_bits_;
            size_t byte_idx = combined_hash / 8;
            size_t bit_idx = combined_hash % 8;
            if (!(bits_[byte_idx] & (1 << bit_idx))) {
                return false; // Definitely not in the set!
            }
        }
        return true; // Might be in the set
    }

    void BloomFilter::serialize(std::string& out) const {
        uint64_t size = bits_.size();
        uint64_t nb = num_bits_;
        uint64_t nh = num_hashes_;

        PutFixed64(&out, size);
        PutFixed64(&out, nb);
        PutFixed64(&out, nh);
        std::string str(reinterpret_cast<const char*>(bits_.data()), bits_.size());
        out.append(str);
    }

    Status BloomFilter::deserialize(std::string data, BloomFilterView* bloomfilter_) {
    if (data.size() < kBloomFilterFooterSize) {
        return Status::Corruption("sstable too small");
    }
    const size_t footer_pos = data.size() - kBloomFilterFooterSize;
    const char* f = data.data() + footer_pos;

    const uint32_t stored_crc = GetFixed32(f + 8);
    if (Crc32(data.data(), footer_pos + 8) != stored_crc) {
        return Status::Corruption("checksum mismatch");
    }

    // Everything below is now trustworthy, but we still bounds-check
    // so a buggy writer can't make us read out of range.
    const uint64_t index_offset = 8 + 8 + 8;
    const uint64_t count = GetFixed64(data.data());
    if (index_offset > footer_pos) {
        return Status::Corruption("bad index offset");
    }

    const uint64_t num_bits_ = GetFixed64(data.data() + 8);
    const uint64_t num_hashes_ = GetFixed64(data.data() + 16);
    std::vector<uint8_t> bits_;

    if (count > footer_pos - index_offset) 
        return Status::Corruption("bad bloom count");
        
    for (uint64_t i = 0; i < count; i++) {
        bits_.push_back(static_cast<uint8_t>(data[index_offset + i]));
    }

    bloomfilter_->num_bits_ = std::move(num_bits_);
    bloomfilter_->num_hashes_ = std::move(num_hashes_);
    bloomfilter_->bits_ = std::move(bits_);
    return Status::OK();
    }

}  