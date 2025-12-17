#include "search_manager.h"

#include <algorithm>

#include "core/folder_manager.h"
#include "core/notebook.h"
#include "utils/logger.h"

namespace vxcore {

SearchManager::SearchManager(Notebook *notebook) : notebook_(notebook), search_backend_(nullptr) {}

SearchManager::~SearchManager() = default;

VxCoreError SearchManager::SearchFiles(const std::string &query_json,
                                       const std::string &input_files_json,
                                       std::string &out_results_json) {
  try {
    auto query = SearchFilesQuery::FromJson(notebook_, nlohmann::json::parse(query_json));
    auto input_files = ParseInputFiles(input_files_json);
    auto all_files = GetAllFiles(query.scope, &input_files, true);
    auto filtered_files = FilterFilesByTagsAndDate(std::move(all_files), query.scope);

    auto matched_files =
        GetMatchedFilesByPattern(std::move(filtered_files), query.pattern, query.include_files,
                                 query.include_folders, query.max_results);

    out_results_json = SerializeFileResults(matched_files, query.max_results);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("SearchFiles JSON error: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("SearchFiles error: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError SearchManager::SearchContent(const std::string &query_json,
                                         const std::string &input_files_json,
                                         std::string &out_results_json) {
  try {
    (void)input_files_json;
    auto query_data = nlohmann::json::parse(query_json);
    auto query = SearchContentQuery::FromJson(notebook_, query_data);

    nlohmann::json result;
    result["totalResults"] = 0;
    result["truncated"] = false;
    result["results"] = nlohmann::json::array();

    if (search_backend_) {
      std::vector<ContentSearchResult> search_results;
      std::string root = notebook_->GetRootFolder();

      if (search_backend_->Search(root, query.pattern, query.case_sensitive, query.whole_word,
                                  query.regex, query.scope.file_patterns,
                                  query.scope.exclude_patterns, search_results)) {
        for (const auto &sr : search_results) {
          nlohmann::json item;
          item["filePath"] = sr.file_path;
          item["fileId"] = "";
          item["matchCount"] = sr.matches.size();
          item["matches"] = nlohmann::json::array();

          for (const auto &match : sr.matches) {
            nlohmann::json m;
            m["lineNumber"] = match.line_number;
            m["columnStart"] = match.column_start;
            m["columnEnd"] = match.column_end;
            m["lineText"] = match.line_text;
            m["matchText"] = match.match_text;
            item["matches"].push_back(m);
          }

          result["results"].push_back(item);
          if (static_cast<int>(result["results"].size()) >= query.max_results) {
            result["truncated"] = true;
            break;
          }
        }
      }

      result["totalResults"] = result["results"].size();
    }

    out_results_json = result.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("SearchContent JSON error: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("SearchContent error: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError SearchManager::SearchByTags(const std::string &query_json,
                                        const std::string &input_files_json,
                                        std::string &out_results_json) {
  try {
    auto query_data = nlohmann::json::parse(query_json);
    auto query = SearchByTagsQuery::FromJson(notebook_, query_data);

    auto input_files = ParseInputFiles(input_files_json);
    auto all_files = GetAllFiles(query.scope, &input_files, false);
    auto filtered_files = FilterFilesByTagsAndDate(std::move(all_files), query.scope);

    auto matched_files = GetMatchedFilesByTags(std::move(filtered_files), query.tags,
                                               query.tag_operator, query.max_results);

    out_results_json = SerializeFileResults(matched_files, query.max_results);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("SearchByTags JSON error: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("SearchByTags error: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

std::vector<SearchManager::FileInfo> SearchManager::GetMatchedFilesByPattern(
    std::vector<FileInfo> filtered_files, const std::string &pattern, bool include_files,
    bool include_folders, int max_results) {
  if (pattern.empty()) {
    std::vector<FileInfo> matched_files;
    for (auto &file : filtered_files) {
      if ((file.is_folder && !include_folders) || (!file.is_folder && !include_files)) {
        continue;
      }
      matched_files.push_back(std::move(file));
      if (static_cast<int>(matched_files.size()) >= max_results) {
        break;
      }
    }
    return matched_files;
  }

  std::vector<FileInfo> name_matches;
  std::vector<FileInfo> path_matches;

  for (auto &file : filtered_files) {
    if ((file.is_folder && !include_folders) || (!file.is_folder && !include_files)) {
      continue;
    }

    bool name_match = MatchesPattern(file.name, pattern);
    bool path_match = !name_match && MatchesPattern(file.path, pattern);

    if (name_match) {
      name_matches.push_back(std::move(file));
    } else if (path_match) {
      path_matches.push_back(std::move(file));
    }

    if (static_cast<int>(name_matches.size() + path_matches.size()) >= max_results) {
      break;
    }
  }

  std::vector<FileInfo> matched_files;
  matched_files.reserve(
      std::min(static_cast<size_t>(max_results), name_matches.size() + path_matches.size()));
  for (auto &file : name_matches) {
    if (static_cast<int>(matched_files.size()) >= max_results) {
      break;
    }
    matched_files.push_back(std::move(file));
  }
  for (auto &file : path_matches) {
    if (static_cast<int>(matched_files.size()) >= max_results) {
      break;
    }
    matched_files.push_back(std::move(file));
  }

  return matched_files;
}

std::vector<SearchManager::FileInfo> SearchManager::GetMatchedFilesByTags(
    std::vector<FileInfo> filtered_files, const std::vector<std::string> &tags,
    const std::string &tag_operator, int max_results) {
  std::vector<FileInfo> matched_files;
  for (auto &file : filtered_files) {
    if (file.is_folder) {
      continue;
    }

    if (MatchesTags(file.tags, tags, tag_operator)) {
      matched_files.push_back(std::move(file));
      if (static_cast<int>(matched_files.size()) >= max_results) {
        break;
      }
    }
  }
  return matched_files;
}

SearchInputFiles SearchManager::ParseInputFiles(const std::string &input_files_json) {
  SearchInputFiles input_files;
  if (!input_files_json.empty()) {
    auto input_json = nlohmann::json::parse(input_files_json);
    input_files = SearchInputFiles::FromJson(notebook_, input_json);
  }
  return input_files;
}

nlohmann::json SearchManager::FileInfo::ToJson() const {
  nlohmann::json json;
  json["type"] = is_folder ? "folder" : "file";
  json["path"] = path;
  json["id"] = id;
  json["createdUtc"] = created_utc;
  json["modifiedUtc"] = modified_utc;
  json["tags"] = tags;
  return json;
}

std::string SearchManager::SerializeFileResults(const std::vector<FileInfo> &matched_files,
                                                int max_results) {
  nlohmann::json result;
  result["totalResults"] = matched_files.size();
  result["truncated"] = static_cast<int>(matched_files.size()) >= max_results;
  result["results"] = nlohmann::json::array();

  for (const auto &file : matched_files) {
    result["results"].push_back(file.ToJson());
  }

  return result.dump();
}

SearchManager::FileInfo SearchManager::ToFileInfo(const std::string &file_path,
                                                  const FileRecord &record) {
  SearchManager::FileInfo info;
  info.path = file_path;
  info.name = record.name;
  info.id = record.id;
  info.tags = record.tags;
  info.created_utc = record.created_utc;
  info.modified_utc = record.modified_utc;
  info.is_folder = false;
  return info;
}

SearchManager::FileInfo SearchManager::ToFileInfo(const std::string &folder_path,
                                                  const FolderRecord &record) {
  SearchManager::FileInfo info;
  info.path = folder_path;
  info.name = record.name;
  info.id = record.id;
  info.created_utc = record.created_utc;
  info.modified_utc = record.modified_utc;
  info.is_folder = true;
  return info;
}

std::vector<SearchManager::FileInfo> SearchManager::GetAllFiles(const SearchScope &scope,
                                                                const SearchInputFiles *input_files,
                                                                bool include_folders) {
  std::vector<FileInfo> result;

  if (input_files && (!input_files->files.empty() || !input_files->folders.empty())) {
    for (const auto &file_path : input_files->files) {
      const FileRecord *record = nullptr;
      if (notebook_->GetFolderManager()->GetFileInfo(file_path, &record) == VXCORE_OK) {
        assert(record);
        result.push_back(ToFileInfo(file_path, *record));
      }
    }

    for (const auto &folder_path : input_files->folders) {
      CollectFilesInFolder(folder_path, scope.recursive, scope.file_patterns,
                           scope.exclude_patterns, include_folders, result);
    }
  } else {
    const std::string start_path = scope.folder_path.empty() ? "." : scope.folder_path;
    CollectFilesInFolder(start_path, scope.recursive, scope.file_patterns, scope.exclude_patterns,
                         include_folders, result);
  }

  return result;
}

std::vector<SearchManager::FileInfo> SearchManager::FilterFilesByTagsAndDate(
    std::vector<FileInfo> files, const SearchScope &scope) {
  std::vector<FileInfo> result;
  const bool filter_by_created = scope.date_filter_field == "created";
  const bool filter_by_modified = scope.date_filter_field == "modified";
  for (auto &file : files) {
    bool matches = true;

    if (!scope.tags.empty() && !file.is_folder) {
      if (!MatchesTags(file.tags, scope.tags, scope.tag_operator)) {
        matches = false;
      }
    }

    if (matches && !scope.date_filter_field.empty()) {
      int64_t timestamp = 0;
      if (filter_by_created) {
        timestamp = file.created_utc;
      } else if (filter_by_modified) {
        timestamp = file.modified_utc;
      }
      if (!MatchesDateFilter(timestamp, scope)) {
        matches = false;
      }
    }

    if (matches) {
      result.push_back(std::move(file));
    }
  }
  return result;
}

void SearchManager::CollectFilesInFolder(const std::string &folder_path, bool recursive,
                                         const std::vector<std::string> &file_patterns,
                                         const std::vector<std::string> &exclude_patterns,
                                         bool include_folders, std::vector<FileInfo> &out_files) {
  if (MatchesPatterns(folder_path, exclude_patterns)) {
    return;
  }

  FolderManager::FolderContents contents;
  if (notebook_->GetFolderManager()->ListFolderContents(folder_path, include_folders, contents) !=
      VXCORE_OK) {
    return;
  }

  for (const auto &file : contents.files) {
    const auto file_path = ConcatenatePaths(folder_path, file.name);
    if (MatchesPatterns(file_path, exclude_patterns)) {
      continue;
    }
    if (!file_patterns.empty() && !MatchesPatterns(file_path, file_patterns)) {
      continue;
    }
    out_files.push_back(ToFileInfo(file_path, file));
  }

  for (const auto &folder : contents.folders) {
    std::string subfolder_path = ConcatenatePaths(folder_path, folder.name);

    if (MatchesPatterns(subfolder_path, exclude_patterns)) {
      continue;
    }

    if (include_folders) {
      out_files.push_back(ToFileInfo(subfolder_path, folder));
    }

    if (recursive) {
      CollectFilesInFolder(subfolder_path, recursive, file_patterns, exclude_patterns,
                           include_folders, out_files);
    }
  }
}

bool SearchManager::MatchesPattern(const std::string &text, const std::string &pattern) const {
  if (pattern.find('*') == std::string::npos && pattern.find('?') == std::string::npos) {
    return text.find(pattern) != std::string::npos;
  }

  size_t text_idx = 0;
  size_t pattern_idx = 0;
  size_t star_idx = std::string::npos;
  size_t match_idx = 0;

  while (text_idx < text.size()) {
    if (pattern_idx < pattern.size() &&
        (pattern[pattern_idx] == '?' || pattern[pattern_idx] == text[text_idx])) {
      text_idx++;
      pattern_idx++;
    } else if (pattern_idx < pattern.size() && pattern[pattern_idx] == '*') {
      star_idx = pattern_idx;
      match_idx = text_idx;
      pattern_idx++;
    } else if (star_idx != std::string::npos) {
      pattern_idx = star_idx + 1;
      match_idx++;
      text_idx = match_idx;
    } else {
      return false;
    }
  }

  while (pattern_idx < pattern.size() && pattern[pattern_idx] == '*') {
    pattern_idx++;
  }

  return pattern_idx == pattern.size();
}

bool SearchManager::MatchesPatterns(const std::string &text,
                                    const std::vector<std::string> &patterns) const {
  for (const auto &pattern : patterns) {
    if (MatchesPattern(text, pattern)) {
      return true;
    }
  }
  return false;
}

bool SearchManager::MatchesTags(const std::vector<std::string> &file_tags,
                                const std::vector<std::string> &search_tags,
                                const std::string &tag_operator) const {
  if (search_tags.empty()) {
    return true;
  }

  if (tag_operator == "AND") {
    for (const auto &search_tag : search_tags) {
      if (std::find(file_tags.begin(), file_tags.end(), search_tag) == file_tags.end()) {
        return false;
      }
    }
    return true;
  } else {
    for (const auto &search_tag : search_tags) {
      if (std::find(file_tags.begin(), file_tags.end(), search_tag) != file_tags.end()) {
        return true;
      }
    }
    return false;
  }
}

bool SearchManager::MatchesDateFilter(int64_t timestamp, const SearchScope &scope) const {
  if (scope.date_filter_from > 0 && timestamp < scope.date_filter_from) {
    return false;
  }
  if (scope.date_filter_to > 0 && timestamp > scope.date_filter_to) {
    return false;
  }
  return true;
}

}  // namespace vxcore
