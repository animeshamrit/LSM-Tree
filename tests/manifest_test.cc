#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "minilsm/manifest.h"
#include "minilsm/manifest_reader.h"
#include "minilsm/manifest_writer.h"
#include "minilsm/lsm_db.h"

namespace minilsm {
namespace {

namespace fs = std::filesystem;

class ManifestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tmpl = (fs::temp_directory_path() / "minilsm_manifest_XXXXXX").string();
    ASSERT_NE(::mkdtemp(&tmpl[0]), nullptr);
    dir_ = tmpl;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string Path(const std::string& name) const { return dir_ + "/" + name; }
  std::string ManifestPath() const { return Path("MANIFEST"); }
  std::string SstName(uint64_t n) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "sst-%06llu.sst", (unsigned long long)n);
    return Path(buf);
  }

  void WriteRaw(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), bytes.size());
  }
  std::string ReadRaw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  }

  Manifest Make(uint64_t max, std::vector<uint64_t> nums) {
    Manifest m;
    m.set_max_sstable_filenum(max);
    m.set_sstable_filenum(nums);
    return m;
  }

  void WriteManifestFile(const Manifest& m) {
    std::unique_ptr<ManifestWriter> w;
    ASSERT_TRUE(ManifestWriter::Open(ManifestPath(), &w).ok());
    ASSERT_TRUE(w->WriteManifest(m).ok());
  }

  // Put one key, flush, so each call produces one SSTable.
  void FlushOneKey(LSMDB* db, const std::string& key) {
    ASSERT_TRUE(db->Put(key, "value-" + key).ok());
    ASSERT_TRUE(db->FlushMemTableToSstable().ok());
  }

  std::string dir_;
};

// ---------- encode / decode / reader ----------

TEST_F(ManifestTest, RoundTripEmptyList) {
  WriteManifestFile(Make(0, {}));
  Manifest got;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &got).ok());
  EXPECT_EQ(got.get_max_sstable_filenum(), 0u);
  EXPECT_TRUE(got.get_sstable_filenum().empty());
}

TEST_F(ManifestTest, RoundTripOneNumber) {
  WriteManifestFile(Make(7, {7}));
  Manifest got;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &got).ok());
  EXPECT_EQ(got.get_max_sstable_filenum(), 7u);
  EXPECT_EQ(got.get_sstable_filenum(), (std::vector<uint64_t>{7}));
}

TEST_F(ManifestTest, RoundTripManyNumbers) {
  std::vector<uint64_t> nums;
  for (uint64_t i = 1; i <= 500; ++i) nums.push_back(i);
  WriteManifestFile(Make(900, nums));
  Manifest got;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &got).ok());
  EXPECT_EQ(got.get_max_sstable_filenum(), 900u);
  EXPECT_EQ(got.get_sstable_filenum(), nums);
}

