#include <gtest/gtest.h>

#include <string>

#include "minilsm/lsm_db.h"

using minilsm::LSMDB;

TEST(LSMDBTest, PutThenGet) {
  LSMDB db;
  ASSERT_TRUE(db.Put("a", "1").ok());
  std::string v;
  ASSERT_TRUE(db.Get("a", &v).ok());
  EXPECT_EQ(v, "1");
}

TEST(LSMDBTest, MissingKeyIsNotFound) {
  LSMDB db;
  std::string v;
  EXPECT_TRUE(db.Get("nope", &v).IsNotFound());
}

TEST(LSMDBTest, DeleteMakesKeyNotFound) {
  LSMDB db;
  db.Put("a", "1");
  EXPECT_TRUE(db.Delete("a").ok());
  std::string v;
  EXPECT_TRUE(db.Get("a", &v).IsNotFound());
}

TEST(LSMDBTest, DeleteOfMissingKeyIsOk) {
  LSMDB db;
  EXPECT_TRUE(db.Delete("ghost").ok());
}

TEST(LSMDBTest, WorksThroughBasePointer) {
  LSMDB real;
  minilsm::DB* db = &real;   // code written against the interface
  db->Put("k", "v");
  std::string v;
  EXPECT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "v");
}