#pragma once

#include <string>
#include <memory>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>

#include "minilsm/status.h"
#include "minilsm/db.h"
#include "minilsm/memtable.h"
#include "minilsm/wal_writer.h"

namespace minilsm {

class LSMDB : public DB {
private:
  MemTable memtable_;
  std::unique_ptr<WalWriter> wal_; 
  size_t recovery_dropped_bytes_ = 0;
  std::string dir_;
  bool sync_;
  uint64_t next_sst_number_ = 1;
  std::vector<uint64_t> live_wals_;  // the numbers of the WALs that are still live
  std::deque<SstableView> sstables_;
  std::vector<BloomFilter> bloomfilters_;

  std::queue<std::function<void()>> bg_queue_;

  std::atomic<bool> is_halted_{false};
  Status bg_error_status_{Status::OK()};

  std::mutex status_mutex_;
  std::mutex queue_mutex_;
  std::mutex wal_mutex_;  
  std::mutex sstable_mutex;
  std::condition_variable queue_cv_;
  std::thread bg_thread_;
  bool shutdown_ = false;

  void BackgroundWorker();
  void SubmitBackgroundTask(std::function<void()> task);

  Status CheckDatabaseState() const {
        if (is_halted_.load(std::memory_order_relaxed)) {
            return Status::IOError("Database is HALTED due to a critical background write failure.");
        }
        return Status::OK();
    }

  void PanicDatabase(Status failure_reason);

 

 public:
  LSMDB();
  ~LSMDB();
  static Status Open(const std::string& dir, bool sync,
                     std::unique_ptr<LSMDB>* out);
  Status Put(const std::string& key, const std::string& value) override;
  Status Get(const std::string& key, std::string* value) override;
  Status Delete(const std::string& key) override;
  Status FlushMemTableToSstable();
  Status RotateWal();
  Status ClearWal(uint64_t current_wal_number);

  size_t recovery_dropped_bytes() const { return recovery_dropped_bytes_; }

  static constexpr size_t kmaximum_bytes_ = 4 * 1024 * 1024;  // 4 MiB of memtable
};

}  // namespace minilsm