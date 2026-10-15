// Brick 12e: tests for the asynchronous flush.
//
// FlushMemTableToSstable() returns as soon as the work is queued; a background
// thread writes the SSTable and deletes the old WAL later. These tests never
// assume a flush has finished unless they have waited for it, in one of two ways:
//
//   * Close()  destroys the database. The destructor joins the worker after it
//              has drained the queue, so afterwards every accepted flush is on
//              disk and the directory is in its final state.
//   * WaitUntil(...)  polls (up to ~5 s) while the database stays open. Used
//              when the database must NOT be reopened yet (failure tests).
//
// No source changes are needed. Failure injection: a non-empty directory named
// like the file the database wants to create. open() on it fails with EISDIR,
// and it survives both unlink() and std::filesystem::remove().
//
// File naming assumed: sst-%06llu.sst, sst-%06llu.sst.tmp, wal-%06llu.log.
#include "minilsm/lsm_db.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace minilsm {
namespace {

namespace fs = std::filesystem;

// Polls until ok() is true; gives up after about 5 seconds.
template <typename Pred>
bool WaitUntil(Pred ok) {
  for (int i = 0; i < 500; ++i) {
    if (ok()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

using Numbers = std::vector<uint64_t>;

class AsyncFlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = (fs::temp_directory_path() /
            ("minilsm_async_" + std::to_string(::getpid()) + "_" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name()))
               .string();
    ResetDir();
  }
  void TearDown() override {
    db_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // ---- directory and database lifecycle
  void ResetDir() {
    db_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
  }
  void Open(bool sync = true) {
    db_.reset();
    ASSERT_TRUE(LSMDB::Open(dir_, sync, &db_).ok());
  }
  void Close() { db_.reset(); }  // joins the worker after it drains the queue

  // ---- what is on disk
  std::string SstP(uint64_t n) const { return dir_ + "/" + Name("sst-", n, ".sst"); }
  std::string WalP(uint64_t n) const { return dir_ + "/" + Name("wal-", n, ".log"); }
  static std::string Name(const char* pre, uint64_t n, const char* suf) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s%06llu%s", pre, (unsigned long long)n, suf);
    return buf;
  }
  Numbers Scan(const std::string& pre, const std::string& suf) const {
    Numbers out;
    for (const auto& e : fs::directory_iterator(dir_)) {
      if (!e.is_regular_file()) continue;
      const std::string name = e.path().filename().string();
      if (name.size() <= pre.size() + suf.size()) continue;
      if (name.compare(0, pre.size(), pre) != 0) continue;
      if (name.compare(name.size() - suf.size(), suf.size(), suf) != 0) continue;
      const std::string d = name.substr(pre.size(), name.size() - pre.size() - suf.size());
      if (d.find_first_not_of("0123456789") != std::string::npos) continue;
      out.push_back(std::stoull(d));
    }
    std::sort(out.begin(), out.end());
    return out;
  }
  Numbers Ssts() const { return Scan("sst-", ".sst"); }
  Numbers Wals() const { return Scan("wal-", ".log"); }

  static Numbers Range(uint64_t first, uint64_t last) {  // inclusive
    Numbers v;
    for (uint64_t i = first; i <= last; ++i) v.push_back(i);
    return v;
  }

  // ---- fault injection
  void Block(const std::string& path) {
    fs::create_directory(path);
    std::ofstream(path + "/x") << "x";
  }
  void Unblock(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  // ---- reads
  void Expect(const std::string& k, const std::string& v) {
    std::string out;
    Status s = db_->Get(k, &out);
    EXPECT_TRUE(s.ok()) << "key " << k << ": " << s.message();
    EXPECT_TRUE(out == v) << "key " << k << ": got " << out.size()
                          << " bytes, want " << v.size();
  }
  void ExpectMissing(const std::string& k) {
    std::string out;
    Status s = db_->Get(k, &out);
    EXPECT_TRUE(s.IsNotFound()) << "status: " << s.message() << " key: " << k;
    //EXPECT_TRUE(db_->Get(k, &out).IsNotFound()) << "key " << k;
  }
  static std::string Key(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%05d", i);
    return buf;
  }

  // ---- flushing
  void Flush() { ASSERT_TRUE(db_->FlushMemTableToSstable().ok()); }

  // A halted database fails every call with something other than NotFound.
  bool IsHalted() {
    std::string v;
    Status s = db_->Get("probe", &v);
    return !s.ok() && !s.IsNotFound();
  }
  // Failure path: the flush is accepted, the task fails later and halts the DB.
  void FlushAndWaitForHalt() {
    ASSERT_TRUE(db_->FlushMemTableToSstable().ok());
    ASSERT_TRUE(WaitUntil([&] { return IsHalted(); }));
  }

  std::string dir_;
  std::unique_ptr<LSMDB> db_;
};

// =============================================================== basic async

TEST_F(AsyncFlushTest, FlushedDataIsReadableImmediately) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  Expect("a", "1");  // no waiting: the in-memory view serves reads at once
  Expect("b", "2");
  ExpectMissing("c");
}

TEST_F(AsyncFlushTest, FlushThenCloseWritesTheSstableAndRotatesTheWal) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  Close();                                   // drains the background task
  EXPECT_EQ(Ssts(), Numbers{1});
  EXPECT_EQ(Wals(), Numbers{2});             // wal-1 deleted, wal-2 is live
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
}

TEST_F(AsyncFlushTest, FlushOfAnEmptyMemTableDoesNothing) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(Flush());
  Close();
  EXPECT_TRUE(Ssts().empty());
  EXPECT_EQ(Wals(), Numbers{1});             // no rotation either
}

