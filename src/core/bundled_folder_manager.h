#ifndef VXCORE_BUNDLED_FOLDER_MANAGER_H
#define VXCORE_BUNDLED_FOLDER_MANAGER_H

#include <map>
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

  VxCoreError DeleteFile(const std::string &folder_path, const std::string &file_name) override;

  VxCoreError UpdateFileMetadata(const std::string &folder_path, const std::string &file_name,
                                 const std::string &metadata_json) override;

  VxCoreError UpdateFileTags(const std::string &folder_path, const std::string &file_name,
                             const std::string &tags_json) override;

  VxCoreError GetFileInfo(const std::string &folder_path, const std::string &file_name,
                          std::string &out_file_info_json) override;

  VxCoreError GetFileMetadata(const std::string &folder_path, const std::string &file_name,
                              std::string &out_metadata_json) override;

  VxCoreError RenameFile(const std::string &folder_path, const std::string &old_name,
                         const std::string &new_name) override;

  VxCoreError MoveFile(const std::string &src_folder_path, const std::string &file_name,
                       const std::string &dest_folder_path) override;

  VxCoreError CopyFile(const std::string &src_folder_path, const std::string &file_name,
                       const std::string &dest_folder_path, const std::string &new_name,
                       std::string &out_file_id) override;

  void ClearCache() override;

 private:
  void EnsureRootFolder();
  VxCoreError GetFolderConfig(const std::string &folder_path, FolderConfig **out_config);
  VxCoreError LoadFolderConfig(const std::string &folder_path,
                               std::unique_ptr<FolderConfig> &out_config);
  VxCoreError SaveFolderConfig(const std::string &folder_path, const FolderConfig &config);

  std::string GetConfigPath(const std::string &folder_path) const;
  std::string GetContentPath(const std::string &folder_path) const;

  void CacheConfig(const std::string &folder_path, std::unique_ptr<FolderConfig> config);
  FolderConfig *GetCachedConfig(const std::string &folder_path);
  void InvalidateCache(const std::string &folder_path);

  FileRecord *FindFileRecord(FolderConfig &config, const std::string &file_name);

  Notebook *notebook_;
  std::map<std::string, std::unique_ptr<FolderConfig>> config_cache_;
};

}  // namespace vxcore

#endif
