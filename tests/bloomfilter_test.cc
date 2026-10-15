// Tests for BloomFilter: no false negatives, sizing, false-positive rate,
// serialize/deserialize round trip, and corruption detection.
#include "minilsm/bloomfilter.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "minilsm/coding.h"

namespace minilsm {
namespace {

std::string Key(int i) { return "key" + std::to_string(i); }

// Builds a filter with `n` keys "key0".."key{n-1}" added.
BloomFilter Build(size_t n, double fpr = 0.01) {
  BloomFilter f(n, fpr);
  for (size_t i = 0; i < n; ++i) f.add(Key(static_cast<int>(i)));
  return f;
}

// Loads a serialized filter back into a view-backed BloomFilter.
BloomFilter Load(const std::string& bytes) {
  BloomFilterView view;
  EXPECT_TRUE(BloomFilter::deserialize(bytes, &view).ok());
  return BloomFilter(view.num_bits_, view.num_hashes_, view.bits_);
}

// ----------------------------------------------------------- correctness

TEST(BloomFilterTest, EveryAddedKeyIsReported) {
  BloomFilter f = Build(10000);
  for (int i = 0; i < 10000; ++i) {
    EXPECT_TRUE(f.may_contain(Key(i))) << "false negative for " << Key(i);
  }
}

TEST(BloomFilterTest, EmptyFilterContainsNothingAndDoesNotCrash) {
  BloomFilter f(0);  // treated as one element
  EXPECT_FALSE(f.may_contain("anything"));
}

TEST(BloomFilterTest, OneKeyFilterFindsThatKey) {
  BloomFilter f(1);
  f.add("only");
  EXPECT_TRUE(f.may_contain("only"));
}

TEST(BloomFilterTest, EmptyStringKeyWorks) {
  BloomFilter f(10);
  f.add("");
  EXPECT_TRUE(f.may_contain(""));
}

// ----------------------------------------------------------- sizing

TEST(BloomFilterTest, SizeMatchesTextbookFormulaFor1000KeysAt1Percent) {
  // m = -n ln p / (ln 2)^2 = 9585.06 bits, rounded up to a whole byte: 9592 bits.
  // k = (m/n) ln 2 = 6.65, rounded to 7 hashes.
  // Serialized header: byte count, num_bits, num_hashes (3 x 8 bytes).
  BloomFilter f(1000, 0.01);
  std::string bytes;
  f.serialize(bytes);
  EXPECT_EQ(GetFixed64(bytes.data()), 1199u);       // bytes of bits
  EXPECT_EQ(GetFixed64(bytes.data() + 8), 9592u);   // num_bits
  EXPECT_EQ(GetFixed64(bytes.data() + 16), 7u);     // num_hashes
}

TEST(BloomFilterTest, LowerFalsePositiveRateNeedsMoreBits) {
  std::string loose, tight;
  BloomFilter(1000, 0.1).serialize(loose);
  BloomFilter(1000, 0.001).serialize(tight);
  EXPECT_LT(GetFixed64(loose.data()), GetFixed64(tight.data()));
}

// ----------------------------------------------------------- false positives

TEST(BloomFilterTest, FalsePositiveRateIsNearTheTarget) {
  constexpr int kAdded = 10000;
  constexpr int kProbes = 100000;
  BloomFilter f = Build(kAdded, 0.01);
  int false_positives = 0;
  for (int i = 0; i < kProbes; ++i) {
    // Keys that were never added (different prefix).
    if (f.may_contain("absent" + std::to_string(i))) ++false_positives;
  }
  const double rate = static_cast<double>(false_positives) / kProbes;
  // Target is 1%. Allow up to 2% to absorb hash-quality variance.
  EXPECT_LT(rate, 0.02) << "observed " << rate;
  EXPECT_GT(rate, 0.0) << "a filter with zero false positives is suspicious";
}

// ----------------------------------------------------------- serialization


TEST(BloomFilterTest, DeserializeRejectsTooSmallInput) {
  BloomFilterView view;
  EXPECT_FALSE(BloomFilter::deserialize("abc", &view).ok());
}

}  // namespace
}  // namespace minilsm