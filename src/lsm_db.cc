#include "minilsm/lsm_db.h"

#include "minilsm/wal_reader.h"
#include "minilsm/wal_record.h"
#include "minilsm/sstable_writer.h"
#include "minilsm/sstable_reader.h"
#include "minilsm/bloomfilter.h"

#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <iostream>
#include <fstream>

namespace minilsm {

LSMDB::LSMDB() {
    bg_thread_ = std::thread(&LSMDB::BackgroundWorker, this);
}

LSMDB::~LSMDB() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_ = true;
    }
    queue_cv_.notify_one();
    if (bg_thread_.joinable()) {
        bg_thread_.join(); // Gracefully wait for running jobs to finish
    }
}

void LSMDB::PanicDatabase(Status failure_reason) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    
    // Prevent overriding an earlier critical error if multiple tasks failed
    if (is_halted_.load()) return;

    this->bg_error_status_ = failure_reason;
    this->is_halted_.store(true);

    // Log a severe error to the system console/logs
    std::cerr << "[CRITICAL ERROR] LSMDB Background Flush Failed: " 
              << failure_reason.message() 
              << ". Database is now locked down." << std::endl;
}

void LSMDB::BackgroundWorker() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return shutdown_ || is_halted_.load(std::memory_order_relaxed) || !bg_queue_.empty(); });
            
            if (is_halted_.load() || (shutdown_ && bg_queue_.empty())) return;
            
            task = std::move(bg_queue_.front());
            bg_queue_.pop();
        }
        task(); 
    }
}

void LSMDB::SubmitBackgroundTask(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    bg_queue_.push(std::move(task));
    queue_cv_.notify_one();
}

std::string SstPath(const std::string& dir, uint64_t n) {
  char name[32];
  std::snprintf(name, sizeof(name), "sst-%06llu.sst", (unsigned long long)n);
  return dir + "/" + name;
}

std::string WalPath(const std::string& dir, uint64_t n) {
  char name[32];
  std::snprintf(name, sizeof(name), "wal-%06llu.log", (unsigned long long)n);
  return dir + "/" + name;
}

bool ParseNumbered(const std::string& name, const std::string& prefix,
                   const std::string& suffix, uint64_t* n) {
  if (name.size() <= prefix.size() + suffix.size()) return false;
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  const std::string digits =
      name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (digits.size() > 18) return false;  // keeps stoull in range
  for (char c : digits) {
    if (c < '0' || c > '9') return false;
  }
  *n = std::stoull(digits);
  return true;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Status ScanDirectory(const std::string& dir, std::vector<uint64_t>* ssts,
                     std::vector<uint64_t>* wals) {
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec)) continue;
    const std::string name = it->path().filename().string();
    uint64_t n = 0;
    if (ParseNumbered(name, "sst-", ".sst", &n)) {
      ssts->push_back(n);
    } else if (ParseNumbered(name, "wal-", ".log", &n)) {
      wals->push_back(n);
    } else if (EndsWith(name, ".sst.tmp")) {
      std::filesystem::remove(it->path(), entry_ec);
    }
  }
  if (ec) return Status::IOError("scan " + dir + ": " + ec.message());
  std::sort(ssts->begin(), ssts->end());
  std::sort(wals->begin(), wals->end());
  return Status::OK();
}

Status LSMDB::Open(const std::string& dir, bool sync,
                   std::unique_ptr<LSMDB>* out) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return Status::IOError("create " + dir + ": " + ec.message());
  }

  std::unique_ptr<LSMDB> db(new LSMDB());
  db->dir_ = dir;
  db->sync_ = sync;

  std::vector<uint64_t> ssts, wals;
  Status s = ScanDirectory(dir, &ssts, &wals);
  if (!s.ok()) return s;

  for (uint64_t n : ssts) {
    BloomFilterView bloomfilter_view;
    s = ReadBloomFilterBlock(SstPath(dir, n), &bloomfilter_view);
    if (!s.ok()) return s;
    BloomFilter bloomfilter_(bloomfilter_view.num_bits_, bloomfilter_view.num_hashes_, bloomfilter_view.bits_);
    bloomfilter_.sst_filenum =  n;
    db->bloomfilters_.push_back(std::move(bloomfilter_));
  }  
  db->next_sst_number_ = ssts.empty() ? 1 : ssts.back() + 1;

