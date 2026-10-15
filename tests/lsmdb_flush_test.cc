// Brick 12d: synchronous flush. Tests for LSMDB::Flush(), size-triggered
// flush, recovery in Open(), and failure behaviour.
//
// Works with the current header: Open(dir, sync, out) and the public
// FlushMemTableToSstable(). Without a configurable limit the size-trigger tests
// write a few MiB past the built-in 4 MiB limit (slower, still fine).
// If you add Open(dir, sync, size_t limit, out), compile with
// -DMINILSM_TEST_HAS_LIMIT to make those tests use a tiny limit instead.
// File naming assumed: sst-%06llu.sst and wal-%06llu.log, each with its own
// counter starting at 1.
#include "minilsm/lsm_db.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <chrono>
#include <thread>

namespace minilsm {
namespace {

namespace fs = std::filesystem;

#ifdef MINILSM_TEST_HAS_LIMIT
constexpr size_t kLimit = 512;
#else
constexpr size_t kLimit = 4u * 1024 * 1024;  // the built-in limit
#endif

// limit == 0: ordinary open (sync on). limit != 0: bulk-test open (sync off for
// speed; a process restart still sees every write()).
Status OpenDb(const std::string& dir, size_t limit, std::unique_ptr<LSMDB>* db) {
  if (limit == 0) return LSMDB::Open(dir, /*sync=*/true, db);
#ifdef MINILSM_TEST_HAS_LIMIT
  return LSMDB::Open(dir, /*sync=*/false, limit, db);  // <-- adapt
#else
  return LSMDB::Open(dir, /*sync=*/false, db);
#endif
}

template <typename Pred>
bool WaitUntil(Pred ok) {
  for (int i = 0; i < 500; ++i) {
    if (ok()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// Strict "<prefix><digits><suffix>" parse.
bool ParseNum(const std::string& name, const std::string& pre,
              const std::string& suf, uint64_t* n) {
  if (name.size() <= pre.size() + suf.size()) return false;
  if (name.compare(0, pre.size(), pre) != 0) return false;
  if (name.compare(name.size() - suf.size(), suf.size(), suf) != 0) return false;
  std::string d = name.substr(pre.size(), name.size() - pre.size() - suf.size());
  if (d.find_first_not_of("0123456789") != std::string::npos) return false;
  *n = std::stoull(d);
  return true;
}

class FlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = (fs::temp_directory_path() /
            (std::string("minilsm_flush_") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name()))
               .string();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    db_.reset();
    fs::remove_all(dir_);
  }

  void Open(size_t limit = 0) {
    db_.reset();
    Status s = OpenDb(dir_, limit, &db_);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_TRUE(db_ != nullptr);
  }
  void Close() { db_.reset(); }

  Status FlushDb() {
  Status s = db_->FlushMemTableToSstable();
  if(!s.ok())
  {
    return s;
  }
  db_.reset();                          // dtor joins the worker after it drains the queue
  Status o = LSMDB::Open(dir_, /*sync=*/true, &db_);
  return s.ok() ? o : s;
  }

  // Failure path: the flush is accepted, the task fails later and halts the DB.
  void FlushAndWaitForHalt() {
    ASSERT_TRUE(db_->FlushMemTableToSstable().ok());
    std::string v;
    ASSERT_TRUE(WaitUntil([&] {
      Status s = db_->Get("probe", &v);
      return !s.ok() && !s.IsNotFound();
    }));
  }

  // Wait until `count` SSTables exist. Only valid when no WAL deletion follows.
  void FlushAndWaitForSst(size_t count) {
    ASSERT_TRUE(db_->FlushMemTableToSstable().ok());
    ASSERT_TRUE(WaitUntil([&] { return Ssts().size() == count; }));
  }

  std::string SstP(uint64_t n) const { return Name("sst-", n, ".sst"); }
  std::string WalP(uint64_t n) const { return Name("wal-", n, ".log"); }
  std::string Name(const char* pre, uint64_t n, const char* suf) const {
    char b[32];
    std::snprintf(b, sizeof b, "%06llu", (unsigned long long)n);
    return dir_ + "/" + pre + b + suf;
  }

  // Sorted numbers of regular files matching the pattern.
  std::vector<uint64_t> Numbers(const std::string& pre, const std::string& suf) const {
    std::vector<uint64_t> out;
    for (const auto& e : fs::directory_iterator(dir_)) {
      if (!e.is_regular_file()) continue;
      uint64_t n;
      if (ParseNum(e.path().filename().string(), pre, suf, &n)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
  }
  std::vector<uint64_t> Ssts() const { return Numbers("sst-", ".sst"); }
  std::vector<uint64_t> Wals() const { return Numbers("wal-", ".log"); }

  uint64_t CurrentWal() const {
    auto w = Wals();
    EXPECT_EQ(w.size(), 1u);
    return w.empty() ? 0 : w.back();
  }

  void Expect(const std::string& k, const std::string& v) {
    std::string got;
    Status s = db_->Get(k, &got);
    EXPECT_TRUE(s.ok()) << "key " << k << ": " << s.message();
    EXPECT_EQ(got, v) << "key " << k;
  }
  void ExpectMissing(const std::string& k) {
    std::string got;
    Status s = db_->Get(k, &got);
    EXPECT_TRUE(s.IsNotFound()) << "key " << k << " should be absent";
  }

  // A directory squatting on a path makes opening that file fail. Used only to
// make WalWriter::Open / SstableWriter::Open fail on demand.
  void Block(const std::string& path) {      // non-empty dir: survives unlink AND fs::remove
  fs::create_directory(path);
  std::ofstream(path + "/x") << "x";
  }
  void Unblock(const std::string& path) { fs::remove_all(path); }

  std::string dir_;
  std::unique_ptr<LSMDB> db_;
};

// ---------------------------------------------------------------- basics

TEST_F(FlushTest, FlushOnEmptyMemTableIsNoOp) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(FlushDb().ok());
  EXPECT_TRUE(Ssts().empty());
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n});
}

TEST_F(FlushTest, FlushWritesSstableAndRotatesWal) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  EXPECT_EQ(Ssts(), std::vector<uint64_t>{1});      // sst-N from wal-N
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n + 1});  // old WAL gone, new one live
}

