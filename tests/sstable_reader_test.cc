#include "minilsm/sstable_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "minilsm/sstable_writer.h"

namespace minilsm {
namespace {

namespace fs = std::filesystem;

class SstableReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "minilsm_sstreader_test";
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  std::string Path(const std::string& n) const { return (dir_ / n).string(); }

  // Builds an SSTable and publishes it with the writer from 12a.
  void WriteSst(const std::string& path, const std::vector<SstEntry>& entries) {
    std::string bytes;
    std::string bloomfilter;
    ASSERT_TRUE(BuildSstable(entries, bloomfilter, &bytes).ok());
    std::unique_ptr<SstableWriter> w;
    ASSERT_TRUE(SstableWriter::Open(path, &w).ok());
    ASSERT_TRUE(w->WriteSstableFile(bytes).ok());
  }

  static std::string ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  static void WriteFile(const std::string& p, const std::string& d) {
    std::ofstream(p, std::ios::binary) << d;
  }

  static std::vector<SstEntry> Sample() {
    return {{false, "apple", "red"},
            {true, "banana", ""},
            {false, "cherry", "dark red"}};
  }

  fs::path dir_;
};

// ---- the happy path ----

TEST_F(SstableReaderTest, RoundTrip) {
  WriteSst(Path("a.sst"), Sample());

  SstableView view;
  ASSERT_TRUE(ReadSstableFile(Path("a.sst"), &view).ok());
  EXPECT_EQ(view.entry_count(), 3u);

  std::string v;
  EXPECT_EQ(view.Get("apple", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(v, "red");
  EXPECT_EQ(view.Get("banana", &v), SstableView::GetResult::kDeleted);
  EXPECT_EQ(view.Get("cherry", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(v, "dark red");
  EXPECT_EQ(view.Get("durian", &v), SstableView::GetResult::kNotPresent);
}

// Bigger than any single read() would return, so the loop must advance
// the offset correctly across several calls.
TEST_F(SstableReaderTest, LargeFileRoundTrips) {
  std::vector<SstEntry> entries;
  for (int i = 0; i < 20000; i++) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%08d", i);
    entries.push_back({false, key, std::string(50, 'v')});
  }
  WriteSst(Path("big.sst"), entries);
  ASSERT_GT(fs::file_size(Path("big.sst")), 1024u * 1024u);

  SstableView view;
  ASSERT_TRUE(ReadSstableFile(Path("big.sst"), &view).ok());
  EXPECT_EQ(view.entry_count(), 20000u);

  std::string v;
  EXPECT_EQ(view.Get("key00000000", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(view.Get("key00010000", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(view.Get("key00019999", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(v, std::string(50, 'v'));
  EXPECT_EQ(view.Get("key00020000", &v), SstableView::GetResult::kNotPresent);
}

TEST_F(SstableReaderTest, EmptySstableRoundTrips) {
  WriteSst(Path("empty_table.sst"), {});

  SstableView view;
  ASSERT_TRUE(ReadSstableFile(Path("empty_table.sst"), &view).ok());
  EXPECT_EQ(view.entry_count(), 0u);
  std::string v;
  EXPECT_EQ(view.Get("anything", &v), SstableView::GetResult::kNotPresent);
}

// ---- the file could not be read at all ----

TEST_F(SstableReaderTest, MissingFileIsAnError) {
  SstableView view;
  EXPECT_FALSE(ReadSstableFile(Path("nope.sst"), &view).ok());
}

// open() succeeds on a directory, but read() fails (EISDIR).
// This must be an error, not a hang or a crash.
TEST_F(SstableReaderTest, DirectoryInsteadOfFileIsAnError) {
  fs::create_directories(dir_ / "adir.sst");
  SstableView view;
  EXPECT_FALSE(ReadSstableFile(Path("adir.sst"), &view).ok());
}

// ---- the file was read, but it is not a valid SSTable ----

TEST_F(SstableReaderTest, EmptyFileIsRejected) {
  WriteFile(Path("empty.sst"), "");
  SstableView view;
  EXPECT_FALSE(ReadSstableFile(Path("empty.sst"), &view).ok());
}

TEST_F(SstableReaderTest, GarbageFileIsRejected) {
  WriteFile(Path("junk.sst"), std::string(500, 'x'));
  SstableView view;
  EXPECT_FALSE(ReadSstableFile(Path("junk.sst"), &view).ok());
}

TEST_F(SstableReaderTest, TruncatedFileIsRejected) {
  WriteSst(Path("a.sst"), Sample());
  const std::string bytes = ReadFile(Path("a.sst"));

  for (size_t cut : {size_t{1}, size_t{10}, bytes.size() / 2, bytes.size() - 1}) {
    WriteFile(Path("t.sst"), bytes.substr(0, bytes.size() - cut));
    SstableView view;
    EXPECT_FALSE(ReadSstableFile(Path("t.sst"), &view).ok())
        << "cut " << cut << " bytes";
  }
}

TEST_F(SstableReaderTest, FlippedByteInDataIsRejected) {
  WriteSst(Path("a.sst"), Sample());
  std::string bytes = ReadFile(Path("a.sst"));
  bytes[2] = static_cast<char>(bytes[2] ^ 0xFF);  // inside the first entry
  WriteFile(Path("bad.sst"), bytes);

  SstableView view;
  EXPECT_FALSE(ReadSstableFile(Path("bad.sst"), &view).ok());
}

TEST_F(SstableReaderTest, FlippedByteInMagicIsRejected) {
  WriteSst(Path("a.sst"), Sample());
  std::string bytes = ReadFile(Path("a.sst"));
  bytes.back() = static_cast<char>(bytes.back() ^ 0xFF);  // last byte = magic
  WriteFile(Path("bad.sst"), bytes);

  SstableView view;
  EXPECT_FALSE(ReadSstableFile(Path("bad.sst"), &view).ok());
}

// Flip every byte, one at a time. Each corruption must be caught.
TEST_F(SstableReaderTest, AnySingleByteCorruptionIsDetected) {
  WriteSst(Path("a.sst"), Sample());
  const std::string good = ReadFile(Path("a.sst"));

  for (size_t i = 0; i < good.size(); i++) {
    std::string bad = good;
    bad[i] = static_cast<char>(bad[i] ^ 0x01);
    WriteFile(Path("bad.sst"), bad);
    SstableView view;
    EXPECT_FALSE(ReadSstableFile(Path("bad.sst"), &view).ok())
        << "flipping byte " << i << " went undetected";
  }
}

// A failed read must not leave a half-filled view behind that looks usable.
TEST_F(SstableReaderTest, FailedReadLeavesViewEmpty) {
  SstableView view;
  ASSERT_FALSE(ReadSstableFile(Path("nope.sst"), &view).ok());
  EXPECT_EQ(view.entry_count(), 0u);
}

}  // namespace
}  // namespace minilsm