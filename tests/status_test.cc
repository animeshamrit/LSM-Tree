#include <gtest/gtest.h>

#include "minilsm/db.h"  // included to prove it compiles
#include "minilsm/status.h"

using minilsm::Status;

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_FALSE(s.IsNotFound());
}

TEST(StatusTest, NotFoundIsNotOk) {
  Status s = Status::NotFound("missing key");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_EQ(s.message(), "missing key");
}

TEST(StatusTest, CorruptionIsDistinctFromNotFound) {
  Status s = Status::Corruption("bad checksum");
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_FALSE(s.IsNotFound());
}