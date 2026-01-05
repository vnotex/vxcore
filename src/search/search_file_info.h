#ifndef VXCORE_SEARCH_FILE_INFO_H
#define VXCORE_SEARCH_FILE_INFO_H

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vxcore {

struct FileRecord;
struct FolderRecord;

struct SearchFileInfo {
  std::string path;
  std::string absolute_path;
  std::string name;
  std::string id;
  std::vector<std::string> tags;
  int64_t created_utc;
  int64_t modified_utc;
  bool is_folder;

  nlohmann::json ToJson() const;

  static SearchFileInfo FromFileRecord(const std::string &file_path, const FileRecord &record);
  static SearchFileInfo FromFolderRecord(const std::string &folder_path,
                                         const FolderRecord &record);
};

}  // namespace vxcore

#endif