TEST_F(AsyncFlushTest, WritesAfterAFlushAreVisibleAndNewestWins) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "1").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  ASSERT_TRUE(db_->Put("a", "2").ok());      // shadows the flushed value
  ASSERT_TRUE(db_->Delete("b").ok());        // tombstone shadows the flushed value
  Expect("a", "2");
  ExpectMissing("b");
  Close();
  ASSERT_NO_FATAL_FAILURE(Open());           // same answers after a restart
  Expect("a", "2");
  ExpectMissing("b");
}

TEST_F(AsyncFlushTest, TombstoneInALaterFlushHidesAnEarlierValue) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("k", "v").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  ASSERT_TRUE(db_->Delete("k").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  ExpectMissing("k");
  Close();
  EXPECT_EQ(Ssts(), (Numbers{1, 2}));
  ASSERT_NO_FATAL_FAILURE(Open());
  ExpectMissing("k");                        // the tombstone is data on disk
}

TEST_F(AsyncFlushTest, NewestSstableWinsAfterRestart) {
  ASSERT_NO_FATAL_FAILURE(Open());
  for (const char* v : {"1", "2", "3"}) {
    ASSERT_TRUE(db_->Put("k", v).ok());
    ASSERT_NO_FATAL_FAILURE(Flush());
  }
  Close();
  EXPECT_EQ(Ssts(), (Numbers{1, 2, 3}));
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("k", "3");
}

TEST_F(AsyncFlushTest, SstableNumberingContinuesAcrossReopen) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  Close();
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  Close();
  EXPECT_EQ(Ssts(), (Numbers{1, 2}));        // not overwritten, not skipped
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
  Expect("b", "2");
}

TEST_F(AsyncFlushTest, UnflushedWritesSurviveARestart) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Delete("zzz").ok());
  Close();
  EXPECT_TRUE(Ssts().empty());
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
}

TEST_F(AsyncFlushTest, ReplayedDataCanBeFlushed) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  Close();
  ASSERT_NO_FATAL_FAILURE(Open());           // the WAL is replayed into the MemTable
  ASSERT_NO_FATAL_FAILURE(Flush());
  Close();
  EXPECT_EQ(Ssts(), Numbers{1});
  EXPECT_EQ(Wals(), Numbers{2});
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
  Expect("b", "2");
}

