// Copyright (c) 2025 VNote
#include "core/buffer_provider.h"

#include <filesystem>
#include <fstream>
#include <limits>

#include "utils/file_utils.h"

namespace vxcore {

VxCoreError SaveBufferFileContent(const std::string &full_path, const std::vector<uint8_t> &body) {
  try {
    const auto path = PathFromUtf8(full_path);
    const auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
      std::filesystem::create_directories(parent);
    }
    if (body.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file || (!body.empty() && !file.write(reinterpret_cast<const char *>(body.data()),
                                              static_cast<std::streamsize>(body.size())))) {
      return VXCORE_ERR_IO;
    }
    file.close();
    return file.fail() ? VXCORE_ERR_IO : VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

}  // namespace vxcore
