#pragma once

#include <memory>
#include <string>

#include "minilsm/status.h"
#include "minilsm/sstable_format.h"

namespace minilsm {

class SstableWriter {
 public:
  // Opens (or creates) the file for appending.
  static Status Open(const std::string& path,
                     std::unique_ptr<SstableWriter>* out);

  ~SstableWriter();

  SstableWriter(const SstableWriter&) = delete;
  SstableWriter& operator=(const SstableWriter&) = delete;

  Status WriteSstableFile(const std::string& buf);
  Status Close();

 private:
  SstableWriter(int fd, std::string path) : fd_(fd), path_(path) {} //intialize directly in constructor
//   SstableWriter(int fd, bool sync) { //intialize by default values and then assigned, fails for const members
//     fd_ = fd;
//     sync_ = sync;
//   }

  int fd_;
  bool finished_ = false;
  std::string path_;  // store the path for renaming after sync
};

}  // namespace minilsm