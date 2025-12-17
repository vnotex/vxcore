#ifndef VXCORE_SEARCH_QUERY_H
#define VXCORE_SEARCH_QUERY_H

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vxcore {

class Notebook;

struct SearchScope {
  std::string folder_path;
  bool recursive = true;
  std::vector<std::string> file_patterns;
  std::vector<std::string> exclude_patterns;
  std::vector<std::string> tags;
  std::string tag_operator = "AND";
  std::string date_filter_field;
  int64_t date_filter_from = 0;
  int64_t date_filter_to = 0;

  static SearchScope FromJson(const nlohmann::json &json);
  static SearchScope FromJson(const Notebook *notebook, const nlohmann::json &json);
};

struct SearchInputFiles {
  std::vector<std::string> files;
  std::vector<std::string> folders;

  static SearchInputFiles FromJson(const nlohmann::json &json);
  static SearchInputFiles FromJson(const Notebook *notebook, const nlohmann::json &json);
};

struct SearchFilesQuery {
  std::string pattern;
  bool include_files = true;
  bool include_folders = true;
  SearchScope scope;
  int max_results = 100;

  static SearchFilesQuery FromJson(const nlohmann::json &json);
  static SearchFilesQuery FromJson(const Notebook *notebook, const nlohmann::json &json);
};

struct SearchContentQuery {
  std::string pattern;
  bool case_sensitive = false;
  bool whole_word = false;
  bool regex = false;
  SearchScope scope;
  int max_results = 100;

  static SearchContentQuery FromJson(const nlohmann::json &json);
  static SearchContentQuery FromJson(const Notebook *notebook, const nlohmann::json &json);
};

struct SearchByTagsQuery {
  std::vector<std::string> tags;
  std::string tag_operator = "AND";
  SearchScope scope;
  int max_results = 100;

  static SearchByTagsQuery FromJson(const nlohmann::json &json);
  static SearchByTagsQuery FromJson(const Notebook *notebook, const nlohmann::json &json);
};

}  // namespace vxcore

#endif