TEST_F(FlushTest, DataReadableAfterFlush) {
  Open();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  ASSERT_TRUE(FlushDb().ok());
  Expect("a", "1");
  Expect("b", "2");
  ExpectMissing("c");
}

TEST_F(FlushTest, WritesAfterFlushGoToTheNewWalAndSurviveRestart) {
  Open();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());  // must land in the NEW wal
  Close();
  Open();
  Expect("a", "1");  // from the SSTable
  Expect("b", "2");  // from the new WAL
}

TEST_F(FlushTest, RestartAfterFlushKeepsEverything) {
  Open();
  for (int i = 0; i < 20; i++)
    ASSERT_TRUE(db_->Put("k" + std::to_string(100 + i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(FlushDb().ok());
  Close();
  Open();
  for (int i = 0; i < 20; i++)
    Expect("k" + std::to_string(100 + i), "v" + std::to_string(i));
  EXPECT_EQ(Wals().size(), 1u);
}

TEST_F(FlushTest, FlushedDataSurvivesTwoRestarts) {
  Open();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  Close();
  Open();
  Close();
  Open();
  Expect("a", "1");
}

// ------------------------------------------------- newest wins / tombstones

TEST_F(FlushTest, NewerValueBeatsOlderFileAndMemTable) {
  Open();
  ASSERT_TRUE(db_->Put("k", "old").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("k", "mid").ok());
  ASSERT_TRUE(FlushDb().ok());
  Expect("k", "mid");  // newer file beats older file
  ASSERT_TRUE(db_->Put("k", "new").ok());
  Expect("k", "new");  // MemTable beats files
  Close();
  Open();
  Expect("k", "new");
}

TEST_F(FlushTest, TombstoneInMemTableShadowsFlushedValue) {
  Open();
  ASSERT_TRUE(db_->Put("k", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Delete("k").ok());
  ExpectMissing("k");
  Close();
  Open();  // delete is replayed from the WAL
  ExpectMissing("k");
}

TEST_F(FlushTest, TombstoneInNewerFileShadowsOlderFile) {
  Open();
  ASSERT_TRUE(db_->Put("k", "1").ok());
  ASSERT_TRUE(db_->Put("other", "x").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Delete("k").ok());
  ASSERT_TRUE(FlushDb().ok());
  ExpectMissing("k");  // tombstone must stop the search at the newer file
  Expect("other", "x");
  Close();
  Open();
  ExpectMissing("k");
  Expect("other", "x");
}

TEST_F(FlushTest, TombstoneForUnknownKeyIsHarmlessAcrossFlush) {
  Open();
  ASSERT_TRUE(db_->Delete("z").ok());
  ASSERT_TRUE(FlushDb().ok());
  EXPECT_EQ(Ssts().size(), 1u);
  ExpectMissing("z");
  Close();
  Open();
  ExpectMissing("z");
}

TEST_F(FlushTest, PutAfterDeleteAcrossFilesBringsKeyBack) {
  Open();
  ASSERT_TRUE(db_->Put("k", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Delete("k").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("k", "2").ok());
  Expect("k", "2");
  Close();
  Open();
  Expect("k", "2");
}

TEST_F(FlushTest, MissingKeyFallsThroughAllFiles) {
  Open();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  ASSERT_TRUE(FlushDb().ok());
  ExpectMissing("c");
  Expect("a", "1");  // found in the OLDER file even though the newer lacks it
  Expect("b", "2");
}

// ------------------------------------------------------------- numbering

TEST_F(FlushTest, FileNumbersIncreaseWithEachFlush) {
  Open();
  const uint64_t n = CurrentWal();
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(db_->Put("k" + std::to_string(i), "v").ok());
    ASSERT_TRUE(FlushDb().ok());
  }
  EXPECT_EQ(Ssts(), (std::vector<uint64_t>{1, 2, 3}));  // SSTable numbers have their own counter
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n + 3});
}

TEST_F(FlushTest, NumberingContinuesAfterRestartWithoutOverwriting) {
  Open();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  ASSERT_TRUE(FlushDb().ok());
  const auto before = Ssts();
  Close();
  Open();
  ASSERT_TRUE(db_->Put("c", "3").ok());
  ASSERT_TRUE(FlushDb().ok());
  const auto after = Ssts();
  ASSERT_EQ(after.size(), before.size() + 1);
  EXPECT_GT(after.back(), before.back());
  Expect("a", "1");
  Expect("b", "2");
  Expect("c", "3");
}

// ------------------------------------------------------ size-triggered flush

// Fixed-width key so every entry has the same size.
std::string Key(const char* prefix, int i, size_t width = 12) {
  std::string k = prefix + std::to_string(100000 + i);
  if (k.size() < width) k.append(width - k.size(), '_');
  return k;
}

TEST_F(FlushTest, SizeTriggerFlushesAndKeepsAllKeys) {
  Open(kLimit);
  const size_t vsize = kLimit / 25;  // ~25 entries per MemTable
  const int count = static_cast<int>(kLimit * 3 / (vsize + 16)) + 1;
  std::map<std::string, std::string> model;
  for (int i = 0; i < count; i++) {
    std::string k = Key("key", i);
    std::string v = std::to_string(i) + std::string(vsize, 'v');
    ASSERT_TRUE(db_->Put(k, v).ok());
    model[k] = v;
  }
  EXPECT_GE(Ssts().size(), 2u) << "75 entries (~3x the limit in total) must have flushed at least twice";
  EXPECT_EQ(Wals().size(), 1u);
  for (auto& kv : model) Expect(kv.first, kv.second);
  Close();
  Open(kLimit);
  for (auto& kv : model) Expect(kv.first, kv.second);
}

TEST_F(FlushTest, SizeTriggerDeleteAlsoCountsTowardLimit) {
  Open(kLimit);
  // Tombstones carry no value but still take MemTable space.
  const size_t key_size = std::max<size_t>(kLimit / 40, 32);
  const int count = static_cast<int>(kLimit * 3 / key_size) + 1;
  for (int i = 0; i < count; i++)
    ASSERT_TRUE(db_->Delete(Key("gone", i, key_size)).ok());
  EXPECT_FALSE(Ssts().empty()) << "tombstones take space and must trigger a flush";
  for (int i = 0; i < count; i += 7) ExpectMissing(Key("gone", i, key_size));
}

TEST_F(FlushTest, OverwritingOneKeyDoesNotGrowTheMemTable) {
  Open(kLimit);
  // The same key rewritten many times occupies one entry, not N.
  const std::string v(kLimit / 10, 'o');
  for (int i = 0; i < 50; i++) ASSERT_TRUE(db_->Put("same", v).ok());
  EXPECT_TRUE(Ssts().empty()) << "50 overwrites of one key hold ~1 entry; no flush yet";
  Expect("same", v);
}

// Design: an entry that cannot fit in the MemTable limit is rejected, so the
// MemTable never exceeds kLimit. The check must happen BEFORE the WAL append,
// otherwise a restart would replay the rejected entry into the MemTable.
TEST_F(FlushTest, EntryLargerThanLimitIsRejectedAndNotLogged) {
  Open(kLimit);
  const uint64_t n = CurrentWal();
  const auto wal_size = fs::file_size(WalP(n));
  EXPECT_FALSE(db_->Put("big", std::string(kLimit, 'x')).ok());
  EXPECT_EQ(fs::file_size(WalP(n)), wal_size) << "rejected entry must not reach the WAL";
  ExpectMissing("big");
  EXPECT_TRUE(Ssts().empty());
  ASSERT_TRUE(db_->Put("ok", "1").ok());  // DB still usable
  Close();
  Open(kLimit);
  ExpectMissing("big");                   // not replayed from the WAL
  Expect("ok", "1");
}

TEST_F(FlushTest, OversizedDeleteKeyIsRejectedAndNotLogged) {
  Open(kLimit);
  const uint64_t n = CurrentWal();
  const auto wal_size = fs::file_size(WalP(n));
  EXPECT_FALSE(db_->Delete(std::string(kLimit, 'k')).ok());
  EXPECT_EQ(fs::file_size(WalP(n)), wal_size);
  EXPECT_TRUE(Ssts().empty());
}

TEST_F(FlushTest, LargeButLegalEntryIsAccepted) {
  Open(kLimit);
  const std::string v(kLimit / 2, 'x');
  ASSERT_TRUE(db_->Put("big", v).ok());
  Expect("big", v);
  EXPECT_TRUE(Ssts().empty()) << "fits in the MemTable, nothing to flush";
  Close();
  Open(kLimit);
  Expect("big", v);
}

TEST_F(FlushTest, EntryThatDoesNotFitAlongsideOthersFlushesThemFirst) {
  Open(kLimit);
  const std::string a(kLimit * 6 / 10, 'a');
  const std::string b(kLimit * 6 / 10, 'b');  // a + b > limit, each alone is legal
  ASSERT_TRUE(db_->Put("a", a).ok());
  EXPECT_TRUE(Ssts().empty());
  ASSERT_TRUE(db_->Put("b", b).ok());         // must flush "a", then hold "b"
  EXPECT_EQ(Ssts().size(), 1u);
  Expect("a", a);
  Expect("b", b);
  Close();
  Open(kLimit);
  Expect("a", a);
  Expect("b", b);
}

TEST_F(FlushTest, ManyRoundsWithOverwritesMatchesModel) {
  Open(kLimit);
  const size_t pad = kLimit / 20;
  std::map<std::string, std::string> model;
  for (int round = 0; round < 6; round++) {
    for (int i = 0; i < 40; i++) {
      std::string k = "k" + std::to_string(100 + (i * 7 + round) % 50);
      if ((i + round) % 5 == 0) {
        ASSERT_TRUE(db_->Delete(k).ok());
        model.erase(k);
      } else {
        std::string v = "r" + std::to_string(round) + "i" + std::to_string(i) +
                        std::string(pad, 'p');
        ASSERT_TRUE(db_->Put(k, v).ok());
        model[k] = v;
      }
    }
    if (round % 2 == 1) {  // restart in the middle of the workload
      Close();
      Open(kLimit);
    }
  }
  EXPECT_FALSE(Ssts().empty());
  auto check = [&] {
    for (int i = 0; i < 50; i++) {
      std::string k = "k" + std::to_string(100 + i);
      auto it = model.find(k);
      if (it == model.end()) ExpectMissing(k); else Expect(k, it->second);
    }
  };
  check();
  Close();
  Open(kLimit);
  check();
}

// ---------------------------------------------------------- failed SSTable

TEST_F(FlushTest, FailedSstableWriteKeepsDataAndWal) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  Block(SstP(1) + ".tmp");

  ASSERT_TRUE(db_->FlushMemTableToSstable().ok());     // accepted; fails in the background

  std::string v;                                       // wait for the halt, don't reopen yet
  ASSERT_TRUE(WaitUntil([&] {
    Status s = db_->Get("probe", &v);
    return !s.ok() && !s.IsNotFound();
  }));
  EXPECT_FALSE(db_->Put("c", "3").ok());               // writes refused while halted

  db_.reset();                                         // join the worker
  EXPECT_TRUE(Ssts().empty());
  EXPECT_EQ(Wals(), (std::vector<uint64_t>{n, n + 1})) << "no WAL may be deleted";

  Unblock(SstP(1) + ".tmp");
  Open();                                              // restart replays every WAL
  Expect("a", "1");
  Expect("b", "2");
}

TEST_F(FlushTest, FailedFlushHaltsDbButKeepsAcknowledgedData) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForHalt());
  EXPECT_FALSE(db_->Put("b", "2").ok());      // halted: refused, and not logged
  EXPECT_FALSE(db_->Delete("a").ok());
  Close();
  EXPECT_TRUE(Ssts().empty());
  EXPECT_EQ(Wals(), (std::vector<uint64_t>{n, n + 1}));   // nothing deleted
  Unblock(SstP(1) + ".tmp");
  Open();
  Expect("a", "1");
  ExpectMissing("b");                         // rejected writes never reach the WAL
}