//   for (uint64_t n : ssts) {
//     SstableView view;
//     s = ReadSstableFile(SstPath(dir, n), &view);
//     if (!s.ok()) return s;
//     db->sstables_.push_back(std::move(view));
//   }
//   db->next_sst_number_ = ssts.empty() ? 1 : ssts.back() + 1;

  for (size_t i = 0; i < wals.size(); ++i) {
    const std::string wal_path = WalPath(dir, wals[i]);
    // 1. Replay the existing log, if any.
    WalReadResult res;
    Status s = ReadWal(wal_path, &res);
    if (!s.ok() && !s.IsNotFound()) {
        return s;  // a real I/O error: don't guess
    }
    if (s.ok()) {
        for (const WalRecord& r : res.records) {
        if (r.type == WalRecord::Type::kPut) {
            db->memtable_.Put(r.key, r.value);
        } else {
            db->memtable_.Delete(r.key);
        }
        }
        db->live_wals_.push_back(wals[i]);
        // 2. Cut off a damaged tail so new records follow a valid prefix.
        if (!res.clean) {
            if(i+1 == wals.size()) {
                std::filesystem::resize_file(wal_path, res.valid_bytes, ec);
                if (ec) {
                    return Status::IOError("truncate " + wal_path + ": " + ec.message());
                }
            }
        db->recovery_dropped_bytes_ = res.file_size - res.valid_bytes;
        }
    }
}

  if (db->live_wals_.empty()) db->live_wals_.push_back(1);
  // 3. Only now start appending.
  s = WalWriter::Open(WalPath(dir, db->live_wals_.back()), sync, &db->wal_);
  if (!s.ok()) return s;

  *out = std::move(db);
  return Status::OK();
}

Status LSMDB::RotateWal() {
  std::lock_guard<std::mutex> lock(wal_mutex_);
  const uint64_t next_number = live_wals_.back() + 1;
  std::unique_ptr<WalWriter> next;
  Status s = WalWriter::Open(WalPath(dir_, next_number), sync_, &next);
  if (!s.ok()) return s;  // nothing changed: keep appending to the old WAL
 
  wal_ = std::move(next);
  live_wals_.push_back(next_number);
  return Status::OK();
}

Status LSMDB::ClearWal(uint64_t current_wal_number){
    std::lock_guard<std::mutex> lock(wal_mutex_);
    for (uint64_t n : live_wals_){
        if(n == current_wal_number) {
            break;
        }
        ::unlink(WalPath(dir_, n).c_str());
    }
    live_wals_.erase(live_wals_.begin(), std::find(live_wals_.begin(), live_wals_.end(), current_wal_number));
    return Status::OK();
}

Status LSMDB::FlushMemTableToSstable() {
    Status state_check = CheckDatabaseState();
    if (!state_check.ok()) return state_check;

    if (memtable_.Snapshot().empty()) {
        return Status::OK();  // nothing to flush
    }
    std::vector<SstEntry> entries = memtable_.Snapshot();
    BloomFilter bloomfilter(entries.size());
    for(auto it:entries)
    {
        bloomfilter.add(it.key);
    }

    uint64_t current_sst_number_ = next_sst_number_;
    {
        std::lock_guard<std::mutex> lock(sstable_mutex);
        bloomfilter.sst_filenum = next_sst_number_;
        next_sst_number_++;
    }

    std::string bloomfilter_bits;
    bloomfilter.serialize(bloomfilter_bits);

    bloomfilters_.push_back(std::move(bloomfilter));

    std::string sstable_data;
    Status s = BuildSstable(entries, bloomfilter_bits, &sstable_data);
    if(!s.ok()) {
        return s;
    }

    memtable_ = MemTable();
    RotateWal();  // start a new WAL for subsequent writes

    SstableView view;
    s = SstableView::Parse(sstable_data, &view);
    if(!s.ok()) {
        return s; 
    }

    {
        std::lock_guard<std::mutex> lock(sstable_mutex);
        sstables_.push_back(std::move(view));
    }

    uint64_t current_wal_number;
    {
        std::lock_guard<std::mutex> lock(wal_mutex_);
        current_wal_number = live_wals_.back();
    }

    SubmitBackgroundTask([this, sstable_data, current_wal_number, current_sst_number_]() {
        std::unique_ptr<SstableWriter> w;
        const std::string path = SstPath(this->dir_, current_sst_number_);
        
        Status s = SstableWriter::Open(path, &w);
        if (!s.ok()) 
        {
            this->PanicDatabase(s); 
            return; 
        }
        
        s = w->WriteSstableFile(sstable_data);
        if (!s.ok()) {
            ::unlink(path.c_str());
            this->PanicDatabase(s); 
            return; 
        }

        w->Close();
        {
            std::lock_guard<std::mutex> lock(sstable_mutex);
            this->sstables_.pop_front();
        }
        this->ClearWal(current_wal_number);
    });

    return Status::OK();  // Return immediately; the flush is happening in the background
}

