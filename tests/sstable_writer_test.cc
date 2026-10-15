#include "minilsm/sstable_writer.h"

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace minilsm {
namespace {

namespace fs = std::filesystem;

class SstableWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "minilsm_sstwriter_test";
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    fs::permissions(dir_, fs::perms::owner_all);  // undo read-only test
    fs::remove_all(dir_);
  }

  std::string Path(const std::string& n) const { return (dir_ / n).string(); }

  static std::string ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  static void WriteFile(const std::string& p, const std::string& d) {
    std::ofstream(p, std::ios::binary) << d;
  }

  fs::path dir_;
};

// ---- the happy path ------------------------------------------------------

TEST_F(SstableWriterTest, PublishesExactBytes) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile("hello world").ok());

  EXPECT_EQ(ReadFile(Path("a.sst")), "hello world");
}

TEST_F(SstableWriterTest, NoTempFileAfterSuccess) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile("x").ok());

  EXPECT_TRUE(fs::exists(Path("a.sst")));
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
}

TEST_F(SstableWriterTest, EmptyBufferStillPublishesAnEmptyFile) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile("").ok());

  EXPECT_TRUE(fs::exists(Path("a.sst")));
  EXPECT_EQ(fs::file_size(Path("a.sst")), 0u);
}

TEST_F(SstableWriterTest, LargeWriteRoundTrips) {
  std::string big(5 * 1024 * 1024, 'q');
  for (size_t i = 0; i < big.size(); i += 4099) big[i] = static_cast<char>(i);

  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("big.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile(big).ok());

  EXPECT_EQ(ReadFile(Path("big.sst")), big);
}

TEST_F(SstableWriterTest, ReplacesAnExistingFile) {
  WriteFile(Path("a.sst"), "old contents");

  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  // Opening must not touch the old file; only a completed write replaces it.
  EXPECT_EQ(ReadFile(Path("a.sst")), "old contents");
  ASSERT_TRUE(w->WriteSstableFile("new").ok());

  EXPECT_EQ(ReadFile(Path("a.sst")), "new");
}

// ---- the atomicity guarantee --------------------------------------------

TEST_F(SstableWriterTest, RealNameDoesNotExistUntilTheWriteCompletes) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());

  EXPECT_FALSE(fs::exists(Path("a.sst")));
  EXPECT_TRUE(fs::exists(Path("a.sst.tmp")));
}

TEST_F(SstableWriterTest, AbandonedWriterLeavesNothingBehind) {
  {
    std::unique_ptr<SstableWriter> w;
    ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  }  // destroyed without ever writing

  EXPECT_FALSE(fs::exists(Path("a.sst")));
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
}

TEST_F(SstableWriterTest, ExplicitCloseWithoutWriteLeavesNothingBehind) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  EXPECT_TRUE(w->Close().ok());

  EXPECT_FALSE(fs::exists(Path("a.sst")));
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
}

TEST_F(SstableWriterTest, DestructorAfterSuccessKeepsTheFile) {
  {
    std::unique_ptr<SstableWriter> w;
    ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
    ASSERT_TRUE(w->WriteSstableFile("keep me").ok());
  }
  EXPECT_EQ(ReadFile(Path("a.sst")), "keep me");
}

TEST_F(SstableWriterTest, CloseAfterSuccessKeepsTheFile) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile("keep me").ok());
  EXPECT_TRUE(w->Close().ok());

  EXPECT_EQ(ReadFile(Path("a.sst")), "keep me");
}

// A crash can leave an old .tmp behind. Starting over must not extend it.
TEST_F(SstableWriterTest, StaleTempFileIsReplacedNotExtended) {
  WriteFile(Path("a.sst.tmp"), "STALE GARBAGE");

  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile("fresh").ok());

  EXPECT_EQ(ReadFile(Path("a.sst")), "fresh");
}

// ---- misuse --------------------------------------------------------------

// A second call must be rejected, and must not touch the published file.
TEST_F(SstableWriterTest, SecondWriteIsRejectedAndFileIsUnharmed) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->WriteSstableFile("first").ok());

  EXPECT_FALSE(w->WriteSstableFile("second").ok());
  EXPECT_EQ(ReadFile(Path("a.sst")), "first");
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
}

TEST_F(SstableWriterTest, WriteAfterCloseIsRejected) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  ASSERT_TRUE(w->Close().ok());

  EXPECT_FALSE(w->WriteSstableFile("x").ok());
  EXPECT_FALSE(fs::exists(Path("a.sst")));
}

TEST_F(SstableWriterTest, CloseTwiceIsHarmless) {
  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
  EXPECT_TRUE(w->Close().ok());
  EXPECT_TRUE(w->Close().ok());
}

// ---- failures ------------------------------------------------------------

TEST_F(SstableWriterTest, OpenInMissingDirectoryFails) {
  std::unique_ptr<SstableWriter> w;
  EXPECT_FALSE(SstableWriter::Open(Path("no_such_dir/a.sst"), &w).ok());
  EXPECT_EQ(w, nullptr);
  EXPECT_FALSE(fs::exists(Path("no_such_dir")));
}

// rename() fails when the destination is a non-empty directory.
TEST_F(SstableWriterTest, RenameFailureIsReportedAndTempIsRemoved) {
  fs::create_directories(dir_ / "a.sst");
  WriteFile(Path("a.sst/child"), "x");

  std::unique_ptr<SstableWriter> w;
  ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());

  EXPECT_FALSE(w->WriteSstableFile("data").ok());
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
  EXPECT_TRUE(fs::is_directory(Path("a.sst")));  // destination untouched
}

// Force write() to fail with EFBIG by capping the maximum file size.
TEST_F(SstableWriterTest, WriteFailureIsReportedAndNothingIsPublished) {
  struct rlimit old_limit;
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &old_limit), 0);
  auto old_handler = std::signal(SIGXFSZ, SIG_IGN);  // get EFBIG, not a signal

  struct rlimit small = old_limit;
  small.rlim_cur = 4096;
  ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &small), 0);

  {
    std::unique_ptr<SstableWriter> w;
    ASSERT_TRUE(SstableWriter::Open(Path("a.sst"), &w).ok());
    EXPECT_FALSE(w->WriteSstableFile(std::string(1024 * 1024, 'z')).ok());
  }  // writer destroyed here

  ::setrlimit(RLIMIT_FSIZE, &old_limit);
  std::signal(SIGXFSZ, old_handler);

  EXPECT_FALSE(fs::exists(Path("a.sst")));
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
}

// Skipped as root, because root ignores directory permissions.
TEST_F(SstableWriterTest, ReadOnlyDirectoryFailsToOpen) {
  if (::geteuid() == 0) GTEST_SKIP() << "root bypasses permissions";
  fs::permissions(dir_, fs::perms::owner_read | fs::perms::owner_exec);

  std::unique_ptr<SstableWriter> w;
  EXPECT_FALSE(SstableWriter::Open(Path("a.sst"), &w).ok());
  EXPECT_FALSE(fs::exists(Path("a.sst.tmp")));
}

}  // namespace
}  // namespace minilsm