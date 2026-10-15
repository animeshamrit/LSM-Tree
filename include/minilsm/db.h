#pragma once

#include <string>

#include "minilsm/status.h"

namespace minilsm {

class DB {
 public:
  virtual ~DB() = default;

  virtual Status Put(const std::string& key, const std::string& value) = 0;
  virtual Status Get(const std::string& key, std::string* value) = 0;
  virtual Status Delete(const std::string& key) = 0;
};

} 