TEST_F(FlushTest, ReopenAfterFailedFlushRecoversAndNextFlushSucceeds) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(SstP(1) + ".tmp");
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForHalt());
  Close();
  Unblock(SstP(1) + ".tmp");
  Open();                                     // replays wal-n and wal-n+1
  Expect("a", "1");
  ASSERT_TRUE(FlushDb().ok());
  EXPECT_EQ(Ssts(), std::vector<uint64_t>{1});
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n + 2});   // the new flush clears both old WALs
  Expect("a", "1");
  Close();
  Open();
  Expect("a", "1");
}

// ------------------------------------------- SSTable ok, new WAL cannot open

// Once sst-N is durable the data is safe. If the next WAL cannot be created we
// keep appending to wal-N, and a restart replays wal-N in full, so nothing is
// lost. (Replaying the flushed part again only duplicates it.)
TEST_F(FlushTest, WalRotationFailureDoesNotLoseWrites) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(WalP(n + 1));                          // opening the next WAL fails
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForSst(1));
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n}); // old WAL kept; ClearWal had nothing to remove
  Expect("a", "1");
  ASSERT_TRUE(db_->Put("b", "2").ok());        // goes to the old WAL
  Expect("b", "2");
  Close();
  Unblock(WalP(n + 1));
  Open();
  Expect("a", "1");
  Expect("b", "2");
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n});
  ASSERT_TRUE(FlushDb().ok());                 // rotates for real, removes the old WAL
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n + 1});
  EXPECT_EQ(Ssts().size(), 2u);
  Expect("a", "1");
  Expect("b", "2");
}

