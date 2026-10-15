#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "minilsm/memtable.h"
#include "minilsm/sstable_format.h"
#include "minilsm/sstable_reader.h"
#include "minilsm/sstable_writer.h"

namespace minilsm {
namespace {

// ---- Snapshot() on its own ----

TEST(MemTableSnapshotTest, EmptyMemTableGivesEmptySnapshot) {
  MemTable m;
  EXPECT_TRUE(m.Snapshot().empty());
}

TEST(MemTableSnapshotTest, EntriesComeOutSortedWhateverTheInsertOrder) {
  MemTable m;
  m.Put("cherry", "3");
  m.Put("apple", "1");
  m.Put("banana", "2");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 3u);
  EXPECT_EQ(snap[0].key, "apple");
  EXPECT_EQ(snap[1].key, "banana");
  EXPECT_EQ(snap[2].key, "cherry");
}

TEST(MemTableSnapshotTest, OverwrittenKeyAppearsOnceWithLatestValue) {
  MemTable m;
  m.Put("k", "old");
  m.Put("k", "new");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].value, "new");
  EXPECT_FALSE(snap[0].is_tombstone);
}

TEST(MemTableSnapshotTest, DeletedKeyIsATombstoneWithEmptyValue) {
  MemTable m;
  m.Put("k", "v");
  m.Delete("k");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_TRUE(snap[0].is_tombstone);
  EXPECT_EQ(snap[0].value, "");
}

// A delete of a key this MemTable never saw must still reach disk,
// because the key may live in an older SSTable.
TEST(MemTableSnapshotTest, DeleteOfUnknownKeyIsStillATombstone) {
  MemTable m;
  m.Delete("ghost");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].key, "ghost");
  EXPECT_TRUE(snap[0].is_tombstone);
}

TEST(MemTableSnapshotTest, OnlyTombstonesStillProducesEntries) {
  MemTable m;
  m.Delete("a");
  m.Delete("b");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 2u);
  EXPECT_TRUE(snap[0].is_tombstone);
  EXPECT_TRUE(snap[1].is_tombstone);
}

TEST(MemTableSnapshotTest, PutAfterDeleteIsALiveEntryAgain) {
  MemTable m;
  m.Put("k", "v1");
  m.Delete("k");
  m.Put("k", "v2");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_FALSE(snap[0].is_tombstone);
  EXPECT_EQ(snap[0].value, "v2");
}

// An empty value is a real value, not a tombstone.
TEST(MemTableSnapshotTest, EmptyValueIsNotATombstone) {
  MemTable m;
  m.Put("k", "");

  auto snap = m.Snapshot();
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_FALSE(snap[0].is_tombstone);
  EXPECT_EQ(snap[0].value, "");
}

// The snapshot is a copy: later writes must not change it.
TEST(MemTableSnapshotTest, SnapshotIsIndependentOfLaterWrites) {
  MemTable m;
  m.Put("a", "1");
  auto snap = m.Snapshot();

  m.Put("a", "changed");
  m.Put("b", "2");

  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].value, "1");
}

// Taking a snapshot is read-only: the MemTable must be unchanged.
TEST(MemTableSnapshotTest, SnapshotDoesNotModifyTheMemTable) {
  MemTable m;
  m.Put("a", "1");
  m.Delete("b");
  (void)m.Snapshot();
  (void)m.Snapshot();

  std::string v;
  EXPECT_EQ(m.Get("a", &v), MemTable::GetResult::kFound);
  EXPECT_EQ(v, "1");
  EXPECT_EQ(m.Get("b", &v), MemTable::GetResult::kDeleted);
  EXPECT_EQ(m.Snapshot().size(), 2u);
}

TEST(MemTableSnapshotTest, SnapshotCanBeBuiltIntoAnSstable) {
  MemTable m;
  for (char c = 'z'; c >= 'a'; --c) m.Put(std::string(1, c), "v");
  m.Delete("m");

  std::string bytes;
  std::string bloomfilter;
  EXPECT_TRUE(BuildSstable(m.Snapshot(), bloomfilter, &bytes).ok());
}

// ---- the whole flush path, through real files ----

class MemTableFlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / "minilsm_flush_test";
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  // snapshot -> build -> atomic write -> read back
  void Flush(const MemTable& m, const std::string& name, SstableView* view) {
    const std::string path = (dir_ / name).string();
    std::string bytes;
    std::string bloomfilter;
    ASSERT_TRUE(BuildSstable(m.Snapshot(), bloomfilter, &bytes).ok());
    std::unique_ptr<SstableWriter> w;
    ASSERT_TRUE(SstableWriter::Open(path, &w).ok());
    ASSERT_TRUE(w->WriteSstableFile(bytes).ok());
    ASSERT_TRUE(ReadSstableFile(path, view).ok());
  }

  std::filesystem::path dir_;
};

TEST_F(MemTableFlushTest, FlushedMemTableReadsBackFromDisk) {
  MemTable m;
  m.Put("apple", "red");
  m.Put("banana", "yellow");
  m.Put("cherry", "dark red");
  m.Delete("banana");
  m.Delete("never_written");

  SstableView view;
  Flush(m, "sst-000001.sst", &view);
  EXPECT_EQ(view.entry_count(), 4u);

  std::string v;
  EXPECT_EQ(view.Get("apple", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(v, "red");
  EXPECT_EQ(view.Get("banana", &v), SstableView::GetResult::kDeleted);
  EXPECT_EQ(view.Get("cherry", &v), SstableView::GetResult::kFound);
  EXPECT_EQ(view.Get("never_written", &v), SstableView::GetResult::kDeleted);
  EXPECT_EQ(view.Get("durian", &v), SstableView::GetResult::kNotPresent);
}

// Keys and values are arbitrary bytes, including NUL and 0xFF.
TEST_F(MemTableFlushTest, BinaryKeysAndValuesSurviveTheFlush) {
  const std::string key("k\0ey\xff", 5);
  const std::string value("va\0l\xff\x01", 6);
  MemTable m;
  m.Put(key, value);

  SstableView view;
  Flush(m, "bin.sst", &view);

  std::string v;
  EXPECT_EQ(view.Get(key, &v), SstableView::GetResult::kFound);
  EXPECT_EQ(v, value);
}

TEST_F(MemTableFlushTest, EveryKeyOfALargeMemTableReadsBack) {
  MemTable m;
  for (int i = 0; i < 5000; i++) m.Put("key" + std::to_string(i), std::to_string(i * 7));
  for (int i = 0; i < 5000; i += 5) m.Delete("key" + std::to_string(i));

  SstableView view;
  Flush(m, "large.sst", &view);
  EXPECT_EQ(view.entry_count(), 5000u);

  for (int i = 0; i < 5000; i++) {
    std::string v;
    auto r = view.Get("key" + std::to_string(i), &v);
    if (i % 5 == 0) {
      EXPECT_EQ(r, SstableView::GetResult::kDeleted) << i;
    } else {
      ASSERT_EQ(r, SstableView::GetResult::kFound) << i;
      EXPECT_EQ(v, std::to_string(i * 7)) << i;
    }
  }
}

}  // namespace
}  // namespace minilsm