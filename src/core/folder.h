#ifndef VXCORE_FOLDER_H
#define VXCORE_FOLDER_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vxcore {

struct FileRecord {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  nlohmann::json metadata;
  std::vector<std::string> tags;

  FileRecord();
  FileRecord(const std::string &name);

  static FileRecord FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
  nlohmann::json ToJsonWithType() const;
};

struct FolderRecord {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  nlohmann::json metadata;

  FolderRecord();
  FolderRecord(const std::string &name);
  FolderRecord(const std::string &id, const std::string &name, int64_t created_utc,
               int64_t modified_utc, const nlohmann::json &metadata);
};

struct FolderConfig {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  nlohmann::json metadata;
  std::vector<FileRecord> files;
  std::vector<std::string> folders;

  FolderConfig();
  FolderConfig(const std::string &name);

  static FolderConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
  nlohmann::json ToJsonWithType() const;
};

}  // namespace vxcore

#endif