TEST_F(FlushTest, DeleteAfterFailedRotationIsNotLost) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(WalP(n + 1));
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForSst(1));
  ASSERT_TRUE(db_->Delete("a").ok());
  ExpectMissing("a");
  Close();
  Unblock(WalP(n + 1));
  Open();
  ExpectMissing("a");                          // replay: Put(a), Delete(a) -> deleted
}

TEST_F(FlushTest, RestartAfterFailedRotationRecoversCleanly) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Block(WalP(n + 1));
  ASSERT_NO_FATAL_FAILURE(FlushAndWaitForSst(1));
  Close();
  Unblock(WalP(n + 1));
  Open();
  Expect("a", "1");
  EXPECT_EQ(Ssts(), std::vector<uint64_t>{1});
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n});
}

// ----------------------------------------------------------- crash windows

// Window A: crash after sst-N became durable and wal-(N+1) was created, but
// before wal-N was deleted. Open replays both; data is duplicated, not lost.
// The stash is the FULL content wal-N had when it was flushed.
TEST_F(FlushTest, StaleWalIsReplayedHarmlesslyAfterCrash) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("k", "v1").ok());
  ASSERT_TRUE(db_->Delete("k").ok());
  ASSERT_TRUE(db_->Put("other", "x").ok());
  const std::string stash = dir_ + "/stash";
  fs::copy_file(WalP(n), stash);
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("z", "9").ok());  // goes to wal-(N+1)
  Close();
  fs::copy_file(stash, WalP(n));         // "crash": wal-N was never deleted
  fs::remove(stash);
  Open();
  ExpectMissing("k");                    // tombstone survives the replay
  Expect("other", "x");
  Expect("z", "9");
  EXPECT_EQ(Wals(), (std::vector<uint64_t>{n, n + 1}));
}

