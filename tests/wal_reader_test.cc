#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "minilsm/wal_reader.h"
#include "minilsm/wal_record.h"
#include "minilsm/wal_writer.h"

using minilsm::EncodeWalRecord;
using minilsm::ReadWal;
using minilsm::WalReadResult;
using minilsm::WalRecord;
using minilsm::WalWriter;

namespace {

WalRecord MakePut(const std::string& k, const std::string& v) {
  WalRecord r;
  r.type = WalRecord::Type::kPut;
  r.key = k;
  r.value = v;
  return r;
}

size_t EncodedSize(const WalRecord& r) {
  std::string buf;
  EncodeWalRecord(r, &buf);
  return buf.size();
}

void FlipByte(const std::string& path, std::streamoff pos) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(pos);
  char c;
  f.get(c);
  c ^= 0x01;
  f.seekp(pos);
  f.put(c);
}

}  // namespace

class WalReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/minilsm_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir_ = tmpl;
    path_ = dir_ + "/wal.log";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  void WriteRecords(const std::vector<WalRecord>& records) {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path_, false, &w).ok());
    for (const auto& r : records) ASSERT_TRUE(w->Append(r).ok());
  }

  std::string dir_;
  std::string path_;
};

TEST_F(WalReaderTest, ReadsAllRecordsInOrder) {
  WriteRecords({MakePut("a", "1"), MakePut("b", "2"), MakePut("c", "3")});

  WalReadResult res;
  ASSERT_TRUE(ReadWal(path_, &res).ok());
  ASSERT_EQ(res.records.size(), 3u);
  EXPECT_EQ(res.records[0].key, "a");
  EXPECT_EQ(res.records[2].key, "c");
  EXPECT_TRUE(res.clean);
  EXPECT_EQ(res.valid_bytes, res.file_size);
}

TEST_F(WalReaderTest, MissingFileIsNotFound) {
  WalReadResult res;
  EXPECT_TRUE(ReadWal(path_, &res).IsNotFound());
}

TEST_F(WalReaderTest, EmptyFileIsCleanAndEmpty) {
  { std::ofstream create(path_); }
  WalReadResult res;
  ASSERT_TRUE(ReadWal(path_, &res).ok());
  EXPECT_TRUE(res.records.empty());
  EXPECT_TRUE(res.clean);
}

TEST_F(WalReaderTest, TornTailKeepsGoodPrefix) {
  WalRecord a = MakePut("a", "1");
  WriteRecords({a, MakePut("b", "2")});
  // Simulate a crash mid-write: chop 3 bytes off the last record.
  std::filesystem::resize_file(path_, std::filesystem::file_size(path_) - 3);

  WalReadResult res;
  ASSERT_TRUE(ReadWal(path_, &res).ok());
  ASSERT_EQ(res.records.size(), 1u);
  EXPECT_EQ(res.records[0].key, "a");
  EXPECT_FALSE(res.clean);
  EXPECT_EQ(res.valid_bytes, EncodedSize(a));
}

TEST_F(WalReaderTest, CorruptionInFirstRecordYieldsNothing) {
  WriteRecords({MakePut("a", "1"), MakePut("b", "2")});
  FlipByte(path_, 14);  // a byte inside the first record

  WalReadResult res;
  ASSERT_TRUE(ReadWal(path_, &res).ok());
  EXPECT_TRUE(res.records.empty());
  EXPECT_FALSE(res.clean);
  EXPECT_EQ(res.valid_bytes, 0u);
}

TEST_F(WalReaderTest, CorruptionInMiddleStopsThere) {
  WalRecord a = MakePut("a", "1");
  WriteRecords({a, MakePut("b", "2"), MakePut("c", "3")});
  FlipByte(path_, static_cast<std::streamoff>(EncodedSize(a)) + 14);

  WalReadResult res;
  ASSERT_TRUE(ReadWal(path_, &res).ok());
  ASSERT_EQ(res.records.size(), 1u);   // "c" is NOT replayed
  EXPECT_EQ(res.records[0].key, "a");
  EXPECT_FALSE(res.clean);
}