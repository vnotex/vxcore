#ifndef VXCORE_FOLDER_MANAGER_H
#define VXCORE_FOLDER_MANAGER_H

#include <map>
#include <memory>
#include <mutex>
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

  VxCoreError TrackFile(const std::string &folder_path, const std::string &file_name,
                        std::string &out_file_id);

  VxCoreError UntrackFile(const std::string &folder_path, const std::string &file_name);

  VxCoreError UpdateFileMetadata(const std::string &folder_path, const std::string &file_name,
                                 const std::string &metadata_json);

  VxCoreError UpdateFileTags(const std::string &folder_path, const std::string &file_name,
                             const std::string &tags_json);

  VxCoreError ListFolder(const std::string &folder_path, std::string &out_contents_json);

  void ClearCache();

 private:
  VxCoreError LoadFolderConfig(const std::string &folder_path, FolderConfig &config);
  VxCoreError SaveFolderConfig(const std::string &folder_path, const FolderConfig &config);
  std::string GetConfigPath(const std::string &folder_path) const;
  std::string GetContentPath(const std::string &folder_path) const;
  FolderConfig *GetCachedConfig(const std::string &folder_path);
  void CacheConfig(const std::string &folder_path, const FolderConfig &config);
  void InvalidateCache(const std::string &folder_path);
  FileRecord *FindFileRecord(FolderConfig &config, const std::string &file_name);

  Notebook *notebook_;
  std::map<std::string, std::unique_ptr<FolderConfig>> config_cache_;
  std::mutex mutex_;
};

}  // namespace vxcore

#endif
