#ifndef VXCORE_NOTEBOOK_H
#define VXCORE_NOTEBOOK_H

#include "vxcore/vxcore_types.h"

#include <nlohmann/json.hpp>
#include <string>

namespace vxcore {

enum class NotebookType { Bundled, Raw };

struct NotebookConfig {
  std::string id;
  std::string name;
  std::string description;
  std::string assetsFolder;
  std::string attachmentsFolder;
  nlohmann::json metadata;

  NotebookConfig();

  static NotebookConfig fromJson(const nlohmann::json &json);
  nlohmann::json toJson() const;
};

struct NotebookRecord {
  std::string id;
  std::string rootFolder;
  NotebookType type;
  int64_t lastOpenedTimestamp;
  NotebookConfig rawConfig;

  NotebookRecord();

  static NotebookRecord fromJson(const nlohmann::json &json);
  nlohmann::json toJson() const;
};

class Notebook {
public:
  Notebook(const std::string &rootFolder, NotebookType type, const NotebookConfig &config);

  const std::string &getId() const { return config_.id; }
  const std::string &getRootFolder() const { return rootFolder_; }
  NotebookType getType() const { return type_; }
  const NotebookConfig &getConfig() const { return config_; }

  void setConfig(const NotebookConfig &config);

  std::string getDbPath(const std::string &localDataFolder) const;
  std::string getMetadataFolder() const;
  std::string getConfigPath() const;

  VxCoreError loadConfig();
  VxCoreError saveConfig();

private:
  std::string rootFolder_;
  NotebookType type_;
  NotebookConfig config_;
};

} // namespace vxcore

#endif
