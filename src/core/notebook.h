#ifndef VXCORE_NOTEBOOK_H
#define VXCORE_NOTEBOOK_H

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class FolderManager;
class MetadataStore;

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
  std::string GetLocalDataFolder() const;
  virtual std::string GetMetadataFolder() const = 0;

  FolderManager *GetFolderManager() { return folder_manager_.get(); }
  MetadataStore *GetMetadataStore() { return metadata_store_.get(); }

  // Closes the notebook, releasing all resources (DB connections, etc.).
  // Must be called before deleting the notebook's local data folder.
  void Close();

  VxCoreError CreateFolderPath(const std::string &folder_path, std::string &out_folder_id);

  VxCoreError CreateTag(const std::string &tag_name, const std::string &parent_tag = "");
  VxCoreError CreateTagPath(const std::string &tag_path);
  VxCoreError DeleteTag(const std::string &tag_name);
  VxCoreError MoveTag(const std::string &tag_name, const std::string &parent_tag);
  VxCoreError GetTags(std::string &out_tags_json) const;
  TagNode *FindTag(const std::string &tag_name);

  // Rebuild the metadata cache from ground truth (config files).
  // Returns VXCORE_OK on success.
  virtual VxCoreError RebuildCache() = 0;

  // Clean and get path related to notebook root folder.
  // Returns null string if |path| is not under notebook root folder.
  std::string GetCleanRelativePath(const std::string &path) const;

  std::string GetAbsolutePath(const std::string &relative_path) const;

 protected:
  Notebook(const std::string &local_data_folder, const std::string &root_folder, NotebookType type);

  void EnsureId();

  // Initialize and open the MetadataStore for this notebook
  // Returns VXCORE_OK on success, or error code on failure
  VxCoreError InitMetadataStore();

  std::string GetDbPath() const;
  virtual std::string GetConfigPath() const = 0;

  const std::string local_data_folder_;
  const std::string root_folder_;
  const NotebookType type_;

  NotebookConfig config_;
  std::unique_ptr<FolderManager> folder_manager_;
  std::unique_ptr<MetadataStore> metadata_store_;

  static const char *kConfigFileName;
};

}  // namespace vxcore

#endif
