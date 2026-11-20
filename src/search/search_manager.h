#ifndef VXCORE_SEARCH_MANAGER_H
#define VXCORE_SEARCH_MANAGER_H

#include <functional>
#include <string>
#include <vector>

#include "search_file_info.h"
#include "search_query.h"
#include "simple_search_backend.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;
struct FileTypesConfig;
class WorkQueue;

struct FileRecord;
struct FolderRecord;

struct FileSearchResult {
  std::vector<SearchFileInfo> files;
  bool truncated = false;
};

struct ManagedContentSearchResult {
  ContentSearchResult content;
  size_t encrypted_skipped_count = 0;
};

class SearchManager {
 public:
  explicit SearchManager(Notebook *notebook, const FileTypesConfig *file_types = nullptr,
                         std::vector<std::string> search_encodings = {});
  ~SearchManager();

  VxCoreError SearchFiles(const std::string &query_json, const std::string &input_files_json,
                          FileSearchResult &out_result);

  VxCoreError SearchContent(const std::string &query_json, const std::string &input_files_json,
                            ManagedContentSearchResult &out_result);

  // Native streaming sink, before representation-specific projection/serialization.
  // The files are borrowed for the callback duration; callbacks may overlap and arrive
  // out of order. No cap is applied. Skipped count is reported only by batch zero.
  using SearchContentBatchFn = std::function<void(
      int batch_index, int total_batches, const std::vector<ContentSearchMatchedFile> &files,
      size_t encrypted_skipped_count)>;
  VxCoreError SearchContentStreaming(const std::string &query_json,
                                     const std::string &input_files_json, int batch_size,
                                     const SearchContentBatchFn &on_batch);

  VxCoreError SearchByTags(const std::string &query_json, const std::string &input_files_json,
                           FileSearchResult &out_result);

  void SetWorkQueue(WorkQueue *queue);
  void SetCancelFlag(const volatile int *flag);

 private:
  std::vector<SearchFileInfo> GetAllFiles(const SearchScope &scope,
                                          const SearchInputFiles *input_files, bool include_folders,
                                          size_t *encrypted_skipped);

  std::vector<SearchFileInfo> FilterFilesByTagsAndDate(std::vector<SearchFileInfo> files,
                                                       const SearchScope &scope);

  std::vector<SearchFileInfo> FetchFilesToSearch(const SearchScope &scope,
                                                 const std::string &input_files_json,
                                                 bool include_folders,
                                                 size_t *encrypted_skipped = nullptr);
  void FilterDefaultContentFileTypes(std::vector<SearchFileInfo> &files,
                                     const SearchScope &scope) const;

  std::vector<SearchFileInfo> GetMatchedFilesByPattern(std::vector<SearchFileInfo> filtered_files,
                                                       const std::string &pattern,
                                                       bool include_files, bool include_folders,
                                                       SearchFileMatchTarget match_target,
                                                       int max_results);

  std::vector<SearchFileInfo> GetMatchedFilesByTags(std::vector<SearchFileInfo> filtered_files,
                                                    const std::vector<std::string> &tags,
                                                    const std::string &tag_operator,
                                                    int max_results);

  void CollectFilesInFolder(const std::string &folder_path, const SearchScope &scope,
                            const std::vector<std::string> &lower_path_patterns,
                            const std::vector<std::string> &lower_exclude_path_patterns,
                            bool include_folders, std::vector<SearchFileInfo> &out_files,
                            size_t *encrypted_skipped);

  bool SkipEncryptedCandidate(const std::string &path, const FileRecord &record,
                              const SearchScope &scope, size_t &encrypted_skipped) const;

  bool MatchesTags(const std::vector<std::string> &file_tags,
                   const std::vector<std::string> &search_tags,
                   const std::string &tag_operator) const;

  bool MatchesDateFilter(int64_t timestamp, const SearchScope &scope) const;

  void CalculateAbsolutePaths(std::vector<SearchFileInfo> &files) const;

  Notebook *notebook_;
  SimpleSearchBackend search_backend_;
  const FileTypesConfig *file_types_ = nullptr;
  const volatile int *cancel_flag_ = nullptr;
};

}  // namespace vxcore

#endif
