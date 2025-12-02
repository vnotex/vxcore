#include "raw_notebook.h"

#include <filesystem>

#include "raw_folder_manager.h"
#include "utils/logger.h"

namespace vxcore {

RawNotebook::RawNotebook(const std::string &root_folder)
    : Notebook(root_folder, NotebookType::Raw) {
  folder_manager_ = std::make_unique<RawFolderManager>(this);
}

VxCoreError RawNotebook::Create(const std::string &root_folder, const NotebookConfig &config,
                                std::unique_ptr<Notebook> &out_notebook) {
  std::filesystem::path rootPath(root_folder);
  if (!std::filesystem::exists(rootPath)) {
    VXCORE_LOG_ERROR("Failed to create raw notebook: root=%s, error=%d", root_folder.c_str(),
                     VXCORE_ERR_NOT_FOUND);
    return VXCORE_ERR_NOT_FOUND;
  }

  auto notebook = std::unique_ptr<RawNotebook>(new RawNotebook(root_folder));
  notebook->config_ = config;
  notebook->EnsureId();
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

VxCoreError RawNotebook::UpdateConfig(const NotebookConfig &config) {
  assert(config_.id == config.id);
  config_ = config;
  return VXCORE_OK;
}

}  // namespace vxcore
