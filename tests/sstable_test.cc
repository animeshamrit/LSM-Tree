// Tests for the SSTable file: format (BuildSstable / Parse / Get), file reads
// (ReadSstableFile), and the bloom filter block (ReadBloomFilterBlock).
#include "minilsm/sstable_format.h"
#include "minilsm/sstable_reader.h"
#include "minilsm/bloomfilter.h"
#include "minilsm/coding.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace minilsm {
namespace {

namespace fs = std::filesystem;
using R = SstableView::GetResult;

SstEntry Put(const std::string& k, const std::string& v) {
  SstEntry e; e.key = k; e.value = v; return e;
}
SstEntry Del(const std::string& k) {
  SstEntry e; e.key = k; e.is_tombstone = true; return e;
}

// Builds a table with an empty bloom block (format tests don't need one).
std::string Build(const std::vector<SstEntry>& entries) {
  std::string bits, out;
  EXPECT_TRUE(BuildSstable(entries, bits, &out).ok());
  return out;
}

SstableView Load(const std::string& bytes) {
  SstableView v;
  EXPECT_TRUE(SstableView::Parse(bytes, &v).ok());
  return v;
}

class SstableFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = (fs::temp_directory_path() /
            ("sstable_test_" + std::to_string(::getpid()) + "_" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name()))
               .string();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  std::string WriteFile(const std::string& name, const std::string& bytes) {
    const std::string path = dir_ + "/" + name;
    std::ofstream(path, std::ios::binary) << bytes;
    return path;
  }
  std::string dir_;
};

// ===================================================================== format

TEST(SstableFormatTest, RoundTripFindsEveryKeyAndTombstone) {
  std::string bytes = Build({Put("a", "1"), Put("b", "two"), Del("c"), Put("d", "")});
  SstableView v = Load(bytes);
  std::string out;
  EXPECT_EQ(v.Get("a", &out), R::kFound);  EXPECT_EQ(out, "1");
  EXPECT_EQ(v.Get("b", &out), R::kFound);  EXPECT_EQ(out, "two");
  EXPECT_EQ(v.Get("c", &out), R::kDeleted);
  EXPECT_EQ(v.Get("d", &out), R::kFound);  EXPECT_EQ(out, "");
}

TEST(SstableFormatTest, MissingKeysAreNotPresent) {
  SstableView v = Load(Build({Put("b", "1"), Put("d", "2")}));
  std::string out;
  EXPECT_EQ(v.Get("a", &out), R::kNotPresent);  // before the first key
  EXPECT_EQ(v.Get("c", &out), R::kNotPresent);  // between keys
  EXPECT_EQ(v.Get("e", &out), R::kNotPresent);  // after the last key
}

TEST(SstableFormatTest, EmptyTableIsValid) {
  SstableView v = Load(Build({}));
  std::string out;
  EXPECT_EQ(v.Get("anything", &out), R::kNotPresent);
}

TEST(SstableFormatTest, EmptyKeyAndEmptyValueWork) {
  SstableView v = Load(Build({Put("", "x"), Put("k", "")}));
  std::string out;
  EXPECT_EQ(v.Get("", &out), R::kFound);  EXPECT_EQ(out, "x");
  EXPECT_EQ(v.Get("k", &out), R::kFound); EXPECT_EQ(out, "");
}

TEST(SstableFormatTest, ManyKeysAllFoundByBinarySearch) {
  std::vector<SstEntry> entries;
  for (int i = 0; i < 2000; ++i) {
    char k[16];
    std::snprintf(k, sizeof(k), "key%05d", i);
    entries.push_back(Put(k, "v" + std::to_string(i)));
  }
  SstableView v = Load(Build(entries));
  for (int i = 0; i < 2000; ++i) {
    char k[16];
    std::snprintf(k, sizeof(k), "key%05d", i);
    std::string out;
    ASSERT_EQ(v.Get(k, &out), R::kFound) << k;
    EXPECT_EQ(out, "v" + std::to_string(i));
  }
}

TEST(SstableFormatTest, RejectsUnsortedInput) {
  std::string bits, out;
  EXPECT_FALSE(BuildSstable({Put("b", "1"), Put("a", "2")}, bits, &out).ok());
}

TEST(SstableFormatTest, RejectsDuplicateKeys) {
  std::string bits, out;
  EXPECT_FALSE(BuildSstable({Put("a", "1"), Put("a", "2")}, bits, &out).ok());
}