TEST_F(FlushTest, NextFlushRemovesEveryStaleWal) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  const std::string stash = dir_ + "/stash";
  fs::copy_file(WalP(n), stash);
  ASSERT_TRUE(FlushDb().ok());
  ASSERT_TRUE(db_->Put("b", "2").ok());
  Close();
  fs::copy_file(stash, WalP(n));
  fs::remove(stash);
  Open();
  ASSERT_TRUE(db_->Put("c", "3").ok());
  ASSERT_TRUE(FlushDb().ok());
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n + 2});
  EXPECT_EQ(Ssts().size(), 2u);
  Expect("a", "1");
  Expect("b", "2");
  Expect("c", "3");
  Close();
  Open();
  Expect("a", "1");
  Expect("b", "2");
  Expect("c", "3");
}

// Window B: crash after sst-N became durable, before wal-(N+1) was created.
TEST_F(FlushTest, CrashBeforeNewWalExistsKeepsOldWal) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  const std::string stash = dir_ + "/stash";
  fs::copy_file(WalP(n), stash);
  ASSERT_TRUE(FlushDb().ok());
  Close();
  fs::remove(WalP(n + 1));
  fs::copy_file(stash, WalP(n));
  fs::remove(stash);
  Open();
  Expect("a", "1");
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n});
  ASSERT_TRUE(db_->Put("x", "9").ok());
  Close();
  Open();
  Expect("a", "1");
  Expect("x", "9");
}

