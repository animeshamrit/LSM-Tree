#pragma once

#include <memory>
#include <string>

#include "minilsm/status.h"
#include "minilsm/wal_record.h"

namespace minilsm {

class WalWriter {
 public:
  // Opens (or creates) the file for appending.
  static Status Open(const std::string& path, bool sync,
                     std::unique_ptr<WalWriter>* out);

  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status Append(const WalRecord& record);
  Status Sync();
  Status Close();

 private:
  WalWriter(int fd, bool sync) : fd_(fd), sync_(sync) {} //intialize directly in constructor
//   WalWriter(int fd, bool sync) { //intialize by default values and then assigned, fails for const members
//     fd_ = fd;
//     sync_ = sync;
//   }

  int fd_;
  bool sync_;
};

}  // namespace minilsm