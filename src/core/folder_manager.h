#ifndef VXCORE_FOLDER_MANAGER_H
#define VXCORE_FOLDER_MANAGER_H

#include <map>
#include <memory>
#include <string>

#include "folder.h"
#include "notebook.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class FolderManager {
 public:
  FolderManager(Notebook *notebook);
  ~FolderManager();

  VxCoreError GetFolderConfig(const std::string &folder_path, std::string &out_config_json);

  VxCoreError CreateFolder(const std::string &parent_path, const std::string &folder_name,
                           std::string &out_folder_id);

  VxCoreError DeleteFolder(const std::string &folder_path);

  VxCoreError UpdateFolderMetadata(const std::string &folder_path,
                                   const std::string &metadata_json);

  VxCoreError GetFolderMetadata(const std::string &folder_path, std::string &out_metadata_json);

  VxCoreError RenameFolder(const std::string &folder_path, const std::string &new_name);

  VxCoreError MoveFolder(const std::string &src_path, const std::string &dest_parent_path);

  VxCoreError CopyFolder(const std::string &src_path, const std::string &dest_parent_path,
                         const std::string &new_name, std::string &out_folder_id);

  VxCoreError CreateFile(const std::string &folder_path, const std::string &file_name,
                         std::string &out_file_id);

  VxCoreError DeleteFile(const std::string &folder_path, const std::string &file_name);

  VxCoreError UpdateFileMetadata(const std::string &folder_path, const std::string &file_name,
                                 const std::string &metadata_json);

  VxCoreError UpdateFileTags(const std::string &folder_path, const std::string &file_name,
                             const std::string &tags_json);

  VxCoreError GetFileInfo(const std::string &folder_path, const std::string &file_name,
                          std::string &out_file_info_json);

  VxCoreError GetFileMetadata(const std::string &folder_path, const std::string &file_name,
                              std::string &out_metadata_json);

  VxCoreError RenameFile(const std::string &folder_path, const std::string &old_name,
                         const std::string &new_name);

  VxCoreError MoveFile(const std::string &src_folder_path, const std::string &file_name,
                       const std::string &dest_folder_path);

  VxCoreError CopyFile(const std::string &src_folder_path, const std::string &file_name,
                       const std::string &dest_folder_path, const std::string &new_name,
                       std::string &out_file_id);

  void ClearCache();

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

  std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name) const;

  std::pair<std::string, std::string> SplitPath(const std::string &path) const;

  Notebook *notebook_;
  std::map<std::string, std::unique_ptr<FolderConfig>> config_cache_;
};

}  // namespace vxcore

#endif