TEST(SstableFormatTest, ParseRejectsTooSmallInput) {
  SstableView v;
  EXPECT_FALSE(SstableView::Parse("short", &v).ok());
}

TEST(SstableFormatTest, FlippingAnyByteIsDetected) {
  std::string bytes = Build({Put("a", "1"), Put("b", "2")});
  SstableView clean;
  ASSERT_TRUE(SstableView::Parse(bytes, &clean).ok()) << "clean table must load";
  for (size_t pos = 0; pos < bytes.size(); ++pos) {
    std::string damaged = bytes;
    damaged[pos] = static_cast<char>(damaged[pos] ^ 0x40);
    SstableView v;
    EXPECT_FALSE(SstableView::Parse(damaged, &v).ok())
        << "undetected corruption at byte " << pos;
  }
}

TEST(SstableFormatTest, EveryTruncationIsDetected) {
  std::string bytes = Build({Put("a", "1"), Put("b", "2")});
  SstableView clean;
  ASSERT_TRUE(SstableView::Parse(bytes, &clean).ok()) << "clean table must load";
  for (size_t len = 0; len < bytes.size(); ++len) {
    SstableView v;
    EXPECT_FALSE(SstableView::Parse(bytes.substr(0, len), &v).ok())
        << "accepted truncation to " << len;
  }
}

// ==================================================================== files

TEST_F(SstableFileTest, ReadSstableFileRoundTrip) {
  const std::string path = WriteFile("sst-000001.sst",
                                     Build({Put("a", "1"), Del("b")}));
  SstableView v;
  ASSERT_TRUE(ReadSstableFile(path, &v).ok());
  std::string out;
  EXPECT_EQ(v.Get("a", &out), R::kFound);
  EXPECT_EQ(v.Get("b", &out), R::kDeleted);
}

TEST_F(SstableFileTest, MissingFileIsIOErrorNotNotFound) {
  SstableView v;
  Status s = ReadSstableFile(dir_ + "/nope.sst", &v);
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound()) << "a missing table file must not look like a missing key";
}

TEST_F(SstableFileTest, EmptyFileIsRejected) {
  const std::string path = WriteFile("sst-000001.sst", "");
  SstableView v;
  EXPECT_FALSE(ReadSstableFile(path, &v).ok());
}

TEST_F(SstableFileTest, DirectoryInsteadOfFileIsAnError) {
  fs::create_directories(dir_ + "/sst-000001.sst");
  SstableView v;
  EXPECT_FALSE(ReadSstableFile(dir_ + "/sst-000001.sst", &v).ok());
}

TEST_F(SstableFileTest, CorruptFileIsRejectedWithoutCrashing) {
  std::string bytes = Build({Put("a", "1")});
  bytes[3] = static_cast<char>(bytes[3] ^ 0x01);
  const std::string path = WriteFile("sst-000001.sst", bytes);
  SstableView v;
  EXPECT_FALSE(ReadSstableFile(path, &v).ok());
}

// ============================================================ bloom block

// The reader expects the bloom filter block at the offset named in the table,
// so the filter is built by the same code the flush uses.
std::string BuildWithBloom(const std::vector<SstEntry>& entries) {
  BloomFilter f(entries.size());
  for (const SstEntry& e : entries) f.add(e.key);
  std::string bits;
  f.serialize(bits);
  std::string out;
  EXPECT_TRUE(BuildSstable(entries, bits, &out).ok());
  return out;
}

TEST_F(SstableFileTest, BloomBlockReadsBackAndAnswersForAddedKeys) {
  const std::string path = WriteFile(
      "sst-000001.sst", BuildWithBloom({Put("a", "1"), Put("b", "2"), Put("c", "3")}));
  BloomFilterView view;
  ASSERT_TRUE(ReadBloomFilterBlock(path, &view).ok());
  BloomFilter f(view.num_bits_, view.num_hashes_, view.bits_);
  EXPECT_TRUE(f.may_contain("a"));
  EXPECT_TRUE(f.may_contain("b"));
  EXPECT_TRUE(f.may_contain("c"));
}

TEST_F(SstableFileTest, BloomReadOnTinyFileIsAnErrorNotACrash) {
  const std::string path = WriteFile("sst-000001.sst", "tiny");
  BloomFilterView view;
  EXPECT_FALSE(ReadBloomFilterBlock(path, &view).ok());
}

