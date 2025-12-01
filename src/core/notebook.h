#ifndef VXCORE_NOTEBOOK_H
#define VXCORE_NOTEBOOK_H

#include <nlohmann/json.hpp>
#include <string>

#include "vxcore/vxcore_types.h"

namespace vxcore {

enum class NotebookType { Bundled, Raw };

struct NotebookConfig {
  std::string id;
  std::string name;
  std::string description;
  std::string assets_folder;
  std::string attachments_folder;
  nlohmann::json metadata;

  NotebookConfig();

  static NotebookConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct NotebookRecord {
  std::string id;
  std::string root_folder;
  NotebookType type;
  int64_t last_opened_timestamp;
  NotebookConfig raw_config;

  NotebookRecord();

  static NotebookRecord FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

class Notebook {
 public:
  const std::string &GetId() const { return config_.id; }
  const std::string &GetRootFolder() const { return root_folder_; }
  NotebookType GetType() const { return type_; }
  std::string GetTypeStr() const { return type_ == NotebookType::Raw ? "raw" : "bundled"; }
  const NotebookConfig &GetConfig() const { return config_; }

  VxCoreError UpdateConfig(const NotebookConfig &config);

  std::string GetDbPath(const std::string &local_data_folder) const;
  std::string GetMetadataFolder() const;
  std::string GetConfigPath() const;

  static VxCoreError CreateBundledNotebook(const std::string &root_folder,
                                           const NotebookConfig *overridden_config,
                                           std::unique_ptr<Notebook> &out_notebook);

  static VxCoreError CreateRawNotebook(const std::string &root_folder, const NotebookConfig &config,
                                       std::unique_ptr<Notebook> &out_notebook);

  static VxCoreError FromBundledNotebook(const std::string &root_folder,
                                         std::unique_ptr<Notebook> &out_notebook);

 protected:
  Notebook(const std::string &root_folder, NotebookType type);

 private:
  VxCoreError LoadConfig();

  void EnsureId();

  std::string root_folder_;
  NotebookType type_;
  NotebookConfig config_;
};

}  // namespace vxcore

#endif
