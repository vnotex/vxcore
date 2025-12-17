#ifndef VXCORE_SEARCH_MANAGER_H
#define VXCORE_SEARCH_MANAGER_H

#include <memory>
#include <string>
#include <vector>

#include "search_backend.h"
#include "search_query.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;

struct FileRecord;
struct FolderRecord;

class SearchManager {
 public:
  explicit SearchManager(Notebook *notebook);
  ~SearchManager();

  VxCoreError SearchFiles(const std::string &query_json, const std::string &input_files_json,
                          std::string &out_results_json);

  VxCoreError SearchContent(const std::string &query_json, const std::string &input_files_json,
                            std::string &out_results_json);

  VxCoreError SearchByTags(const std::string &query_json, const std::string &input_files_json,
                           std::string &out_results_json);

 private:
  struct FileInfo {
    std::string path;
    std::string name;
    std::string id;
    std::vector<std::string> tags;
    int64_t created_utc;
    int64_t modified_utc;
    bool is_folder;

    nlohmann::json ToJson() const;
  };

  std::vector<FileInfo> GetAllFiles(const SearchScope &scope, const SearchInputFiles *input_files,
                                    bool include_folders);

  std::vector<FileInfo> FilterFilesByTagsAndDate(std::vector<FileInfo> files,
                                                 const SearchScope &scope);

  std::vector<FileInfo> GetMatchedFilesByPattern(std::vector<FileInfo> filtered_files,
                                                 const std::string &pattern, bool include_files,
                                                 bool include_folders, int max_results);

  std::vector<FileInfo> GetMatchedFilesByTags(std::vector<FileInfo> filtered_files,
                                              const std::vector<std::string> &tags,
                                              const std::string &tag_operator, int max_results);

  SearchInputFiles ParseInputFiles(const std::string &input_files_json);

  std::string SerializeFileResults(const std::vector<FileInfo> &matched_files, int max_results);

  void CollectFilesInFolder(const std::string &folder_path, bool recursive,
                            const std::vector<std::string> &file_patterns,
                            const std::vector<std::string> &exclude_patterns, bool include_folders,
                            std::vector<FileInfo> &out_files);

  bool MatchesPattern(const std::string &text, const std::string &pattern) const;

  bool MatchesPatterns(const std::string &text, const std::vector<std::string> &patterns) const;

  bool MatchesTags(const std::vector<std::string> &file_tags,
                   const std::vector<std::string> &search_tags,
                   const std::string &tag_operator) const;

  bool MatchesDateFilter(int64_t timestamp, const SearchScope &scope) const;

  static FileInfo ToFileInfo(const std::string &file_path, const FileRecord &record);

  static FileInfo ToFileInfo(const std::string &file_path, const FolderRecord &record);

  Notebook *notebook_;
  std::unique_ptr<ISearchBackend> search_backend_;
};

}  // namespace vxcore

#endif
