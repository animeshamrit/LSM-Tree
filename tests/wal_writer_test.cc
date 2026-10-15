#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "minilsm/wal_record.h"
#include "minilsm/wal_writer.h"

using minilsm::DecodeWalRecord;
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

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

// A fixture gives every test a fresh temp directory and cleans it up.
class WalWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/minilsm_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir_ = tmpl;
    path_ = dir_ + "/wal.log";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string dir_;
  std::string path_;
};

TEST_F(WalWriterTest, AppendedRecordsCanBeDecoded) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(path_, /*sync=*/false, &w).ok());
  ASSERT_TRUE(w->Append(MakePut("a", "1")).ok());
  ASSERT_TRUE(w->Append(MakePut("b", "2")).ok());
  ASSERT_TRUE(w->Close().ok());

  std::string bytes = ReadAll(path_);
  size_t offset = 0;
  WalRecord r1, r2;
  ASSERT_TRUE(DecodeWalRecord(bytes, &offset, &r1).ok());
  ASSERT_TRUE(DecodeWalRecord(bytes, &offset, &r2).ok());
  EXPECT_EQ(r1.key, "a");
  EXPECT_EQ(r2.key, "b");
  EXPECT_EQ(offset, bytes.size());
}

TEST_F(WalWriterTest, ReopenAppendsInsteadOfTruncating) {
  {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path_, false, &w).ok());
    ASSERT_TRUE(w->Append(MakePut("first", "1")).ok());
  }  // destructor closes the file here
  {
    std::unique_ptr<WalWriter> w;
    ASSERT_TRUE(WalWriter::Open(path_, false, &w).ok());
    ASSERT_TRUE(w->Append(MakePut("second", "2")).ok());
  }

  std::string bytes = ReadAll(path_);
  size_t offset = 0;
  WalRecord r1, r2;
  ASSERT_TRUE(DecodeWalRecord(bytes, &offset, &r1).ok());
  ASSERT_TRUE(DecodeWalRecord(bytes, &offset, &r2).ok());
  EXPECT_EQ(r1.key, "first");
  EXPECT_EQ(r2.key, "second");
}

TEST_F(WalWriterTest, SyncModeWorks) {
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(path_, /*sync=*/true, &w).ok());
  EXPECT_TRUE(w->Append(MakePut("k", "v")).ok());
  EXPECT_FALSE(ReadAll(path_).empty());
}

TEST_F(WalWriterTest, OpenInMissingDirectoryIsIOError) {
  std::unique_ptr<WalWriter> w;
  minilsm::Status s =
      WalWriter::Open(dir_ + "/no_such_dir/wal.log", false, &w);
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(w, nullptr);
}

TEST_F(WalWriterTest, LargeRecordIsFullyWritten) {
  std::string big(1 << 20, 'x');  // 1 MB value
  std::unique_ptr<WalWriter> w;
  ASSERT_TRUE(WalWriter::Open(path_, false, &w).ok());
  ASSERT_TRUE(w->Append(MakePut("big", big)).ok());

  std::string bytes = ReadAll(path_);
  size_t offset = 0;
  WalRecord r;
  ASSERT_TRUE(DecodeWalRecord(bytes, &offset, &r).ok());
  EXPECT_EQ(r.value.size(), big.size());
}