#ifndef VXCORE_FOLDER_MANAGER_H
#define VXCORE_FOLDER_MANAGER_H

#include <functional>
#include <string>

#include "folder.h"
#include "notebook.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;

class FolderManager {
 public:
  explicit FolderManager(Notebook *notebook) : notebook_(notebook) {}

  virtual ~FolderManager() = default;

  virtual VxCoreError InitOnCreation() { return VXCORE_OK; }

  virtual VxCoreError GetFolderConfig(const std::string &folder_path,
                                      std::string &out_config_json) = 0;

  virtual VxCoreError CreateFolder(const std::string &parent_path, const std::string &folder_name,
                                   std::string &out_folder_id) = 0;

  virtual VxCoreError DeleteFolder(const std::string &folder_path) = 0;

  virtual VxCoreError UpdateFolderMetadata(const std::string &folder_path,
                                           const std::string &metadata_json) = 0;

  virtual VxCoreError GetFolderMetadata(const std::string &folder_path,
                                        std::string &out_metadata_json) = 0;

  virtual VxCoreError RenameFolder(const std::string &folder_path, const std::string &new_name) = 0;

  virtual VxCoreError MoveFolder(const std::string &src_path,
                                 const std::string &dest_parent_path) = 0;

  virtual VxCoreError CopyFolder(const std::string &src_path, const std::string &dest_parent_path,
                                 const std::string &new_name, std::string &out_folder_id) = 0;

  virtual VxCoreError CreateFile(const std::string &folder_path, const std::string &file_name,
                                 std::string &out_file_id) = 0;

  virtual VxCoreError DeleteFile(const std::string &file_path) = 0;

  virtual VxCoreError UpdateFileMetadata(const std::string &file_path,
                                         const std::string &metadata_json) = 0;

  virtual VxCoreError UpdateFileTags(const std::string &file_path,
                                     const std::string &tags_json) = 0;

  virtual VxCoreError TagFile(const std::string &file_path, const std::string &tag_name) = 0;

  virtual VxCoreError UntagFile(const std::string &file_path, const std::string &tag_name) = 0;

  virtual VxCoreError GetFileInfo(const std::string &file_path,
                                  std::string &out_file_info_json) = 0;

  virtual VxCoreError GetFileInfo(const std::string &file_path, const FileRecord **out_record) = 0;

  virtual VxCoreError GetFileMetadata(const std::string &file_path,
                                      std::string &out_metadata_json) = 0;

  virtual VxCoreError RenameFile(const std::string &file_path, const std::string &new_name) = 0;

  virtual VxCoreError MoveFile(const std::string &src_file_path,
                               const std::string &dest_folder_path) = 0;

  virtual VxCoreError CopyFile(const std::string &src_file_path,
                               const std::string &dest_folder_path, const std::string &new_name,
                               std::string &out_file_id) = 0;

  virtual VxCoreError ImportFile(const std::string &folder_path,
                                 const std::string &external_file_path,
                                 std::string &out_file_id) = 0;

  virtual void IterateAllFiles(
      std::function<bool(const std::string &, const FileRecord &)> callback) = 0;

  virtual VxCoreError FindFilesByTag(const std::string &tag_name, std::string &out_files_json) = 0;

  struct FolderContents {
    std::vector<FileRecord> files;
    std::vector<FolderRecord> folders;
  };

  virtual VxCoreError ListFolderContents(const std::string &folder_path, bool include_folders_info,
                                         FolderContents &out_contents) = 0;

  virtual void ClearCache() = 0;

 protected:
  inline std::string GetCleanRelativePath(const std::string &path) const {
    return notebook_->GetCleanRelativePath(path);
  }

  Notebook *notebook_ = nullptr;
};

}  // namespace vxcore

#endif