TEST_F(ManifestTest, MissingFileIsNotFound) {
  Manifest got;
  Status s = ReadManifestFile(ManifestPath(), &got);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(ManifestTest, EmptyFileIsCorruption) {
  WriteRaw(ManifestPath(), "");
  Manifest got;
  EXPECT_TRUE(ReadManifestFile(ManifestPath(), &got).IsCorruption());
}

TEST_F(ManifestTest, ShortFileIsCorruption) {
  WriteRaw(ManifestPath(), std::string(11, 'x'));  // header is 12 bytes
  Manifest got;
  EXPECT_TRUE(ReadManifestFile(ManifestPath(), &got).IsCorruption());
}

TEST_F(ManifestTest, LengthNotMultipleOfEightIsCorruption) {
  std::string enc;
  Manifest::EncodeManifestRecord(Make(3, {1, 2, 3}), &enc);
  WriteRaw(ManifestPath(), enc + "abc");
  Manifest got;
  EXPECT_TRUE(ReadManifestFile(ManifestPath(), &got).IsCorruption());
}

TEST_F(ManifestTest, TruncatedByEightBytesIsCorruption) {
  std::string enc;
  Manifest::EncodeManifestRecord(Make(3, {1, 2, 3}), &enc);
  enc.resize(enc.size() - 8);  // still a valid length, but the CRC no longer matches
  WriteRaw(ManifestPath(), enc);
  Manifest got;
  EXPECT_TRUE(ReadManifestFile(ManifestPath(), &got).IsCorruption());
}

TEST_F(ManifestTest, FlippedByteAnywhereIsCorruption) {
  std::string enc;
  Manifest::EncodeManifestRecord(Make(3, {1, 2, 3}), &enc);
  for (size_t i = 0; i < enc.size(); ++i) {
    std::string bad = enc;
    bad[i] = static_cast<char>(bad[i] ^ 0x01);
    WriteRaw(ManifestPath(), bad);
    Manifest got;
    EXPECT_TRUE(ReadManifestFile(ManifestPath(), &got).IsCorruption())
        << "flip at byte " << i << " was not detected";
  }
}

TEST_F(ManifestTest, DecodeClearsOutputOnFailure) {
  Manifest got = Make(9, {4, 5});
  Status s = Manifest::DecodeManifestRecord("short", &got);
  EXPECT_TRUE(s.IsCorruption());
  EXPECT_EQ(got.get_max_sstable_filenum(), 0u);
  EXPECT_TRUE(got.get_sstable_filenum().empty());
}

// ---------- writer ----------

TEST_F(ManifestTest, SecondWriteOnSameWriterFailsCleanly) {
  std::unique_ptr<ManifestWriter> w;
  ASSERT_TRUE(ManifestWriter::Open(ManifestPath(), &w).ok());
  ASSERT_TRUE(w->WriteManifest(Make(2, {1, 2})).ok());

  Status s = w->WriteManifest(Make(3, {1, 2, 3}));
  EXPECT_FALSE(s.ok());

  Manifest got;  // the published file is untouched
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &got).ok());
  EXPECT_EQ(got.get_sstable_filenum(), (std::vector<uint64_t>{1, 2}));
}

TEST_F(ManifestTest, FailureBeforeRenameKeepsOldManifest) {
  WriteManifestFile(Make(2, {1, 2}));

  // A directory at MANIFEST.tmp makes the writer's open() fail.
  fs::create_directory(ManifestPath() + ".tmp");
  std::unique_ptr<ManifestWriter> w;
  EXPECT_FALSE(ManifestWriter::Open(ManifestPath(), &w).ok());

  Manifest got;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &got).ok());
  EXPECT_EQ(got.get_max_sstable_filenum(), 2u);
  EXPECT_EQ(got.get_sstable_filenum(), (std::vector<uint64_t>{1, 2}));
}

TEST_F(ManifestTest, ClosingWriterWithoutWritingLeavesNoTempFile) {
  {
    std::unique_ptr<ManifestWriter> w;
    ASSERT_TRUE(ManifestWriter::Open(ManifestPath(), &w).ok());
  }
  EXPECT_FALSE(fs::exists(ManifestPath() + ".tmp"));
  EXPECT_FALSE(fs::exists(ManifestPath()));
}

// ---------- LSMDB::Open with a MANIFEST ----------

TEST_F(ManifestTest, ReopenAfterSeveralFlushesListsAllOfThem) {
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "a");
    FlushOneKey(db.get(), "b");
    FlushOneKey(db.get(), "c");
  }  // destructor drains the background queue

  Manifest m;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &m).ok());
  EXPECT_EQ(m.get_sstable_filenum(), (std::vector<uint64_t>{1, 2, 3}));
  EXPECT_EQ(m.get_max_sstable_filenum(), 3u);

  std::unique_ptr<LSMDB> db;
  ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
  for (const std::string k : {"a", "b", "c"}) {
    std::string v;
    ASSERT_TRUE(db->Get(k, &v).ok()) << k;
    EXPECT_EQ(v, "value-" + k);
  }
}

