#ifndef VXCORE_NOTEBOOK_H
#define VXCORE_NOTEBOOK_H

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class FolderManager;

enum class NotebookType { Bundled, Raw };

struct TagNode {
  std::string name;
  std::string parent;
  nlohmann::json metadata;

  TagNode();
  TagNode(const std::string &name, const std::string &parent = std::string());

  static TagNode FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct NotebookConfig {
  std::string id;
  std::string name;
  std::string description;
  std::string assets_folder;
  std::string attachments_folder;
  nlohmann::json metadata;
  std::vector<TagNode> tags;

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
  virtual ~Notebook() = default;

  const std::string &GetId() const { return config_.id; }
  const std::string &GetRootFolder() const { return root_folder_; }
  NotebookType GetType() const { return type_; }
  std::string GetTypeStr() const { return type_ == NotebookType::Raw ? "raw" : "bundled"; }
  const NotebookConfig &GetConfig() const { return config_; }

  virtual VxCoreError UpdateConfig(const NotebookConfig &config) = 0;

  std::string GetDbPath(const std::string &local_data_folder) const;
  std::string GetMetadataFolder() const;
  std::string GetConfigPath() const;

  FolderManager *GetFolderManager() { return folder_manager_.get(); }

  VxCoreError CreateTag(const std::string &tag_name, const std::string &parent_tag = "");
  VxCoreError DeleteTag(const std::string &tag_name);
  VxCoreError GetTags(std::string &out_tags_json) const;
  TagNode *FindTag(const std::string &tag_name);

 protected:
  Notebook(const std::string &root_folder, NotebookType type);

  void EnsureId();

  std::string root_folder_;
  NotebookType type_;
  NotebookConfig config_;
  std::unique_ptr<FolderManager> folder_manager_;
};

}  // namespace vxcore

#endif
