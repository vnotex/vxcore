#include "folder.h"

#include <chrono>

#include "utils/utils.h"

namespace vxcore {

FileRecord::FileRecord() : created_utc(0), modified_utc(0) {}

FileRecord::FileRecord(const std::string &name)
    : id(GenerateUUID()), name(name), metadata(nlohmann::json::object()) {
  auto now = GetCurrentTimestampMillis();
  created_utc = now;
  modified_utc = now;
}

FileRecord FileRecord::FromJson(const nlohmann::json &json) {
  FileRecord record;
  if (json.contains("id") && json["id"].is_string()) {
    record.id = json["id"].get<std::string>();
  }
  if (json.contains("name") && json["name"].is_string()) {
    record.name = json["name"].get<std::string>();
  }
  if (json.contains("createdUtc") && json["createdUtc"].is_number()) {
    record.created_utc = json["createdUtc"].get<int64_t>();
  }
  if (json.contains("modifiedUtc") && json["modifiedUtc"].is_number()) {
    record.modified_utc = json["modifiedUtc"].get<int64_t>();
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    record.metadata = json["metadata"];
  } else {
    record.metadata = nlohmann::json::object();
  }
  if (json.contains("tags") && json["tags"].is_array()) {
    record.tags = json["tags"].get<std::vector<std::string>>();
  }
  return record;
}

nlohmann::json FileRecord::ToJson() const {
  nlohmann::json json;
  json["id"] = id;
  json["name"] = name;
  json["createdUtc"] = created_utc;
  json["modifiedUtc"] = modified_utc;
  json["metadata"] = metadata;
  json["tags"] = tags;
  return json;
}

nlohmann::json FileRecord::ToJsonWithType() const {
  nlohmann::json json = ToJson();
  json["type"] = "file";
  return json;
}

FolderRecord::FolderRecord()
    : created_utc(0), modified_utc(0), metadata(nlohmann::json::object()) {}

FolderRecord::FolderRecord(const std::string &name)
    : name(name), created_utc(0), modified_utc(0), metadata(nlohmann::json::object()) {}

FolderRecord::FolderRecord(const std::string &id, const std::string &name, int64_t created_utc,
                           int64_t modified_utc, const nlohmann::json &metadata)
    : id(id),
      name(name),
      created_utc(created_utc),
      modified_utc(modified_utc),
      metadata(metadata) {}

nlohmann::json FolderRecord::ToJson() const {
  nlohmann::json json;
  json["id"] = id;
  json["name"] = name;
  json["createdUtc"] = created_utc;
  json["modifiedUtc"] = modified_utc;
  json["metadata"] = metadata;
  json["type"] = "folder";
  return json;
}

FolderConfig::FolderConfig() : created_utc(0), modified_utc(0) {}

FolderConfig::FolderConfig(const std::string &name)
    : id(GenerateUUID()), name(name), metadata(nlohmann::json::object()) {
  auto now = GetCurrentTimestampMillis();
  created_utc = now;
  modified_utc = now;
}

FolderConfig FolderConfig::FromJson(const nlohmann::json &json) {
  FolderConfig config;
  if (json.contains("id") && json["id"].is_string()) {
    config.id = json["id"].get<std::string>();
  }
  if (json.contains("name") && json["name"].is_string()) {
    config.name = json["name"].get<std::string>();
  }
  if (json.contains("createdUtc") && json["createdUtc"].is_number()) {
    config.created_utc = json["createdUtc"].get<int64_t>();
  }
  if (json.contains("modifiedUtc") && json["modifiedUtc"].is_number()) {
    config.modified_utc = json["modifiedUtc"].get<int64_t>();
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    config.metadata = json["metadata"];
  } else {
    config.metadata = nlohmann::json::object();
  }
  if (json.contains("files") && json["files"].is_array()) {
    for (const auto &file_json : json["files"]) {
      config.files.push_back(FileRecord::FromJson(file_json));
    }
  }
  if (json.contains("folders") && json["folders"].is_array()) {
    config.folders = json["folders"].get<std::vector<std::string>>();
  }
  return config;
}

nlohmann::json FolderConfig::ToJson() const {
  nlohmann::json json;
  json["id"] = id;
  json["name"] = name;
  json["createdUtc"] = created_utc;
  json["modifiedUtc"] = modified_utc;
  json["metadata"] = metadata;

  nlohmann::json files_json = nlohmann::json::array();
  for (const auto &file : files) {
    files_json.push_back(file.ToJson());
  }
  json["files"] = files_json;

  json["folders"] = folders;
  return json;
}

nlohmann::json FolderConfig::ToJsonWithType() const {
  nlohmann::json json = ToJson();
  json["type"] = "folder";
  return json;
}

}  // namespace vxcore
