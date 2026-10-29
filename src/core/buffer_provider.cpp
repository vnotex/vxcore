// Copyright (c) 2025 VNote
#include "buffer_provider.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>

#include "utils/file_utils.h"

#include "bundled_notebook.h"
#include "external_buffer_provider.h"
#include "notebook.h"
#include "standard_buffer_provider.h"
#include "utils/logger.h"

namespace vxcore {

VxCoreError IBufferProvider::LoadContent(const std::string &full_path,
                                       std::vector<uint8_t> &out_body) {
  return IsEncrypted() ? VXCORE_ERR_UNSUPPORTED : LoadBufferFileContent(full_path, out_body);
}

VxCoreError IBufferProvider::SaveContent(const std::string &full_path,
                                       const std::vector<uint8_t> &body) {
  return IsEncrypted() ? VXCORE_ERR_UNSUPPORTED : SaveBufferFileContent(full_path, body);
}

VxCoreError LoadBufferFileContent(const std::string &full_path, std::vector<uint8_t> &out_body) {
  try {
    const auto path = PathFromUtf8(full_path);
    if (!std::filesystem::exists(path)) return VXCORE_ERR_NOT_FOUND;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return VXCORE_ERR_IO;
    const std::streamoff size = file.tellg();
    if (size < 0) return VXCORE_ERR_IO;
    if (static_cast<uintmax_t>(size) > out_body.max_size() ||
        size > std::numeric_limits<std::streamsize>::max()) return VXCORE_ERR_OUT_OF_MEMORY;
    out_body.resize(static_cast<size_t>(size));
    file.seekg(0);
    if (!file || (size > 0 && !file.read(reinterpret_cast<char *>(out_body.data()),
                                        static_cast<std::streamsize>(size)))) {
      out_body.clear();
      return VXCORE_ERR_IO;
    }
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

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

std::shared_ptr<IBufferProvider> CreateBufferProvider(Notebook *notebook,
                                                      const std::string &file_path) {
  if (!notebook) {
    VXCORE_LOG_ERROR("Cannot create buffer provider: notebook is null");
    return nullptr;
  }

  try {
    return std::make_shared<StandardBufferProvider>(notebook, file_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create StandardBufferProvider: %s", e.what());
    return nullptr;
  }
}

std::shared_ptr<IBufferProvider> CreateBufferProviderForExternal(
    const std::string &absolute_file_path) {
  if (absolute_file_path.empty()) {
    VXCORE_LOG_ERROR("Cannot create external buffer provider: path is empty");
    return nullptr;
  }

  try {
    return std::make_shared<ExternalBufferProvider>(absolute_file_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create ExternalBufferProvider: %s", e.what());
    return nullptr;
  }
}

}  // namespace vxcore
