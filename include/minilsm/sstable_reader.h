#pragma once

#include <string>
#include <vector>

#include "minilsm/status.h"
#include "minilsm/sstable_format.h"
#include "minilsm/bloomfilter.h"

namespace minilsm {
Status ReadSstableFile(const std::string& path, SstableView* view);
Status ReadBloomFilterBlock(const std::string& path, BloomFilterView* bloomfilter_);
}  // namespace minilsm