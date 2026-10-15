#include "minilsm/sstable_reader.h"
#include "minilsm/sstable_format.h"
#include "minilsm/coding.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include <cstring>
#include <sys/stat.h>

namespace minilsm {

// Reads the sstable file and retrieves the string
Status ReadSstableFile(const std::string& path, SstableView* view ) {
//   *result = WalReadResult();

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Status::IOError("open " + path + ": " + std::strerror(errno));

    struct stat st;
    int s =::fstat(fd, &st);
    if (s != 0) {
        Status s = Status::IOError("fstat " + path + ": " + std::strerror(errno));
        ::close(fd);  // build the message BEFORE close, because close() can overwrite errno
        return s;
    }
    std::string data;
    data.resize(st.st_size);
    size_t off = 0;
    while(off < st.st_size) {
        ssize_t n = ::read(fd, &data[off], st.st_size - off);
        if (n < 0) {
        if (errno == EINTR) continue;                // interrupted: retry
        Status s = Status::IOError("read " + path + ": " + std::strerror(errno));
        ::close(fd);                                 // build the message BEFORE close,
        return s;                                    // because close() can overwrite errno
    }
    if (n == 0) break;                              // 0 means end of file
    off += static_cast<size_t>(n);                           
  }
  ::close(fd);
  data.resize(off); 
  return SstableView::Parse(std::move(data), view); 
}

Status ReadBloomFilterBlock(const std::string& path, BloomFilterView* bloomfilter_ ) {
//   *result = WalReadResult();

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Status::IOError("open " + path + ": " + std::strerror(errno));

    off_t current_pos = lseek(fd, -40, SEEK_END);
    if (current_pos == (off_t)-1) {
        close(fd);
        Status s = Status::IOError("read " + path + ": " + "Seek to end failed!");
        return s;
    }

    std::string footer_buffer;
    footer_buffer.resize(8);
    size_t off = 0;
    
    while(off < 8) {
        ssize_t n = ::read(fd, &footer_buffer[off], 8 - off);
        if (n < 0) {
        if (errno == EINTR) continue;                // interrupted: retry
        Status s = Status::IOError("read " + path + ": " + std::strerror(errno));
        ::close(fd);                                 // build the message BEFORE close,
        return s;                                    // because close() can overwrite errno
        }
    if (n == 0) break;                              // 0 means end of file
    off += static_cast<size_t>(n);                           
    }

    uint64_t BloomFilter_offset = GetFixed64(&footer_buffer[0]);
    off_t file_size = lseek(fd, 0, SEEK_END);
    off_t footer_start = file_size - 40;

    uint64_t BloomFilter_size = footer_start - BloomFilter_offset;

    std::string data;
    data.resize(BloomFilter_size);
    off = 0;
    while(off < BloomFilter_size) {
        ssize_t n = ::read(fd, &data[off], BloomFilter_size - off);
        if (n < 0) {
        if (errno == EINTR) continue;                // interrupted: retry
        Status s = Status::IOError("read " + path + ": " + std::strerror(errno));
        ::close(fd);                                 // build the message BEFORE close,
        return s;                                    // because close() can overwrite errno
    }
    if (n == 0) break;                              // 0 means end of file
    off += static_cast<size_t>(n);                           
  }
  ::close(fd);
  data.resize(off); 
  return BloomFilter::deserialize(std::move(data), bloomfilter_); 
}

}  // namespace minilsm