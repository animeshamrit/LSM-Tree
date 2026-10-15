#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "minilsm/sstable_format.h"

using minilsm::BuildSstable;
using minilsm::SstableView;
using minilsm::SstEntry;
using Result = minilsm::SstableView::GetResult;

namespace {

SstEntry Put(const std::string& k, const std::string& v) {
  SstEntry e;
  e.key = k;
  e.value = v;
  return e;
}

SstEntry Del(const std::string& k) {
  SstEntry e;
  e.is_tombstone = true;
  e.key = k;
  return e;
}

SstableView Build(const std::vector<SstEntry>& entries, std::string* raw = nullptr) {
  std::string bytes;
  std::string bloomfilter;
  EXPECT_TRUE(BuildSstable(entries, bloomfilter, &bytes).ok());
  if (raw) *raw = bytes;
  SstableView view;
  EXPECT_TRUE(SstableView::Parse(bytes, &view).ok());
  return view;
}

}  // namespace

TEST(SstableFormatTest, FindsEveryKey) {
  SstableView v = Build({Put("a", "1"), Put("b", "2"), Put("c", "3")});
  std::string out;
  EXPECT_EQ(v.Get("a", &out), Result::kFound);
  EXPECT_EQ(out, "1");
  EXPECT_EQ(v.Get("b", &out), Result::kFound);
  EXPECT_EQ(out, "2");
  EXPECT_EQ(v.Get("c", &out), Result::kFound);
  EXPECT_EQ(out, "3");
}

TEST(SstableFormatTest, MissingKeyIsNotPresent) {
  SstableView v = Build({Put("b", "2"), Put("d", "4")});
  std::string out;
  EXPECT_EQ(v.Get("a", &out), Result::kNotPresent);  // before first
  EXPECT_EQ(v.Get("c", &out), Result::kNotPresent);  // between
  EXPECT_EQ(v.Get("z", &out), Result::kNotPresent);  // after last
  EXPECT_EQ(v.Get("b", &out), Result::kFound);
  EXPECT_EQ(out, "2");
}

TEST(SstableFormatTest, TombstoneIsDeletedNotNotPresent) {
  SstableView v = Build({Put("a", "1"), Del("b")});
  std::string out;
  EXPECT_EQ(v.Get("b", &out), Result::kDeleted);
}

TEST(SstableFormatTest, EmptyValueIsFound) {
  SstableView v = Build({Put("a", "")});
  std::string out = "garbage";
  EXPECT_EQ(v.Get("a", &out), Result::kFound);
  EXPECT_EQ(out, "");
}

TEST(SstableFormatTest, EmptyTableIsValid) {
  SstableView v = Build({});
  std::string out;
  EXPECT_EQ(v.entry_count(), 0u);
  EXPECT_EQ(v.Get("a", &out), Result::kNotPresent);
}

TEST(SstableFormatTest, UnsortedInputIsRejected) {
  std::string bytes;
  std::string bloomfilter;
  EXPECT_TRUE(
      BuildSstable({Put("b", "1"), Put("a", "2")}, bloomfilter, &bytes).IsInvalidArgument());
}

TEST(SstableFormatTest, DuplicateKeysAreRejected) {
  std::string bytes;
  std::string bloomfilter;
  EXPECT_TRUE(
      BuildSstable({Put("a", "1"), Put("a", "2")},bloomfilter, &bytes).IsInvalidArgument());
}

TEST(SstableFormatTest, FlippedByteIsCorruption) {
  std::string raw;
  Build({Put("a", "1"), Put("b", "2")}, &raw);
  raw[2] ^= 0x01;
  SstableView view;
  EXPECT_TRUE(SstableView::Parse(raw, &view).IsCorruption());
}

TEST(SstableFormatTest, WrongMagicIsCorruption) {
  std::string raw;
  Build({Put("a", "1")}, &raw);
  raw[raw.size() - 1] ^= 0x01;  // last byte belongs to the magic number
  SstableView view;
  EXPECT_TRUE(SstableView::Parse(raw, &view).IsCorruption());
}

TEST(SstableFormatTest, TruncatedFileIsCorruption) {
  std::string raw;
  Build({Put("a", "1")}, &raw);
  raw.resize(raw.size() - 5);
  SstableView view;
  EXPECT_TRUE(SstableView::Parse(raw, &view).IsCorruption());
  EXPECT_TRUE(SstableView::Parse("tiny", &view).IsCorruption());
}