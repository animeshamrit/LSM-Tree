#include "minilsm/manifest_reader.h"
#include "minilsm/manifest.h"
#include "minilsm/coding.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include <cstring>
#include <sys/stat.h>

namespace minilsm {

// Reads the Manifest file and retrieves the string
Status ReadManifestFile(const std::string& path, Manifest* record ) {

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0){
        if (errno == ENOENT) return Status::NotFound("MANIFEST " + path);
        return Status::IOError("open " + path + ": " + std::strerror(errno));
    } 

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
  return Manifest::DecodeManifestRecord(std::move(data), record); 
}

}  // namespace minilsm