TEST_F(SstableFileTest, BloomReadOnMissingFileIsIOError) {
  BloomFilterView view;
  Status s = ReadBloomFilterBlock(dir_ + "/none.sst", &view);
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
}

TEST_F(SstableFileTest, CorruptBloomBitsAreDetected) {
  std::string bytes = BuildWithBloom({Put("a", "1"), Put("b", "2")});
  // The bloom block sits just before the 8-byte offset and 4-byte CRC that end
  // the table's footer region: flip a byte a little before those 12 bytes.
  bytes[bytes.size() - 40 - 2] = static_cast<char>(bytes[bytes.size() - 40 - 2] ^ 0x01);
  const std::string path = WriteFile("sst-000001.sst", bytes);
  BloomFilterView view;
  EXPECT_FALSE(ReadBloomFilterBlock(path, &view).ok());
}

TEST_F(SstableFileTest, BloomOffsetPastTheFooterIsRejectedNotACrash) {
  std::string bytes = BuildWithBloom({Put("a", "1")});
  // The 8 bytes at size-40 hold the bloom block's start offset. Point it at a
  // real position inside the file but after the bloom block, so the size
  // computation goes negative (an unsigned underflow) rather than failing in lseek.
  std::string bad;
  PutFixed64(&bad, bytes.size() - 20);
  bytes.replace(bytes.size() - 40, 8, bad);
  const std::string path = WriteFile("sst-000001.sst", bytes);
  BloomFilterView view;
  Status s = Status::OK();
  EXPECT_NO_THROW(s = ReadBloomFilterBlock(path, &view));
  EXPECT_FALSE(s.ok());
}

// ============================================== bloom block: round trip, damage

std::string BloomKey(int i) {
  char k[16];
  std::snprintf(k, sizeof(k), "key%05d", i);
  return k;
}

TEST_F(SstableFileTest, BloomBlockRoundTripKeepsEveryAnswer) {
  std::vector<SstEntry> entries;
  BloomFilter original(500);
  for (int i = 0; i < 500; ++i) {
    entries.push_back(Put(BloomKey(i), "v"));
    original.add(BloomKey(i));
  }
  const std::string path = WriteFile("sst-000001.sst", BuildWithBloom(entries));
  BloomFilterView view;
  ASSERT_TRUE(ReadBloomFilterBlock(path, &view).ok());
  BloomFilter copy(view.num_bits_, view.num_hashes_, view.bits_);
  for (int i = 0; i < 500; ++i) {
    EXPECT_TRUE(copy.may_contain(BloomKey(i))) << BloomKey(i);
  }
  for (int i = 0; i < 5000; ++i) {
    const std::string probe = "absent" + std::to_string(i);
    EXPECT_EQ(original.may_contain(probe), copy.may_contain(probe)) << probe;
  }
}

TEST_F(SstableFileTest, FlippingAnyBloomBitIsDetectedInFile) {
  const std::string bytes = BuildWithBloom({Put("a", "1"), Put("b", "2")});
  // The 8 bytes at size-40 hold the bloom block's offset; the bits run up to size-40.
  const size_t bloom_begin = static_cast<size_t>(GetFixed64(bytes.data() + bytes.size() - 40));
  const size_t bloom_end = bytes.size() - 40;
  ASSERT_LT(bloom_begin, bloom_end);
  for (size_t pos = bloom_begin; pos < bloom_end; ++pos) {
    std::string damaged = bytes;
    damaged[pos] = static_cast<char>(damaged[pos] ^ 0x40);
    const std::string path = WriteFile("sst-000001.sst", damaged);
    BloomFilterView view;
    EXPECT_FALSE(ReadBloomFilterBlock(path, &view).ok())
        << "undetected corruption in bloom bits at byte " << pos;
  }
}

TEST_F(SstableFileTest, TruncatedFileWithBloomBlockIsDetected) {
  const std::string bytes = BuildWithBloom({Put("a", "1"), Put("b", "2"), Put("c", "3")});
  for (size_t len : {bytes.size() - 1, bytes.size() / 2, size_t{40}, size_t{5}}) {
    const std::string path = WriteFile("sst-000001.sst", bytes.substr(0, len));
    BloomFilterView view;
    Status s = Status::OK();
    EXPECT_NO_THROW(s = ReadBloomFilterBlock(path, &view));
    EXPECT_FALSE(s.ok()) << "accepted truncation to " << len;
  }
}

}  // namespace
}  // namespace minilsm