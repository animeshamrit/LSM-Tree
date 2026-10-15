#pragma once

#include <string>
#include <utility>

namespace minilsm {

class Status {
public:
    enum class Code {
        kOk,
        kNotFound,
        kCorruption,
        kIOError,
        kInvalidArgument
    };
    
private:
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_;
  std::string msg_;

 public:

  // Default-constructed Status means success.
  Status() : code_(Code::kOk) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") {
    return Status(Code::kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg = "") {
    return Status(Code::kCorruption, std::move(msg));
  }
  static Status IOError(std::string msg = "") {
    return Status(Code::kIOError, std::move(msg));
  }
  static Status InvalidArgument(std::string msg = "") {
    return Status(Code::kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

};

}