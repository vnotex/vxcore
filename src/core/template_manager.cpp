#include "core/template_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "core/config_manager.h"
#include "utils/file_utils.h"

namespace vxcore {

TemplateManager::TemplateManager(ConfigManager *config_manager)
    : config_manager_(config_manager),
      template_folder_path_(config_manager->GetAppDataPath() + "/templates") {}

VxCoreError TemplateManager::EnsureTemplateFolderExists() const {
  std::error_code ec;
  std::filesystem::create_directories(template_folder_path_, ec);
  if (ec) {
    return VXCORE_ERR_IO;
  }
  return VXCORE_OK;
}

bool TemplateManager::IsValidTemplateName(const std::string &name) const {
  if (name.empty()) return false;
  if (name == "." || name == "..") return false;
  if (!IsSingleName(name)) return false;
  return true;
}

VxCoreError TemplateManager::GetTemplateFolderPath(std::string &out_path) const {
  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;
  out_path = template_folder_path_;
  return VXCORE_OK;
}

VxCoreError TemplateManager::ListTemplates(std::vector<std::string> &out_names) const {
  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;

  out_names.clear();

  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(template_folder_path_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string filename = entry.path().filename().string();
    if (!filename.empty() && filename[0] == '.') continue;
    out_names.push_back(filename);
  }

  return VXCORE_OK;
}

VxCoreError TemplateManager::ListTemplatesBySuffix(const std::string &suffix,
                                                   std::vector<std::string> &out_names) const {
  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;

  out_names.clear();

  std::string suffix_lower = suffix;
  std::transform(suffix_lower.begin(), suffix_lower.end(), suffix_lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(template_folder_path_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::string filename = entry.path().filename().string();
    if (!filename.empty() && filename[0] == '.') continue;

    std::string entry_ext = entry.path().extension().string();
    std::transform(entry_ext.begin(), entry_ext.end(), entry_ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (entry_ext == suffix_lower) {
      out_names.push_back(filename);
    }
  }

  return VXCORE_OK;
}

VxCoreError TemplateManager::GetTemplateContent(const std::string &name,
                                                std::string &out_content) const {
  if (!IsValidTemplateName(name)) return VXCORE_ERR_INVALID_PARAM;

  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;

  std::string full_path = template_folder_path_ + "/" + name;

  std::error_code ec;
  bool exists = std::filesystem::exists(full_path, ec);
  if (!exists) return VXCORE_ERR_NOT_FOUND;

  return ReadFile(full_path, out_content);
}

VxCoreError TemplateManager::CreateTemplate(const std::string &name, const std::string &content) {
  if (!IsValidTemplateName(name)) return VXCORE_ERR_INVALID_PARAM;

  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;

  std::string full_path = template_folder_path_ + "/" + name;

  std::error_code ec;
  bool exists = std::filesystem::exists(full_path, ec);
  if (exists) return VXCORE_ERR_ALREADY_EXISTS;

  return WriteFile(full_path, content);
}

VxCoreError TemplateManager::DeleteTemplate(const std::string &name) {
  if (!IsValidTemplateName(name)) return VXCORE_ERR_INVALID_PARAM;

  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;

  std::string full_path = template_folder_path_ + "/" + name;

  std::error_code ec;
  bool exists = std::filesystem::exists(full_path, ec);
  if (!exists) return VXCORE_ERR_NOT_FOUND;

  std::filesystem::remove(full_path, ec);
  if (ec) return VXCORE_ERR_IO;

  return VXCORE_OK;
}

VxCoreError TemplateManager::RenameTemplate(const std::string &old_name,
                                            const std::string &new_name) {
  if (!IsValidTemplateName(old_name) || !IsValidTemplateName(new_name))
    return VXCORE_ERR_INVALID_PARAM;

  VxCoreError err = EnsureTemplateFolderExists();
  if (err != VXCORE_OK) return err;

  std::string old_path = template_folder_path_ + "/" + old_name;
  std::string new_path = template_folder_path_ + "/" + new_name;

  std::error_code ec;
  bool old_exists = std::filesystem::exists(old_path, ec);
  if (!old_exists) return VXCORE_ERR_NOT_FOUND;

  if (old_name == new_name) return VXCORE_OK;

  bool new_exists = std::filesystem::exists(new_path, ec);
  if (new_exists) return VXCORE_ERR_ALREADY_EXISTS;

  std::filesystem::rename(old_path, new_path, ec);
  if (ec) return VXCORE_ERR_IO;

  return VXCORE_OK;
}

}  // namespace vxcore
