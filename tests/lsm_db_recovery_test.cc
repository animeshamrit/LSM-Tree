#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "minilsm/lsm_db.h"

using minilsm::LSMDB;

class RecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/minilsm_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir_ = tmpl;
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::unique_ptr<LSMDB> OpenDb() {
    std::unique_ptr<LSMDB> db;
    EXPECT_TRUE(LSMDB::Open(dir_, /*sync=*/false, &db).ok());
    return db;
  }

  std::string dir_;
};

TEST_F(RecoveryTest, EmptyDirectoryOpensEmpty) {
  auto db = OpenDb();
  std::string v;
  EXPECT_TRUE(db->Get("anything", &v).IsNotFound());
  EXPECT_EQ(db->recovery_dropped_bytes(), 0u);
}

TEST_F(RecoveryTest, WritesSurviveRestart) {
  {
    auto db = OpenDb();
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());
  }  // destroyed: simulates the process ending

  auto db = OpenDb();
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ(v, "1");
  ASSERT_TRUE(db->Get("b", &v).ok());
  EXPECT_EQ(v, "2");
}

TEST_F(RecoveryTest, DeleteSurvivesRestart) {
  {
    auto db = OpenDb();
    db->Put("a", "1");
    db->Delete("a");
  }
  auto db = OpenDb();
  std::string v;
  EXPECT_TRUE(db->Get("a", &v).IsNotFound());
}

TEST_F(RecoveryTest, ReplayKeepsOrderSoLastWriteWins) {
  {
    auto db = OpenDb();
    db->Put("x", "1");
    db->Put("x", "2");
    db->Put("x", "3");
  }
  auto db = OpenDb();
  std::string v;
  ASSERT_TRUE(db->Get("x", &v).ok());
  EXPECT_EQ(v, "3");
}

TEST_F(RecoveryTest, ReopenTwiceDoesNotDuplicateOrLoseData) {
  {
    auto db = OpenDb();
    db->Put("a", "1");
  }
  { auto db = OpenDb(); }  // open and close without writing
  auto db = OpenDb();
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  EXPECT_EQ(v, "1");
  // The log should hold exactly one record: replay must not re-append.
  auto size_after = std::filesystem::file_size(dir_ + "/wal-000001.log");
  { auto again = OpenDb(); }
  EXPECT_EQ(std::filesystem::file_size(dir_ + "/wal-000001.log"), size_after);
}

TEST_F(RecoveryTest, TornTailIsDroppedAndNewWritesStillWork) {
  {
    auto db = OpenDb();
    db->Put("a", "1");
    db->Put("b", "2");
  }
  // Simulate a crash mid-append: chop 3 bytes off the last record.
  const std::string wal = dir_ + "/wal-000001.log";
  std::filesystem::resize_file(wal, std::filesystem::file_size(wal) - 3);

  {
    auto db = OpenDb();
    std::string v;
    ASSERT_TRUE(db->Get("a", &v).ok());          // good prefix survives
    EXPECT_TRUE(db->Get("b", &v).IsNotFound());  // torn record is gone
    EXPECT_GT(db->recovery_dropped_bytes(), 0u); // and we can see it
    ASSERT_TRUE(db->Put("c", "3").ok());         // a write AFTER recovery
  }

  // The write after recovery must itself survive another restart.
  auto db = OpenDb();
  std::string v;
  ASSERT_TRUE(db->Get("a", &v).ok());
  ASSERT_TRUE(db->Get("c", &v).ok());
  EXPECT_EQ(v, "3");
  EXPECT_TRUE(db->Get("b", &v).IsNotFound());
  EXPECT_EQ(db->recovery_dropped_bytes(), 0u);
}