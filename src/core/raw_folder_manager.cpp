#include "raw_folder_manager.h"

#include "notebook.h"

namespace vxcore {

RawFolderManager::RawFolderManager(Notebook *notebook) : FolderManager(notebook) {
  assert(notebook && notebook->GetType() == NotebookType::Raw);
}

RawFolderManager::~RawFolderManager() {}

VxCoreError RawFolderManager::GetFolderConfig(const std::string &folder_path,
                                              std::string &out_config_json) {
  (void)folder_path;
  (void)out_config_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::CreateFolder(const std::string &parent_path,
                                           const std::string &folder_name,
                                           std::string &out_folder_id) {
  (void)parent_path;
  (void)folder_name;
  (void)out_folder_id;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::DeleteFolder(const std::string &folder_path) {
  (void)folder_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFolderMetadata(const std::string &folder_path,
                                                   const std::string &metadata_json) {
  (void)folder_path;
  (void)metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFolderMetadata(const std::string &folder_path,
                                                std::string &out_metadata_json) {
  (void)folder_path;
  (void)out_metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::RenameFolder(const std::string &folder_path,
                                           const std::string &new_name) {
  (void)folder_path;
  (void)new_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::MoveFolder(const std::string &src_path,
                                         const std::string &dest_parent_path) {
  (void)src_path;
  (void)dest_parent_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::CopyFolder(const std::string &src_path,
                                         const std::string &dest_parent_path,
                                         const std::string &new_name, std::string &out_folder_id) {
  (void)src_path;
  (void)dest_parent_path;
  (void)new_name;
  (void)out_folder_id;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::CreateFile(const std::string &folder_path,
                                         const std::string &file_name, std::string &out_file_id) {
  (void)folder_path;
  (void)file_name;
  (void)out_file_id;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::DeleteFile(const std::string &file_path) {
  (void)file_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFileMetadata(const std::string &file_path,
                                                 const std::string &metadata_json) {
  (void)file_path;
  (void)metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFileTags(const std::string &file_path,
                                             const std::string &tags_json) {
  (void)file_path;
  (void)tags_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::TagFile(const std::string &file_path, const std::string &tag_name) {
  (void)file_path;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UntagFile(const std::string &file_path, const std::string &tag_name) {
  (void)file_path;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileInfo(const std::string &file_path,
                                          std::string &out_file_info_json) {
  (void)file_path;
  (void)out_file_info_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileInfo(const std::string &file_path,
                                          const FileRecord **out_record) {
  (void)file_path;
  (void)out_record;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileMetadata(const std::string &file_path,
                                              std::string &out_metadata_json) {
  (void)file_path;
  (void)out_metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::RenameFile(const std::string &file_path,
                                         const std::string &new_name) {
  (void)file_path;
  (void)new_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::MoveFile(const std::string &file_path,
                                       const std::string &dest_folder_path) {
  (void)file_path;
  (void)dest_folder_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::CopyFile(const std::string &file_path,
                                       const std::string &dest_folder_path,
                                       const std::string &new_name, std::string &out_file_id) {
  (void)file_path;
  (void)dest_folder_path;
  (void)new_name;
  (void)out_file_id;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::ImportFile(const std::string &folder_path,
                                         const std::string &external_file_path,
                                         std::string &out_file_id) {
  (void)folder_path;
  (void)external_file_path;
  (void)out_file_id;
  return VXCORE_ERR_UNSUPPORTED;
}

void RawFolderManager::IterateAllFiles(
    std::function<bool(const std::string &, const FileRecord &)> callback) {
  (void)callback;
}

VxCoreError RawFolderManager::FindFilesByTag(const std::string &tag_name,
                                             std::string &out_files_json) {
  (void)tag_name;
  (void)out_files_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::ListFolderContents(const std::string &folder_path,
                                                 bool include_folders_info,
                                                 FolderContents &out_contents) {
  (void)folder_path;
  (void)include_folders_info;
  (void)out_contents;
  return VXCORE_ERR_UNSUPPORTED;
}

void RawFolderManager::ClearCache() {}

}  // namespace vxcore