// ============================================================ many in flight

// Flush() returns before the worker is done, so these queue up. Destroying the
// database must still finish every one of them.
TEST_F(AsyncFlushTest, DestroyingTheDatabaseFinishesAllQueuedFlushes) {
  constexpr int kFlushes = 50;
  ASSERT_NO_FATAL_FAILURE(Open());
  for (int i = 1; i <= kFlushes; ++i) {
    ASSERT_TRUE(db_->Put(Key(i), "v" + std::to_string(i)).ok());
    ASSERT_NO_FATAL_FAILURE(Flush());
  }
  Close();
  EXPECT_EQ(Ssts(), Range(1, kFlushes));
  EXPECT_EQ(Wals(), Numbers{kFlushes + 1});  // every older WAL was cleaned up
  ASSERT_NO_FATAL_FAILURE(Open());
  for (int i = 1; i <= kFlushes; ++i) Expect(Key(i), "v" + std::to_string(i));
}

// ============================================================== size trigger

TEST_F(AsyncFlushTest, SizeTriggerFlushesInTheBackgroundAndKeepsEveryKey) {
  ASSERT_NO_FATAL_FAILURE(Open(/*sync=*/false));
  const std::string big(100 * 1024, 'x');
  const int kCount = static_cast<int>(LSMDB::kmaximum_bytes_ / big.size()) + 20;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(db_->Put(Key(i), big + std::to_string(i)).ok()) << "put " << i;
  }
  Close();
  EXPECT_GE(Ssts().size(), 1u);
  ASSERT_NO_FATAL_FAILURE(Open(/*sync=*/false));
  for (int i = 0; i < kCount; ++i) Expect(Key(i), big + std::to_string(i));
}

TEST_F(AsyncFlushTest, EntryLargerThanTheLimitIsRejectedAndNotLogged) {
  ASSERT_NO_FATAL_FAILURE(Open());
  EXPECT_FALSE(db_->Put("k", std::string(LSMDB::kmaximum_bytes_, 'x')).ok());
  EXPECT_FALSE(db_->Delete(std::string(LSMDB::kmaximum_bytes_, 'k')).ok());
  ASSERT_TRUE(db_->Put("a", "1").ok());      // not halted, still usable
  Close();
  ASSERT_NO_FATAL_FAILURE(Open());
  ExpectMissing("k");
  Expect("a", "1");
}

// ============================================================== failed flush

TEST_F(AsyncFlushTest, FailedFlushHaltsTheDatabaseAndRefusesEverything) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForHalt());
  EXPECT_FALSE(db_->Put("b", "2").ok());
  EXPECT_FALSE(db_->Delete("a").ok());
  EXPECT_FALSE(db_->FlushMemTableToSstable().ok());
  std::string v;
  Status s = db_->Get("a", &v);
  EXPECT_TRUE(!s.ok() && !s.IsNotFound());   // an error, not a quiet "missing"
}

TEST_F(AsyncFlushTest, FailedFlushKeepsEveryWalAndReopenRecoversTheData) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  Block(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForHalt());
  EXPECT_FALSE(db_->Put("c", "3").ok());     // refused, so it must not be logged
  Close();
  EXPECT_TRUE(Ssts().empty());
  EXPECT_EQ(Wals(), (Numbers{1, 2}));        // nothing deleted
  Unblock(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
  Expect("b", "2");
  ExpectMissing("c");
}

TEST_F(AsyncFlushTest, ReopenAfterAFailedFlushWorksAndTheNextFlushSucceeds) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForHalt());
  Close();
  Unblock(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(Open());           // replays wal-1 and wal-2
  ASSERT_NO_FATAL_FAILURE(Flush());
  Close();
  EXPECT_EQ(Ssts(), Numbers{1});
  EXPECT_EQ(Wals(), Numbers{3});             // the successful flush cleared both old WALs
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
}

