// Copyright (c) 2025 VNote
#ifndef VXCORE_STANDARD_BUFFER_PROVIDER_H
#define VXCORE_STANDARD_BUFFER_PROVIDER_H

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <string>
#include <vector>

#include "buffer_provider.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;

// StandardBufferProvider implements IBufferProvider for notebook-backed files
// (bundled and raw). Assets are stored in <notebook_root>/<assets_folder>/<file_uuid>/.
// Attachment operations require per-file vx.json metadata and are therefore
// supported for bundled notebooks only; they return VXCORE_ERR_UNSUPPORTED for raw.
class StandardBufferProvider : public IBufferProvider {
 public:
  // Constructor.
  // @param notebook: The notebook instance (not owned).
  // @param file_path: The relative path of the file within the notebook.
  StandardBufferProvider(Notebook *notebook, const std::string &file_path);

  ~StandardBufferProvider() override;

  // IBufferProvider interface implementation
  std::string GetType() const override { return "standard"; }
  bool IsEncrypted() const noexcept override { return encrypted_; }
  VxCoreError GetProtectionError() const noexcept override { return protection_error_; }

  VxCoreError LoadEncryptedContent(std::vector<uint8_t> &out_body);
  VxCoreError SaveEncryptedContent(const std::vector<uint8_t> &body);
  VxCoreError WriteEncryptedBackup(const std::vector<uint8_t> &body, int revision);
  VxCoreError RecoverEncryptedBackup(std::vector<uint8_t> &out_body, int &out_revision);
  VxCoreError CheckEncryptedSnapshot(bool &out_matches);
  std::string GetAuthenticatedEditorType() const;

  // Only an authenticated, open note can resolve logical resources. Callers serialize
  // these operations and note saves using the existing notebook IO gate.
  // Returned body/resource vectors and resource JSON belong to the caller, which
  // wipes them using NotebookEncryption helpers after consumption. No method opens
  // a closed note implicitly. This provider and its notebook must outlive the call.
  VxCoreError ReadProtectedResource(const std::string &url, std::vector<uint8_t> &out_data);
  VxCoreError ExportProtectedResource(const std::string &url,
                                      const std::string &destination_path);
  VxCoreError WriteProtectedComments(const std::vector<uint8_t> &data);
  VxCoreError ListProtectedResources(nlohmann::json &out_resources);

  // Asset operations (filesystem only, no metadata)
  VxCoreError InsertAssetRaw(const std::string &name, const std::vector<uint8_t> &data,
                             std::string &out_relative_path) override;

  VxCoreError InsertAsset(const std::string &source_path, std::string &out_relative_path) override;

  VxCoreError DeleteAsset(const std::string &relative_path) override;

  VxCoreError GetAssetsFolder(std::string &out_path) override;

  VxCoreError GetAssetAbsolutePath(const std::string &relative_path,
                                   std::string &out_abs_path) override;

  // Resource resolution
  VxCoreError ReadResource(const std::string &resource_url,
                          std::vector<uint8_t> &out_data) override;

  VxCoreError GetResourceBasePath(std::string &out_path) override;

  // Attachment operations (filesystem + metadata)
  // Protected insertion returns vxasset:<resourceId>; delete/rename take that
  // logical identity, not a display name. ListAttachments returns display names;
  // ListProtectedResources supplies their resource IDs without exposing filesystem paths.
  VxCoreError InsertAttachment(const std::string &source_path, std::string &out_filename) override;

  VxCoreError DeleteAttachment(const std::string &filename) override;

  VxCoreError RenameAttachment(const std::string &old_filename, const std::string &new_filename,
                               std::string &out_new_filename) override;

  VxCoreError ListAttachments(std::vector<std::string> &out_filenames) override;

  VxCoreError ListUnindexedAttachments(std::vector<std::string> &out_filenames) override;

  VxCoreError GetAttachmentsFolder(std::string &out_path) override;

  // Path identity overrides
  std::string GetFileId() const override { return file_id_; }
  void SetFilePath(const std::string &path) override;
  void SetFileState(const std::string &path, const nlohmann::json &metadata) override;

 private:
  struct ProtectedState;
  VxCoreError RequireProtectedState(bool writing) const;
  VxCoreError RequireUnchangedSnapshot();
  VxCoreError ResolveProtectedPath(const std::filesystem::path &path) const;
  VxCoreError ResolveProtectedAssets(std::filesystem::path &out_path, bool create);
  VxCoreError PublishProtectedManifest(nlohmann::json &manifest);
  VxCoreError ReadEncryptedBackup(std::vector<uint8_t> &out_body, nlohmann::json &out_manifest,
                                   int &out_revision);
  VxCoreError RefreshEncryptedBackupManifest(const nlohmann::json &manifest);
  VxCoreError InsertProtectedResource(const std::string &name, const std::string &media_type,
                                       const std::string &role, std::istream &data,
                                       std::string &out_url);
  VxCoreError InsertProtectedFile(const std::string &source_path, const std::string &role,
                                   std::string &out_url);
  VxCoreError DeleteProtectedResource(const std::string &identity, bool attachment);
  // True only for notebooks that persist per-file attachment metadata (bundled).
  bool AttachmentsSupported() const;

  // Ensures the assets folder exists, creating it if necessary.
  VxCoreError EnsureAssetsFolderExists();

  // Gets the assets folder path (internal helper, doesn't create folder)
  std::string GetAssetsFolderPath();

  // Generates a unique asset name if there's a collision.
  // @param base_name: The original filename.
  // @param assets_folder_path: The absolute path to the assets folder.
  // @return A unique filename.
  std::string GetUniqueAssetName(const std::string &base_name,
                                 const std::string &assets_folder_path);

  Notebook *notebook_;     // Not owned
  std::string file_path_;  // Relative path within notebook
  std::string file_id_;    // Cached file UUID
  bool encrypted_ = false;
  // 0=ordinary, 1=encrypted Markdown, 2=encrypted text, 3=malformed candidate.
  uint8_t metadata_mode_ = 0;
  VxCoreError protection_error_ = VXCORE_OK;
  std::unique_ptr<ProtectedState> protected_;
};

}  // namespace vxcore

#endif  // VXCORE_STANDARD_BUFFER_PROVIDER_H
