// Copyright (c) 2025 VNote
#include "buffer_provider.h"

#include "bundled_notebook.h"
#include "external_buffer_provider.h"
#include "notebook.h"
#include "standard_buffer_provider.h"
#include "utils/logger.h"

namespace vxcore {

std::unique_ptr<IBufferProvider> CreateBufferProvider(Notebook *notebook,
                                                      const std::string &file_path) {
  if (!notebook) {
    VXCORE_LOG_ERROR("Cannot create buffer provider: notebook is null");
    return nullptr;
  }

  // Only bundled notebooks support buffer providers
  if (notebook->GetTypeStr() != "bundled") {
    VXCORE_LOG_DEBUG("Notebook type '%s' does not support buffer provider",
                     notebook->GetTypeStr().c_str());
    return nullptr;
  }

  try {
    return std::make_unique<StandardBufferProvider>(notebook, file_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create StandardBufferProvider: %s", e.what());
    return nullptr;
  }
}

std::unique_ptr<IBufferProvider> CreateBufferProviderForExternal(
    const std::string &absolute_file_path) {
  if (absolute_file_path.empty()) {
    VXCORE_LOG_ERROR("Cannot create external buffer provider: path is empty");
    return nullptr;
  }

  try {
    return std::make_unique<ExternalBufferProvider>(absolute_file_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create ExternalBufferProvider: %s", e.what());
    return nullptr;
  }
}

}  // namespace vxcore