TEST_F(ManifestTest, OpenDeletesUnlistedSstable) {
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "a");
  }
  // A valid SSTable the MANIFEST does not list: an orphan.
  fs::copy_file(SstName(1), SstName(99));
  ASSERT_TRUE(fs::exists(SstName(99)));

  std::unique_ptr<LSMDB> db;
  ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
  EXPECT_FALSE(fs::exists(SstName(99)));
  EXPECT_TRUE(fs::exists(SstName(1)));
}

TEST_F(ManifestTest, OpenFailsWhenListedSstableIsMissing) {
  WriteManifestFile(Make(5, {5}));  // sst-000005.sst does not exist
  std::unique_ptr<LSMDB> db;
  Status s = LSMDB::Open(dir_, false, &db);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsCorruption());
}

TEST_F(ManifestTest, OpenFailsOnCorruptManifest) {
  WriteRaw(ManifestPath(), "garbage-garbage-garbage");
  std::unique_ptr<LSMDB> db;
  EXPECT_FALSE(LSMDB::Open(dir_, false, &db).ok());
}

TEST_F(ManifestTest, OpenFailsOnEmptyManifestAndKeepsSstables) {
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "a");
  }
  WriteRaw(ManifestPath(), "");
  std::unique_ptr<LSMDB> db;
  EXPECT_FALSE(LSMDB::Open(dir_, false, &db).ok());
  EXPECT_TRUE(fs::exists(SstName(1)));  // must not be treated as an orphan
}

TEST_F(ManifestTest, NoManifestBootstrapsFromDiskWithoutDeleting) {
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "a");
    FlushOneKey(db.get(), "b");
  }
  ASSERT_EQ(::unlink(ManifestPath().c_str()), 0);  // a database from before the MANIFEST

  std::unique_ptr<LSMDB> db;
  ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
  EXPECT_TRUE(fs::exists(SstName(1)));
  EXPECT_TRUE(fs::exists(SstName(2)));

  for (const std::string k : {"a", "b"}) {
    std::string v;
    ASSERT_TRUE(db->Get(k, &v).ok()) << k;
    EXPECT_EQ(v, "value-" + k);
  }
}

TEST_F(ManifestTest, NumbersAreNotReusedAfterBootstrap) {
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "a");
    FlushOneKey(db.get(), "b");
  }
  ASSERT_EQ(::unlink(ManifestPath().c_str()), 0);
  const std::string sst1_before = ReadRaw(SstName(1));
  const std::string sst2_before = ReadRaw(SstName(2));
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "c");
  }
  EXPECT_EQ(ReadRaw(SstName(1)), sst1_before);  // not overwritten
  EXPECT_EQ(ReadRaw(SstName(2)), sst2_before);
  EXPECT_TRUE(fs::exists(SstName(3)));

  Manifest m;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &m).ok());
  EXPECT_EQ(m.get_sstable_filenum(), (std::vector<uint64_t>{1, 2, 3}));
}

TEST_F(ManifestTest, CounterSkipsOrphanNumberOnDisk) {
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());
    FlushOneKey(db.get(), "a");
  }
  // An orphan above the manifest counter, as a crash after rename would leave.
  fs::copy_file(SstName(1), SstName(2));
  {
    std::unique_ptr<LSMDB> db;
    ASSERT_TRUE(LSMDB::Open(dir_, false, &db).ok());  // deletes sst-2 as orphan
    FlushOneKey(db.get(), "b");
  }
  Manifest m;
  ASSERT_TRUE(ReadManifestFile(ManifestPath(), &m).ok());
  ASSERT_EQ(m.get_sstable_filenum().size(), 2u);
  EXPECT_GT(m.get_sstable_filenum().back(), 2u);  // never reuses the orphan's number
}

}  // namespace
}  // namespace minilsm
