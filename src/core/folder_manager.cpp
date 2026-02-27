#include "folder_manager.h"

#include <filesystem>

namespace vxcore {

VxCoreError FolderManager::CreateFolderPath(const std::string &folder_path,
                                            std::string &out_folder_id) {
  if (folder_path.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::vector<std::string> path_components = SplitPathComponents(folder_path);
  if (path_components.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::string current_parent = ".";
  std::string folder_id;

  for (size_t i = 0; i < path_components.size(); ++i) {
    const std::string &folder_name = path_components[i];
    if (folder_name.empty()) {
      return VXCORE_ERR_INVALID_PARAM;
    }

    VxCoreError err = CreateFolder(current_parent, folder_name, folder_id);
    if (err != VXCORE_OK && err != VXCORE_ERR_ALREADY_EXISTS) {
      return err;
    }

    current_parent = ConcatenatePaths(current_parent, folder_name);
  }

  out_folder_id = folder_id;
  return VXCORE_OK;
}

std::string FolderManager::GetPublicAssetsFolder(const std::string &file_path) const {
  const NotebookConfig &config = notebook_->GetConfig();
  const std::string &assets_folder = config.assets_folder;

  // If absolute path, return as-is
  if (!IsRelativePath(assets_folder)) {
    return CleanPath(assets_folder);
  }

  // Get parent folder of the file
  auto [parent_path, _] = SplitPath(file_path);
  std::string abs_parent = notebook_->GetAbsolutePath(parent_path);

  // Resolve relative to file's parent folder
  return CleanFsPath(std::filesystem::path(abs_parent) / assets_folder);
}

std::string FolderManager::GetPublicAttachmentsFolder(const std::string &file_path) const {
  const NotebookConfig &config = notebook_->GetConfig();
  const std::string &attachments_folder = config.attachments_folder;

  // If absolute path, return as-is
  if (!IsRelativePath(attachments_folder)) {
    return CleanPath(attachments_folder);
  }

  // Get parent folder of the file
  auto [parent_path, _] = SplitPath(file_path);
  std::string abs_parent = notebook_->GetAbsolutePath(parent_path);

  // Resolve relative to file's parent folder
  return CleanFsPath(std::filesystem::path(abs_parent) / attachments_folder);
}

std::string FolderManager::GetAssetsFolder(const std::string &file_path) {
  // Get file's UUID
  const FileRecord *record = nullptr;
  VxCoreError err = GetFileInfo(file_path, &record);
  if (err != VXCORE_OK || !record) {
    return "";
  }

  std::string public_folder = GetPublicAssetsFolder(file_path);
  return ConcatenatePaths(public_folder, record->id);
}

std::string FolderManager::GetAttachmentsFolder(const std::string &file_path) {
  // Get file's UUID
  const FileRecord *record = nullptr;
  VxCoreError err = GetFileInfo(file_path, &record);
  if (err != VXCORE_OK || !record) {
    return "";
  }

  std::string public_folder = GetPublicAttachmentsFolder(file_path);
  return ConcatenatePaths(public_folder, record->id);
}

}  // namespace vxcore