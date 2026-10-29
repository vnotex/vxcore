#ifndef VXCORE_MEMORY_BUFFER_PROVIDER_H
#define VXCORE_MEMORY_BUFFER_PROVIDER_H

#include <atomic>

#include "buffer_provider.h"

namespace vxcore {

// Runtime source; callbacks own no notebook/context and may outlive the open buffer.
class MemoryBufferProvider final : public IBufferProvider {
 public:
  static VxCoreError ValidateOptions(const nlohmann::json &options);
  MemoryBufferProvider(const nlohmann::json &options, VxCoreBufferContentCallbacks callbacks);
  ~MemoryBufferProvider() override;

  // Opening is provisional until the C API has also allocated its returned ID.
  void AdoptUserdata() noexcept { owns_userdata_ = true; }
  VxCoreError LoadInitialContent(const void *data, size_t size, std::vector<uint8_t> &out_body);
  std::string GetType() const override { return "memory"; }
  bool IsMemoryBacked() const noexcept override { return true; }
  bool IsReadOnly() const noexcept override { return read_only_; }
  bool CanSave() const noexcept override { return callbacks_.save && !read_only_; }
  bool CanReload() const noexcept override { return callbacks_.load != nullptr; }
  const std::string &GetDisplayName() const { return display_name_; }
  const std::string &GetEditorType() const { return editor_type_; }

  VxCoreError LoadContent(const std::string &full_path, std::vector<uint8_t> &out_body) override;
  VxCoreError SaveContent(const std::string &full_path, const std::vector<uint8_t> &body) override;
  VxCoreError ReadResource(const std::string &resource_url, std::vector<uint8_t> &out_data) override;
  VxCoreError GetResourceBasePath(std::string &out_path) override;

  VxCoreError InsertAssetRaw(const std::string &, const std::vector<uint8_t> &,
                            std::string &out) override;
  VxCoreError InsertAsset(const std::string &, std::string &out) override;
  VxCoreError DeleteAsset(const std::string &) override;
  VxCoreError GetAssetsFolder(std::string &out) override;
  VxCoreError GetAssetAbsolutePath(const std::string &, std::string &out) override;
  VxCoreError InsertAttachment(const std::string &, std::string &out) override;
  VxCoreError DeleteAttachment(const std::string &) override;
  VxCoreError RenameAttachment(const std::string &, const std::string &, std::string &out) override;
  VxCoreError ListAttachments(std::vector<std::string> &out) override;
  VxCoreError GetAttachmentsFolder(std::string &out) override;

 private:
  const std::string display_name_;
  const std::string editor_type_;
  const std::string resource_base_path_;
  const bool read_only_;
  const VxCoreBufferContentCallbacks callbacks_;
  std::atomic_flag callback_active_ = ATOMIC_FLAG_INIT;
  bool owns_userdata_ = false;
};

}  // namespace vxcore

#endif  // VXCORE_MEMORY_BUFFER_PROVIDER_H
