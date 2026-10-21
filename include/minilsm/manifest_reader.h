#pragma once

#include <string>

#include "minilsm/status.h"
#include "minilsm/manifest.h"

namespace minilsm {
Status ReadManifestFile(const std::string& path, Manifest* record);
}  // namespace minilsm