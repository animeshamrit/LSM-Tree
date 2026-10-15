#include "minilsm/wal_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace minilsm {

namespace {

Status ErrnoStatus(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

}  // namespace

Status WalWriter::Open(const std::string& path, bool sync,
                       std::unique_ptr<WalWriter>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return ErrnoStatus("open " + path);
  }
  out->reset(new WalWriter(fd, sync));
  return Status::OK();
}

WalWriter::~WalWriter() {
  Close();  // ignore the result: nothing useful to do in a destructor
}

Status WalWriter::Append(const WalRecord& record) {
  if (fd_ < 0) {
    return Status::InvalidArgument("writer is closed");
  }

  std::string buf;
  EncodeWalRecord(record, &buf);

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

  if (sync_) {
    return Sync();
  }
  return Status::OK();
}

Status WalWriter::Sync() {
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

Status WalWriter::Close() {
  if (fd_ < 0) {
    return Status::OK();  // already closed
  }
  int rc = ::close(fd_);
  fd_ = -1;
  if (rc != 0) {
    return ErrnoStatus("close");
  }
  return Status::OK();
}

}  // namespace minilsm