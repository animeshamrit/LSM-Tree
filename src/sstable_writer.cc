#include "minilsm/sstable_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace minilsm {

namespace {

Status ErrnoStatus(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

Status Sync(int fd_) {
  if (fd_ < 0) {
    return Status::InvalidArgument("writer is closed");
  }
#ifdef __APPLE__
  // On macOS, fsync() does not force the drive's own cache to flush.
  // F_FULLFSYNC does.
  if (::fcntl(fd_, F_FULLFSYNC) == 0) return Status::OK();
#endif
  if (::fsync(fd_) != 0) {
    return ErrnoStatus("fsync");
  }
  return Status::OK();
}

Status SyncParentDir(const std::string& path) {
  std::filesystem::path parent = std::filesystem::path(path).parent_path();
  std::string dir = parent.empty() ? "." : parent.string();
  int dfd = ::open(dir.c_str(), O_RDONLY);
  if (dfd < 0) return ErrnoStatus("open dir " + dir);
  Status s = Sync(dfd);
  ::close(dfd);
  return s;
}

}  // namespace

Status SstableWriter::Open(const std::string& path,
                           std::unique_ptr<SstableWriter>* out) {
  ::unlink((path + ".tmp").c_str());  // remove any leftover temp file
  int fd = ::open((path+".tmp").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return ErrnoStatus("open " + (path+".tmp"));
  }
  out->reset(new SstableWriter(fd, path));
  return Status::OK();
}

SstableWriter::~SstableWriter() {
  Close();  // ignore the result: nothing useful to do in a destructor
}

Status SstableWriter::WriteSstableFile(const std::string& buf) {
  if (fd_ < 0) {
    return Status::InvalidArgument("writer is closed");
  }

  // write() may write fewer bytes than asked, so loop until done.
  const char* p = buf.data();
  size_t left = buf.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;  // interrupted by a signal: retry
      return ErrnoStatus("write");
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
    Status s = Sync(fd_);
    int rc = ::close(fd_); fd_ = -1;
    if (s.ok() && rc != 0) s = ErrnoStatus("close");
    
    std::string tmp_path = path_ + ".tmp";
    if (s.ok() && ::rename(tmp_path.c_str(), path_.c_str()) != 0) s = ErrnoStatus("rename");
    if (!s.ok()) { ::unlink(tmp_path.c_str()); return s; } 
    finished_ = true;
    return SyncParentDir(path_); 
}

Status SstableWriter::Close() {
  if (fd_ < 0) {
    return Status::OK();  // already closed
  }
  int rc = ::close(fd_);
  fd_ = -1;

  std::string tmp_path = path_ + ".tmp";
  if (!finished_) ::unlink(tmp_path.c_str());

  if (rc != 0) {
    return ErrnoStatus("close");
  }
  return Status::OK();
}

}  // namespace minilsm