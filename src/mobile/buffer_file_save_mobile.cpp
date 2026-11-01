// Copyright (c) 2025 VNote
#include "core/buffer_provider.h"

#include <filesystem>
#include <new>
#include <stdexcept>

#include "mobile/file_writer_mobile.h"
#include "utils/file_utils.h"

namespace vxcore {

VxCoreError SaveBufferFileContent(const std::string &full_path, const std::vector<uint8_t> &body) {
  try {
    const auto path = PathFromUtf8(full_path);
    const auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
      std::filesystem::create_directories(parent);
    }
    MobileFileWriter writer(path);
    VxCoreError err = writer.Open();
    if (err == VXCORE_OK) err = writer.Write(body.data(), body.size());
    return err == VXCORE_OK ? writer.Commit() : err;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

}  // namespace vxcore
