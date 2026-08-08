#include "folder.h"

#include <chrono>

#include "utils/utils.h"
#include "vxcore/notebook_json_keys.h"

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
  if (json.contains(kJsonKeyId) && json[kJsonKeyId].is_string()) {
    record.id = json[kJsonKeyId].get<std::string>();
  }
  if (json.contains(kJsonKeyName) && json[kJsonKeyName].is_string()) {
    record.name = json[kJsonKeyName].get<std::string>();
  }
  if (json.contains(kJsonKeyCreatedUtc) && json[kJsonKeyCreatedUtc].is_number()) {
    record.created_utc = json[kJsonKeyCreatedUtc].get<int64_t>();
  }
  if (json.contains(kJsonKeyModifiedUtc) && json[kJsonKeyModifiedUtc].is_number()) {
    record.modified_utc = json[kJsonKeyModifiedUtc].get<int64_t>();
  }
  if (json.contains(kJsonKeyMetadata) && json[kJsonKeyMetadata].is_object()) {
    record.metadata = json[kJsonKeyMetadata];
  } else {
    record.metadata = nlohmann::json::object();
  }
  if (json.contains(kJsonKeyTags) && json[kJsonKeyTags].is_array()) {
    record.tags = json[kJsonKeyTags].get<std::vector<std::string>>();
  }
  if (json.contains(kJsonKeyAttachments) && json[kJsonKeyAttachments].is_array()) {
    record.attachments = json[kJsonKeyAttachments].get<std::vector<std::string>>();
  }
  return record;
}

nlohmann::json FileRecord::ToJson() const {
  nlohmann::json json;
  json[kJsonKeyId] = id;
  json[kJsonKeyName] = name;
  json[kJsonKeyCreatedUtc] = created_utc;
  json[kJsonKeyModifiedUtc] = modified_utc;
  json[kJsonKeyMetadata] = metadata;
  json[kJsonKeyTags] = tags;
  if (!attachments.empty()) {
    json[kJsonKeyAttachments] = attachments;
  }
  return json;
}

nlohmann::json FileRecord::ToJsonWithType() const {
  nlohmann::json json = ToJson();
  json[kJsonKeyType] = "file";
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
  json[kJsonKeyId] = id;
  json[kJsonKeyName] = name;
  json[kJsonKeyCreatedUtc] = created_utc;
  json[kJsonKeyModifiedUtc] = modified_utc;
  json[kJsonKeyMetadata] = metadata;
  json[kJsonKeyType] = "folder";
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
  if (json.contains(kJsonKeyId) && json[kJsonKeyId].is_string()) {
    config.id = json[kJsonKeyId].get<std::string>();
  }
  if (json.contains(kJsonKeyName) && json[kJsonKeyName].is_string()) {
    config.name = json[kJsonKeyName].get<std::string>();
  }
  if (json.contains(kJsonKeyCreatedUtc) && json[kJsonKeyCreatedUtc].is_number()) {
    config.created_utc = json[kJsonKeyCreatedUtc].get<int64_t>();
  }
  if (json.contains(kJsonKeyModifiedUtc) && json[kJsonKeyModifiedUtc].is_number()) {
    config.modified_utc = json[kJsonKeyModifiedUtc].get<int64_t>();
  }
  if (json.contains(kJsonKeyMetadata) && json[kJsonKeyMetadata].is_object()) {
    config.metadata = json[kJsonKeyMetadata];
  } else {
    config.metadata = nlohmann::json::object();
  }
  if (json.contains(kJsonKeyFiles) && json[kJsonKeyFiles].is_array()) {
    for (const auto &file_json : json[kJsonKeyFiles]) {
      config.files.push_back(FileRecord::FromJson(file_json));
    }
  }
  if (json.contains(kJsonKeyFolders) && json[kJsonKeyFolders].is_array()) {
    config.folders = json[kJsonKeyFolders].get<std::vector<std::string>>();
  }
  return config;
}

nlohmann::json FolderConfig::ToJson() const {
  nlohmann::json json;
  json[kJsonKeyId] = id;
  json[kJsonKeyName] = name;
  json[kJsonKeyCreatedUtc] = created_utc;
  json[kJsonKeyModifiedUtc] = modified_utc;
  json[kJsonKeyMetadata] = metadata;

  nlohmann::json files_json = nlohmann::json::array();
  for (const auto &file : files) {
    files_json.push_back(file.ToJson());
  }
  json[kJsonKeyFiles] = files_json;

  json[kJsonKeyFolders] = folders;
  return json;
}

nlohmann::json FolderConfig::ToJsonWithType() const {
  nlohmann::json json = ToJson();
  json[kJsonKeyType] = "folder";
  return json;
}

}  // namespace vxcore
