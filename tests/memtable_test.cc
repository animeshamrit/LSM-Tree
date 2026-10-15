#include <gtest/gtest.h>

#include <string>

#include "minilsm/memtable.h"

using minilsm::MemTable;
using Result = minilsm::MemTable::GetResult;

TEST(MemTableTest, PutThenGet) {
  MemTable m;
  m.Put("a", "1");
  std::string v;
  EXPECT_EQ(m.Get("a", &v), Result::kFound);
  EXPECT_EQ(v, "1");
}

TEST(MemTableTest, MissingKeyIsNotPresent) {
  MemTable m;
  std::string v;
  EXPECT_EQ(m.Get("nope", &v), Result::kNotPresent);
}

TEST(MemTableTest, OverwriteReturnsLatestValue) {
  MemTable m;
  m.Put("a", "1");
  m.Put("a", "2");
  std::string v;
  EXPECT_EQ(m.Get("a", &v), Result::kFound);
  EXPECT_EQ(v, "2");
}

TEST(MemTableTest, DeleteLeavesTombstone) {
  MemTable m;
  m.Put("a", "1");
  m.Delete("a");
  std::string v;
  EXPECT_EQ(m.Get("a", &v), Result::kDeleted);
}

TEST(MemTableTest, DeleteOfUnknownKeyStillRecordsTombstone) {
  MemTable m;
  m.Delete("ghost");
  std::string v;
  // We never saw "ghost", but an older SSTable might have it,
  // so the tombstone must exist.
  EXPECT_EQ(m.Get("ghost", &v), Result::kDeleted);
}

TEST(MemTableTest, PutAfterDeleteBringsKeyBack) {
  MemTable m;
  m.Put("a", "1");
  m.Delete("a");
  m.Put("a", "3");
  std::string v;
  EXPECT_EQ(m.Get("a", &v), Result::kFound);
  EXPECT_EQ(v, "3");
}

TEST(MemTableTest, EmptyValueIsStillFound) {
  MemTable m;
  m.Put("a", "");
  std::string v = "garbage";
  EXPECT_EQ(m.Get("a", &v), Result::kFound);
  EXPECT_EQ(v, "");
}