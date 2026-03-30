#ifndef VXCORE_TEMPLATE_MANAGER_H
#define VXCORE_TEMPLATE_MANAGER_H

#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;

class TemplateManager {
 public:
  explicit TemplateManager(ConfigManager *config_manager);

  VxCoreError GetTemplateFolderPath(std::string &out_path) const;
  VxCoreError ListTemplates(std::vector<std::string> &out_names) const;
  VxCoreError ListTemplatesBySuffix(const std::string &suffix,
                                    std::vector<std::string> &out_names) const;
  VxCoreError GetTemplateContent(const std::string &name, std::string &out_content) const;
  VxCoreError CreateTemplate(const std::string &name, const std::string &content);
  VxCoreError DeleteTemplate(const std::string &name);
  VxCoreError RenameTemplate(const std::string &old_name, const std::string &new_name);

 private:
  VxCoreError EnsureTemplateFolderExists() const;
  bool IsValidTemplateName(const std::string &name) const;

  ConfigManager *config_manager_ = nullptr;
  std::string template_folder_path_;
};

}  // namespace vxcore

#endif
