#include "search_file_info.h"

#include "core/folder.h"

namespace vxcore {

nlohmann::json SearchFileInfo::ToJson() const {
  nlohmann::json json;
  json["type"] = is_folder ? "folder" : "file";
  json["path"] = path;
  json["absolute_path"] = absolute_path;
  json["id"] = id;
  json["createdUtc"] = created_utc;
  json["modifiedUtc"] = modified_utc;
  json["tags"] = tags;
  return json;
}

SearchFileInfo SearchFileInfo::FromFileRecord(const std::string &file_path,
                                              const FileRecord &record) {
  SearchFileInfo info;
  info.path = file_path;
  info.name = record.name;
  info.id = record.id;
  info.tags = record.tags;
  info.created_utc = record.created_utc;
  info.modified_utc = record.modified_utc;
  info.is_folder = false;
  return info;
}

SearchFileInfo SearchFileInfo::FromFolderRecord(const std::string &folder_path,
                                                const FolderRecord &record) {
  SearchFileInfo info;
  info.path = folder_path;
  info.name = record.name;
  info.id = record.id;
  info.created_utc = record.created_utc;
  info.modified_utc = record.modified_utc;
  info.is_folder = true;
  return info;
}

}  // namespace vxcore
