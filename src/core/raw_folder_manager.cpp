#include "raw_folder_manager.h"

#include "notebook.h"

namespace vxcore {

RawFolderManager::RawFolderManager(Notebook *notebook) : notebook_(notebook) {
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

VxCoreError RawFolderManager::DeleteFile(const std::string &folder_path,
                                         const std::string &file_name) {
  (void)folder_path;
  (void)file_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFileMetadata(const std::string &folder_path,
                                                 const std::string &file_name,
                                                 const std::string &metadata_json) {
  (void)folder_path;
  (void)file_name;
  (void)metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFileTags(const std::string &folder_path,
                                             const std::string &file_name,
                                             const std::string &tags_json) {
  (void)folder_path;
  (void)file_name;
  (void)tags_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileInfo(const std::string &folder_path,
                                          const std::string &file_name,
                                          std::string &out_file_info_json) {
  (void)folder_path;
  (void)file_name;
  (void)out_file_info_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileMetadata(const std::string &folder_path,
                                              const std::string &file_name,
                                              std::string &out_metadata_json) {
  (void)folder_path;
  (void)file_name;
  (void)out_metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::RenameFile(const std::string &folder_path,
                                         const std::string &old_name, const std::string &new_name) {
  (void)folder_path;
  (void)old_name;
  (void)new_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::MoveFile(const std::string &src_folder_path,
                                       const std::string &file_name,
                                       const std::string &dest_folder_path) {
  (void)src_folder_path;
  (void)file_name;
  (void)dest_folder_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::CopyFile(const std::string &src_folder_path,
                                       const std::string &file_name,
                                       const std::string &dest_folder_path,
                                       const std::string &new_name, std::string &out_file_id) {
  (void)src_folder_path;
  (void)file_name;
  (void)dest_folder_path;
  (void)new_name;
  (void)out_file_id;
  return VXCORE_ERR_UNSUPPORTED;
}

void RawFolderManager::ClearCache() {}

}  // namespace vxcore
