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
};

}  // namespace vxcore

#endif
