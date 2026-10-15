#include <gtest/gtest.h>

#include <string>

#include "minilsm/crc32.h"

using minilsm::Crc32;

TEST(Crc32Test, KnownVector) {
  // The standard check value for CRC-32.
  std::string s = "123456789";
  EXPECT_EQ(Crc32(s.data(), s.size()), 0xCBF43926u);
}

TEST(Crc32Test, EmptyInputIsZero) {
  EXPECT_EQ(Crc32("", 0), 0u);
}

TEST(Crc32Test, SameInputSameOutput) {
  std::string s = "hello";
  EXPECT_EQ(Crc32(s.data(), s.size()), Crc32(s.data(), s.size()));
}

TEST(Crc32Test, SingleBitFlipChangesChecksum) {
  std::string a = "hello world";
  std::string b = a;
  b[3] ^= 0x01;  // flip one bit
  EXPECT_NE(Crc32(a.data(), a.size()), Crc32(b.data(), b.size()));
}

TEST(Crc32Test, OrderMatters) {
  std::string a = "ab";
  std::string b = "ba";
  EXPECT_NE(Crc32(a.data(), a.size()), Crc32(b.data(), b.size()));
}
