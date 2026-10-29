#include "memory_buffer_provider.h"

#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <vxcore/notebook_json_keys.h>

#include "utils/file_utils.h"

namespace vxcore {
namespace {
template <typename Function>
VxCoreError CallbackResult(Function &&function) noexcept {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

struct CallbackGuard {
  std::atomic_flag &active;
  ~CallbackGuard() { active.clear(std::memory_order_release); }
};

struct ContentSink {
  std::vector<uint8_t> body;
  VxCoreError error = VXCORE_OK;

  static VxCoreError Append(void *userdata, const void *data, size_t size) noexcept {
    auto &sink = *static_cast<ContentSink *>(userdata);
    if (sink.error != VXCORE_OK) return sink.error;
    sink.error = CallbackResult([&]() -> VxCoreError {
      if (!data && size) return VXCORE_ERR_NULL_POINTER;
      if (size > sink.body.max_size() - sink.body.size()) return VXCORE_ERR_OUT_OF_MEMORY;
      if (size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        sink.body.insert(sink.body.end(), bytes, bytes + size);
      }
      return VXCORE_OK;
    });
    return sink.error;
  }
};
}  // namespace

VxCoreError MemoryBufferProvider::ValidateOptions(const nlohmann::json &options) {
  if (!options.is_object()) return VXCORE_ERR_INVALID_PARAM;
  for (const auto *key : {kJsonKeyDisplayName, kJsonKeyEditorType}) {
    const auto value = options.find(key);
    if (value == options.end() || !value->is_string()) return VXCORE_ERR_INVALID_PARAM;
    const auto &text = value->get_ref<const std::string &>();
    if (text.empty() || text.find('\0') != std::string::npos) return VXCORE_ERR_INVALID_PARAM;
  }
  const auto read_only = options.find(kJsonKeyReadOnly);
  if (read_only != options.end() && !read_only->is_boolean()) return VXCORE_ERR_INVALID_PARAM;
  const auto base = options.find(kJsonKeyResourceBasePath);
  if (base != options.end()) {
    if (!base->is_string()) return VXCORE_ERR_INVALID_PARAM;
    const auto &text = base->get_ref<const std::string &>();
    if (text.find('\0') != std::string::npos) return VXCORE_ERR_INVALID_PARAM;
    if (!text.empty()) {
      const auto path = PathFromUtf8(text);
      std::error_code error;
      if (!path.is_absolute() || !std::filesystem::is_directory(path, error) || error) {
        return VXCORE_ERR_INVALID_PARAM;
      }
    }
  }
  return VXCORE_OK;
}

MemoryBufferProvider::MemoryBufferProvider(const nlohmann::json &options,
                                           VxCoreBufferContentCallbacks callbacks)
    : display_name_(options.at(kJsonKeyDisplayName).get<std::string>()),
      editor_type_(options.at(kJsonKeyEditorType).get<std::string>()),
      resource_base_path_(options.value(kJsonKeyResourceBasePath, std::string())),
      read_only_(options.value(kJsonKeyReadOnly, false)),
      callbacks_(callbacks) {}

MemoryBufferProvider::~MemoryBufferProvider() {
  if (owns_userdata_ && callbacks_.destroy) {
    // Destruction is a C ABI boundary too; client exceptions must never escape.
    try { callbacks_.destroy(callbacks_.userdata); } catch (...) {}
  }
}

VxCoreError MemoryBufferProvider::LoadInitialContent(const void *data, size_t size,
                                                    std::vector<uint8_t> &out_body) {
  if (callbacks_.load) return LoadContent({}, out_body);
  return CallbackResult([&]() -> VxCoreError {
    if (!data && size) return VXCORE_ERR_NULL_POINTER;
    out_body.resize(size);
    if (size) std::memcpy(out_body.data(), data, size);
    return VXCORE_OK;
  });
}

VxCoreError MemoryBufferProvider::LoadContent(const std::string &,
                                             std::vector<uint8_t> &out_body) {
  if (!callbacks_.load) return VXCORE_ERR_UNSUPPORTED;
  if (callback_active_.test_and_set(std::memory_order_acquire)) return VXCORE_ERR_INVALID_STATE;
  CallbackGuard guard{callback_active_};
  ContentSink sink;
  const auto error = CallbackResult([&]() {
    return callbacks_.load(callbacks_.userdata, &ContentSink::Append, &sink);
  });
  if (sink.error != VXCORE_OK) return sink.error;
  if (error != VXCORE_OK) return error;
  out_body.swap(sink.body);
  return VXCORE_OK;
}

VxCoreError MemoryBufferProvider::SaveContent(const std::string &,
                                             const std::vector<uint8_t> &body) {
  if (read_only_) return VXCORE_ERR_READ_ONLY;
  if (!callbacks_.save) return VXCORE_ERR_UNSUPPORTED;
  if (callback_active_.test_and_set(std::memory_order_acquire)) return VXCORE_ERR_INVALID_STATE;
  CallbackGuard guard{callback_active_};
  return CallbackResult([&]() {
    return callbacks_.save(callbacks_.userdata, body.empty() ? nullptr : body.data(), body.size());
  });
}

VxCoreError MemoryBufferProvider::GetResourceBasePath(std::string &out_path) {
  out_path = resource_base_path_;
  return out_path.empty() ? VXCORE_ERR_UNSUPPORTED : VXCORE_OK;
}

VxCoreError MemoryBufferProvider::ReadResource(const std::string &resource_url,
                                              std::vector<uint8_t> &out_data) {
  out_data.clear();
  if (resource_base_path_.empty()) return VXCORE_ERR_UNSUPPORTED;
  const auto error = CallbackResult([&]() -> VxCoreError {
    const auto relative = PathFromUtf8(resource_url);
    if (relative.empty() || relative.has_root_path() ||
        resource_url.find('\0') != std::string::npos) return VXCORE_ERR_INVALID_PARAM;
    const auto path = PathFromUtf8(resource_base_path_) / relative;
    if (!IsPathWithin(resource_base_path_, PathToUtf8(path), false)) return VXCORE_ERR_INVALID_PARAM;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return ec ? VXCORE_ERR_IO : VXCORE_ERR_NOT_FOUND;
    return IBufferProvider::LoadContent(PathToUtf8(path), out_data);
  });
  if (error != VXCORE_OK) out_data.clear();
  return error;
}

VxCoreError MemoryBufferProvider::InsertAssetRaw(const std::string &,
                                                const std::vector<uint8_t> &, std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::InsertAsset(const std::string &, std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::DeleteAsset(const std::string &) { return VXCORE_ERR_UNSUPPORTED; }
VxCoreError MemoryBufferProvider::GetAssetsFolder(std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::GetAssetAbsolutePath(const std::string &, std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::InsertAttachment(const std::string &, std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::DeleteAttachment(const std::string &) {
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::RenameAttachment(const std::string &, const std::string &,
                                                   std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::ListAttachments(std::vector<std::string> &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}
VxCoreError MemoryBufferProvider::GetAttachmentsFolder(std::string &out) {
  out.clear();
  return VXCORE_ERR_UNSUPPORTED;
}

}  // namespace vxcore