// Window C: crash during the SSTable write leaves a *.sst.tmp behind.
TEST_F(FlushTest, StaleTempSstableIsRemovedOnOpen) {
  {
    std::ofstream f(dir_ + "/sst-000099.sst.tmp");
    f << "half written garbage";
  }
  Open();
  EXPECT_FALSE(fs::exists(dir_ + "/sst-000099.sst.tmp"));
  EXPECT_TRUE(Ssts().empty());
}

TEST_F(FlushTest, FirstWalIsReplayedWhenNoSstableExists) {
  Open();
  const uint64_t n = CurrentWal();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  Close();
  Open();
  Expect("a", "1");
  EXPECT_EQ(Wals(), std::vector<uint64_t>{n});
}

// ------------------------------------------------------------ bad files

TEST_F(FlushTest, CorruptSstableFailsOpen) {
  Open();
  for (int i = 0; i < 10; i++)
    ASSERT_TRUE(db_->Put("key" + std::to_string(100 + i), "value" + std::to_string(i)).ok());
  ASSERT_TRUE(FlushDb().ok());
  Close();
  {
    std::fstream f(SstP(1), std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(2);
    char c;
    f.get(c);
    c = static_cast<char>(c ^ 0xFF);
    f.seekp(2);
    f.put(c);
  }
  std::unique_ptr<LSMDB> db;
  ASSERT_TRUE(OpenDb(dir_, 0, &db).ok());  // open reads only the filters
  std::string v;
  Status s = db->Get("key100", &v);
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsNotFound()) << "corruption must not look like a missing key";
}

TEST_F(FlushTest, UnrelatedAndMalformedNamesAreIgnored) {
  for (const char* name : {"notes.txt", "sst-abc.sst", "wal-1x.log", "sst-000007.sst.bak"}) {
    std::ofstream f(dir_ + "/" + name);
    f << "x";
  }
  Open();
  ASSERT_TRUE(db_->Put("a", "1").ok());
  ASSERT_TRUE(FlushDb().ok());
  Expect("a", "1");
  EXPECT_TRUE(fs::exists(dir_ + "/notes.txt"));
  EXPECT_TRUE(fs::exists(dir_ + "/sst-000007.sst.bak"));
}

}  // namespace
}  // namespace minilsm