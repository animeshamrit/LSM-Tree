#pragma once

#include <memory>
#include <string>

#include "minilsm/status.h"
#include "minilsm/manifest.h"

namespace minilsm {

class ManifestWriter {
 public:
  // Opens (or creates) the file for appending.
  static Status Open(const std::string& path, std::unique_ptr<ManifestWriter>* out);

  ~ManifestWriter();

  ManifestWriter(const ManifestWriter&) = delete;
  ManifestWriter& operator=(const ManifestWriter&) = delete;

  Status WriteManifest(const Manifest& record);
  Status Close();

 private:
  ManifestWriter(int fd, std::string path) : fd_(fd), path_(path) {}
  int fd_;
  bool finished_ = false;
  std::string path_;  // store the path for renaming after sync
};

}  // namespace minilsm