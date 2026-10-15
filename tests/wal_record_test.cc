#include <gtest/gtest.h>

#include <string>

#include "minilsm/wal_record.h"

using minilsm::DecodeWalRecord;
using minilsm::EncodeWalRecord;
using minilsm::WalRecord;

namespace {
WalRecord MakePut(const std::string& k, const std::string& v) {
  WalRecord r;
  r.type = WalRecord::Type::kPut;
  r.key = k;
  r.value = v;
  return r;
}
}  // namespace

TEST(WalRecordTest, PutRoundTrip) {
  std::string buf;
  EncodeWalRecord(MakePut("name", "alice"), &buf);

  size_t offset = 0;
  WalRecord out;
  ASSERT_TRUE(DecodeWalRecord(buf, &offset, &out).ok());
  EXPECT_EQ(out.type, WalRecord::Type::kPut);
  EXPECT_EQ(out.key, "name");
  EXPECT_EQ(out.value, "alice");
  EXPECT_EQ(offset, buf.size());
}

TEST(WalRecordTest, DeleteRoundTrip) {
  WalRecord r;
  r.type = WalRecord::Type::kDelete;
  r.key = "gone";

  std::string buf;
  EncodeWalRecord(r, &buf);

  size_t offset = 0;
  WalRecord out;
  ASSERT_TRUE(DecodeWalRecord(buf, &offset, &out).ok());
  EXPECT_EQ(out.type, WalRecord::Type::kDelete);
  EXPECT_EQ(out.key, "gone");
  EXPECT_EQ(out.value, "");
}

TEST(WalRecordTest, EmptyValueSurvives) {
  std::string buf;
  EncodeWalRecord(MakePut("k", ""), &buf);

  size_t offset = 0;
  WalRecord out;
  ASSERT_TRUE(DecodeWalRecord(buf, &offset, &out).ok());
  EXPECT_EQ(out.value, "");
}

TEST(WalRecordTest, TwoRecordsBackToBack) {
  std::string buf;
  EncodeWalRecord(MakePut("a", "1"), &buf);
  EncodeWalRecord(MakePut("b", "2"), &buf);

  size_t offset = 0;
  WalRecord r1, r2;
  ASSERT_TRUE(DecodeWalRecord(buf, &offset, &r1).ok());
  ASSERT_TRUE(DecodeWalRecord(buf, &offset, &r2).ok());
  EXPECT_EQ(r1.key, "a");
  EXPECT_EQ(r2.key, "b");
  EXPECT_EQ(offset, buf.size());
}

TEST(WalRecordTest, TruncatedBodyIsCorruption) {
  std::string buf;
  EncodeWalRecord(MakePut("key", "value"), &buf);
  buf.resize(buf.size() - 1);  // simulate a torn write

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
  EXPECT_EQ(offset, 0u);  // unchanged on failure
}

TEST(WalRecordTest, TruncatedHeaderIsCorruption) {
  std::string buf = "abc";  // shorter than the 13-byte header

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
}

TEST(WalRecordTest, UnknownTypeIsCorruption) {
  std::string buf;
  EncodeWalRecord(MakePut("k", "v"), &buf);
  buf[4] = static_cast<char>(99);  // not a valid type

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
}

TEST(WalRecordTest, FlippedValueByteIsCorruption) {
  std::string buf;
  EncodeWalRecord(MakePut("key", "value"), &buf);
  buf[buf.size() - 1] ^= 0x01;  // flip one bit in the last value byte

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
  EXPECT_EQ(offset, 0u);
}

TEST(WalRecordTest, FlippedKeyByteIsCorruption) {
  std::string buf;
  EncodeWalRecord(MakePut("key", "value"), &buf);
  buf[13] ^= 0x01;  // first key byte (header is 13 bytes)

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
}

TEST(WalRecordTest, FlippedCrcByteIsCorruption) {
  std::string buf;
  EncodeWalRecord(MakePut("key", "value"), &buf);
  buf[0] ^= 0x01;  // damage the stored checksum itself

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
}

TEST(WalRecordTest, FlippedLengthByteIsCorruption) {
  std::string buf;
  EncodeWalRecord(MakePut("key", "value"), &buf);
  buf[5] ^= 0x01;  // low byte of key_len

  size_t offset = 0;
  WalRecord out;
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &out).IsCorruption());
}

TEST(WalRecordTest, CorruptSecondRecordKeepsFirstReadable) {
  std::string buf;
  EncodeWalRecord(MakePut("a", "1"), &buf);
  const size_t first_end = buf.size();
  EncodeWalRecord(MakePut("b", "2"), &buf);
  buf[buf.size() - 1] ^= 0x01;  // damage only the second record

  size_t offset = 0;
  WalRecord r;
  ASSERT_TRUE(DecodeWalRecord(buf, &offset, &r).ok());
  EXPECT_EQ(r.key, "a");
  EXPECT_EQ(offset, first_end);
  EXPECT_TRUE(DecodeWalRecord(buf, &offset, &r).IsCorruption());
  EXPECT_EQ(offset, first_end);  // marks where valid data ends
}