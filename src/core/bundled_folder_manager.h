#ifndef VXCORE_BUNDLED_FOLDER_MANAGER_H
#define VXCORE_BUNDLED_FOLDER_MANAGER_H

#include <map>
#include <filesystem>
#include <memory>
#include <string>

#include "folder.h"
#include "folder_manager.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;

class BundledFolderManager : public FolderManager {
 public:
  explicit BundledFolderManager(Notebook *notebook);
  ~BundledFolderManager() override;

  VxCoreError InitOnCreation() override;

  VxCoreError GetFolderConfig(const std::string &folder_path,
                              std::string &out_config_json) override;

  VxCoreError CreateFolder(const std::string &parent_path, const std::string &folder_name,
                           std::string &out_folder_id) override;

  VxCoreError DeleteFolder(const std::string &folder_path) override;

  VxCoreError UpdateFolderMetadata(const std::string &folder_path,
                                   const std::string &metadata_json) override;

  VxCoreError GetFolderMetadata(const std::string &folder_path,
                                std::string &out_metadata_json) override;

  VxCoreError RenameFolder(const std::string &folder_path, const std::string &new_name) override;

  VxCoreError MoveFolder(const std::string &src_path, const std::string &dest_parent_path) override;

  VxCoreError CopyFolder(const std::string &src_path, const std::string &dest_parent_path,
                         const std::string &new_name, std::string &out_folder_id) override;

  VxCoreError CreateFile(const std::string &folder_path, const std::string &file_name,
                         std::string &out_file_id) override;

  VxCoreError DeleteFile(const std::string &file_path) override;

  VxCoreError UpdateFileMetadata(const std::string &file_path,
                                 const std::string &metadata_json) override;

  VxCoreError UpdateFileTags(const std::string &file_path, const std::string &tags_json) override;

  VxCoreError TagFile(const std::string &file_path, const std::string &tag_name) override;

  VxCoreError UntagFile(const std::string &file_path, const std::string &tag_name) override;

  VxCoreError GetFileInfo(const std::string &file_path, std::string &out_file_info_json) override;

  VxCoreError GetFileInfo(const std::string &file_path, const FileRecord **out_record) override;

  VxCoreError GetFileMetadata(const std::string &file_path,
                              std::string &out_metadata_json) override;

  VxCoreError RenameFile(const std::string &file_path, const std::string &new_name) override;

  VxCoreError MoveFile(const std::string &src_file_path,
                       const std::string &dest_folder_path) override;

  VxCoreError CopyFile(const std::string &src_file_path, const std::string &dest_folder_path,
                       const std::string &new_name, std::string &out_file_id) override;

  void IterateAllFiles(
      std::function<bool(const std::string &, const FileRecord &)> callback) override;

  VxCoreError FindFilesByTag(const std::string &tag_name, std::string &out_files_json) override;

  VxCoreError ListFolderContents(const std::string &folder_path, bool include_folders_info,
                                 FolderContents &out_contents) override;

  void ClearCache() override;

  // Syncs the MetadataStore from config files (vx.json)
  // Called on notebook open to rebuild cache from ground truth
  // Returns VXCORE_OK on success
  VxCoreError SyncMetadataStoreFromConfigs();

  // Returns the path to the recycle bin folder
  std::string GetRecycleBinPath() const;

 private:
  VxCoreError GetFolderConfig(const std::string &folder_path, FolderConfig **out_config,
                              const std::string *parent_id = nullptr);
  VxCoreError LoadFolderConfig(const std::string &folder_path,
                               std::unique_ptr<FolderConfig> &out_config);
  VxCoreError SaveFolderConfig(const std::string &folder_path, const FolderConfig &config);

  std::string GetConfigPath(const std::string &folder_path) const;
  std::string GetContentPath(const std::string &folder_path) const;

  void CacheConfig(const std::string &folder_path, std::unique_ptr<FolderConfig> config);
  FolderConfig *GetCachedConfig(const std::string &folder_path);
  void InvalidateCache(const std::string &folder_path);

  FileRecord *FindFileRecord(FolderConfig &config, const std::string &file_name);

  // Sync a single folder's data to MetadataStore after loading from vx.json
  // This is the lazy sync implementation - called when a folder is accessed
  void SyncFolderToStore(const std::string &folder_path, const FolderConfig &config,
                         const std::string &parent_folder_id);

  // Get the parent folder's ID (UUID) for a given folder path
  std::string GetParentFolderId(const std::string &folder_path);

  // Recycle bin helpers
  std::string GenerateUniqueRecycleBinName(const std::string &name) const;
  VxCoreError MoveToRecycleBin(const std::filesystem::path &source_path);
  std::map<std::string, std::unique_ptr<FolderConfig>> config_cache_;
};

}  // namespace vxcore

#endif