// The first flush is durable; the second one fails. The first SSTable must stay,
// and the second flush's data must still be recoverable from its WAL.
TEST_F(AsyncFlushTest, SecondFlushFailingLeavesTheFirstSstableIntact) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_NO_FATAL_FAILURE(Flush());
  ASSERT_TRUE(WaitUntil([&] { return Ssts() == Numbers{1} && Wals() == Numbers{2}; }));
  Block(SstP(2) + ".tmp");
  ASSERT_TRUE(db_->Put("b", "2").ok());      // goes to wal-2
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForHalt());
  Close();
  EXPECT_EQ(Ssts(), Numbers{1});
  EXPECT_EQ(Wals(), (Numbers{2, 3}));        // wal-2 still holds "b"
  Unblock(SstP(2) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(Open());
  Expect("a", "1");
  Expect("b", "2");
}

// ============================================================ the ordering race
//
// Flush 1 fails while flush 2 is already queued behind it. If the worker kept
// going, flush 2 could succeed and delete wal-1, the only copy of flush 1's data.
//
// The blocker sits on the FINAL file name, so flush 1 does all of its work (write,
// fsync) and fails only at the rename. That keeps the worker busy long enough for
// the main thread to queue flush 2. The blocker is removed at a random moment, so
// flush 2 sometimes succeeds, sometimes fails, and sometimes is never queued.
// Whatever happens, every write that was acknowledged must survive a restart.
TEST_F(AsyncFlushTest, AnAcknowledgedWriteIsNeverLostWhateverTheInterleaving) {
  std::mt19937 rng(12345);
  for (int iter = 0; iter < 100; ++iter) {
    SCOPED_TRACE("iteration " + std::to_string(iter));
    ResetDir();
    ASSERT_NO_FATAL_FAILURE(Open(/*sync=*/false));   // fast writes: flush 2 queues early
    const std::string blocked = SstP(1);              // rename() onto it fails
    Block(blocked);
    const int delay_us = static_cast<int>(rng() % 6000);
    std::thread unblocker([&] {
      std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
      Unblock(blocked);
    });

    std::map<std::string, std::string> acked;
    if (db_->Put("a", "1").ok()) acked["a"] = "1";
    db_->FlushMemTableToSstable();           // may fail later, may be refused
    if (db_->Put("b", "2").ok()) acked["b"] = "2";
    db_->FlushMemTableToSstable();

    unblocker.join();                        // the blocker is gone from here on
    Close();                                 // joins the worker (and must not hang)
    ASSERT_NO_FATAL_FAILURE(Open(/*sync=*/false));
    for (const auto& kv : acked) Expect(kv.first, kv.second);
  }
}

// ============================================================ concurrency stress

// Reads on the main thread while the worker writes SSTables and deletes WALs.
// Mainly useful under ThreadSanitizer, where a data race fails the run.
TEST_F(AsyncFlushTest, ReadsStayCorrectWhileFlushesRunInTheBackground) {
  constexpr int kCycles = 100;
  ASSERT_NO_FATAL_FAILURE(Open(/*sync=*/false));
  for (int i = 0; i < kCycles; ++i) {
    ASSERT_TRUE(db_->Put(Key(i), "v" + std::to_string(i)).ok());
    ASSERT_NO_FATAL_FAILURE(Flush());
    Expect(Key(i / 2), "v" + std::to_string(i / 2));   // an older, already flushed key
    ExpectMissing("never-written");
  }
  Close();
  EXPECT_EQ(Ssts(), Range(1, kCycles));
  EXPECT_EQ(Wals(), Numbers{kCycles + 1});
  ASSERT_NO_FATAL_FAILURE(Open(/*sync=*/false));
  for (int i = 0; i < kCycles; ++i) Expect(Key(i), "v" + std::to_string(i));
}

}  // namespace
}  // namespace minilsm