Status LSMDB::Put(const std::string& key, const std::string& value) {
    Status state_check = CheckDatabaseState();
    if (!state_check.ok()) return state_check;

    size_t current_size = 1u + key.size() + value.size();
    if (current_size > kmaximum_bytes_) return Status::InvalidArgument("entry too large");
    if(memtable_.approximate_size() + current_size > kmaximum_bytes_) {
        // Handle overflow case
        Status s = LSMDB::FlushMemTableToSstable();
        if (!s.ok()) {
            return s;  // Return the error if flushing fails    
        }
    }

    if (wal_) {
    WalRecord r;
    r.type = WalRecord::Type::kPut;
    r.key = key;
    r.value = value;
    Status s = wal_->Append(r);
    if (!s.ok()) return s;  // WAL first: on failure, memory is untouched
    }
    memtable_.Put(key, value);
  return Status::OK();
}

Status LSMDB::Delete(const std::string& key) {
    Status state_check = CheckDatabaseState();
    if (!state_check.ok()) return state_check;

    size_t current_size = 1u + key.size();
    if (current_size > kmaximum_bytes_) return Status::InvalidArgument("entry too large");
    if(memtable_.approximate_size() + current_size > kmaximum_bytes_) {
        Status s = FlushMemTableToSstable();
        if (!s.ok()) {
            return s;  // Return the error if flushing fails    
        }
    } 

    if(wal_) {
        WalRecord r;
        r.type = WalRecord::Type::kDelete;
        r.key = key;
        Status s = wal_->Append(r);
        if (!s.ok()) return s;  // WAL first: on failure, memory is untouched
    }
    memtable_.Delete(key);
    return Status::OK();
}

Status LSMDB::Get(const std::string& key, std::string* value) {
  Status state_check = CheckDatabaseState();
  if (!state_check.ok()) return state_check;

  MemTable::GetResult mem = memtable_.Get(key, value);
  if (mem == MemTable::GetResult::kFound) return Status::OK();
  if (mem == MemTable::GetResult::kDeleted) return Status::NotFound("deleted");

  size_t skip ;
  {
    std::lock_guard<std::mutex> lock(sstable_mutex);
    skip = sstables_.size();
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {  // newest first
    SstableView::GetResult r = it->Get(key, value);
    if (r == SstableView::GetResult::kFound) return Status::OK();
    if (r == SstableView::GetResult::kDeleted) return Status::NotFound("deleted");
    // kNotPresent: fall through to the next older table
    }
  }

  for (auto it = bloomfilters_.rbegin(); it != bloomfilters_.rend(); ++it) {  // newest first
    if(skip > 0)
    {
        skip--;
        continue;
    }
    bool can_be_present = it->may_contain(key);
    if(can_be_present)
    {
        SstableView view;
        Status s = ReadSstableFile(SstPath(dir_, it->sst_filenum), &view);
        if (!s.ok()) return s;

        SstableView::GetResult r = view.Get(key, value);
        if (r == SstableView::GetResult::kFound) return Status::OK();
        if (r == SstableView::GetResult::kDeleted) return Status::NotFound("deleted");
    }
  }
  return Status::NotFound();
}


}  // namespace minilsm