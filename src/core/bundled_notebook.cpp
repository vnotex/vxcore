#include "bundled_notebook.h"

#include <filesystem>
#include <fstream>

#include "bundled_folder_manager.h"
#include "utils/logger.h"

namespace vxcore {

BundledNotebook::BundledNotebook(const std::string &root_folder)
    : Notebook(root_folder, NotebookType::Bundled) {
  folder_manager_ = std::make_unique<BundledFolderManager>(this);
}

VxCoreError BundledNotebook::Create(const std::string &root_folder,
                                    const NotebookConfig *overridden_config,
                                    std::unique_ptr<Notebook> &out_notebook) {
  auto notebook = std::unique_ptr<BundledNotebook>(new BundledNotebook(root_folder));
  if (overridden_config) {
    notebook->config_ = *overridden_config;
  }
  notebook->EnsureId();
  auto error = notebook->UpdateConfig(notebook->config_);
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save bundled notebook config: root=%s, error=%d",
                     root_folder.c_str(), error);
    return error;
  }
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

VxCoreError BundledNotebook::Open(const std::string &root_folder,
                                  std::unique_ptr<Notebook> &out_notebook) {
  auto notebook = std::unique_ptr<BundledNotebook>(new BundledNotebook(root_folder));
  auto err = notebook->LoadConfig();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook config: root=%s, error=%d",
                     root_folder.c_str(), err);
    return err;
  }
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

VxCoreError BundledNotebook::LoadConfig() {
  std::string configPath = GetConfigPath();
  std::ifstream file(configPath);
  if (!file.is_open()) {
    return VXCORE_ERR_IO;
  }

  try {
    nlohmann::json json;
    file >> json;
    auto config = NotebookConfig::FromJson(json);
    if (config.id.empty()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    config_ = config;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError BundledNotebook::UpdateConfig(const NotebookConfig &config) {
  assert(config_.id == config.id);

  config_ = config;

  try {
    std::filesystem::path metaPath(GetMetadataFolder());
    std::filesystem::create_directories(metaPath);

    std::string configPath = GetConfigPath();
    std::ofstream file(configPath);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    nlohmann::json json = config_.ToJson();
    file << json.dump(2);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

}  // namespace vxcore
