#include <gtest/gtest.h>

#include <string>
#include <filesystem>

#include "minilsm/lsm_db.h"

using minilsm::LSMDB;

std::unique_ptr<LSMDB> OpenFresh(const std::string& name) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / ("lsmdb_" + name)).string();
  std::filesystem::remove_all(dir);
  std::unique_ptr<LSMDB> db;
  EXPECT_TRUE(LSMDB::Open(dir, /*sync=*/false, &db).ok());
  return db;
}

TEST(LSMDBTest, PutThenGet) {
  auto db = OpenFresh("put_then_get");
  ASSERT_TRUE(db->Put("a", "1").ok());
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ(v, "1");
}

TEST(LSMDBTest, MissingKeyIsNotFound) {
  LSMDB db;
  std::string v;
  EXPECT_TRUE(db.Get("nope", &v).IsNotFound());
}

TEST(LSMDBTest, DeleteMakesKeyNotFound) {
  auto db = OpenFresh("delete_makes_key_not_found");
  ASSERT_TRUE(db->Put("a", "1").ok());
  EXPECT_TRUE(db->Delete("a").ok());
  std::string v;
  EXPECT_TRUE(db->Get("a", &v).IsNotFound());
}

TEST(LSMDBTest, DeleteOfMissingKeyIsOk) {
  auto db = OpenFresh("delete_missing_key");
  EXPECT_TRUE(db->Delete("ghost").ok());
}

TEST(LSMDBTest, WorksThroughBasePointer) {
  auto real = OpenFresh("base_pointer");
  minilsm::DB* db = real.get();   // code written against the interface
  ASSERT_TRUE(db->Put("k", "v").ok());
  std::string v;
  EXPECT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "v");
}