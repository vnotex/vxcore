#include "search_query.h"

#include "core/notebook.h"

namespace vxcore {

SearchScope SearchScope::FromJson(const nlohmann::json &json) { return FromJson(nullptr, json); }

SearchScope SearchScope::FromJson(const Notebook *notebook, const nlohmann::json &json) {
  SearchScope scope;
  if (json.contains("folderPath")) {
    scope.folder_path = json["folderPath"].get<std::string>();
    if (notebook) {
      scope.folder_path = notebook->GetCleanRelativePath(scope.folder_path);
    }
  }
  if (json.contains("recursive")) {
    scope.recursive = json["recursive"].get<bool>();
  }
  if (json.contains("filePatterns") && json["filePatterns"].is_array()) {
    scope.path_patterns.reserve(json["filePatterns"].size());
    for (const auto &pattern : json["filePatterns"]) {
      scope.path_patterns.push_back(pattern.get<std::string>());
    }
  }
  if (json.contains("excludePatterns") && json["excludePatterns"].is_array()) {
    for (const auto &pattern : json["excludePatterns"]) {
      scope.exclude_path_patterns.push_back(pattern.get<std::string>());
    }
  }
  if (json.contains("tags") && json["tags"].is_array()) {
    scope.tags.reserve(json["tags"].size());
    for (const auto &tag : json["tags"]) {
      scope.tags.push_back(tag.get<std::string>());
    }
  }
  if (json.contains("excludeTags") && json["excludeTags"].is_array()) {
    scope.exclude_tags.reserve(json["excludeTags"].size());
    for (const auto &tag : json["excludeTags"]) {
      scope.exclude_tags.push_back(tag.get<std::string>());
    }
  }
  if (json.contains("tagOperator")) {
    scope.tag_operator = json["tagOperator"].get<std::string>();
  }
  if (json.contains("dateFilter") && json["dateFilter"].is_object()) {
    const auto &df = json["dateFilter"];
    if (df.contains("field")) {
      scope.date_filter_field = df["field"].get<std::string>();
    }
    if (df.contains("from")) {
      scope.date_filter_from = df["from"].get<int64_t>();
    }
    if (df.contains("to")) {
      scope.date_filter_to = df["to"].get<int64_t>();
    }
  }
  return scope;
}

SearchInputFiles SearchInputFiles::FromJson(const nlohmann::json &json) {
  return FromJson(nullptr, json);
}

SearchInputFiles SearchInputFiles::FromJson(const Notebook *notebook, const nlohmann::json &json) {
  SearchInputFiles input;
  if (json.contains("files") && json["files"].is_array()) {
    input.files.reserve(json["files"].size());
    for (const auto &file : json["files"]) {
      auto file_path = file.get<std::string>();
      if (notebook) {
        file_path = notebook->GetCleanRelativePath(file_path);
      }
      input.files.push_back(file_path);
    }
  }
  if (json.contains("folders") && json["folders"].is_array()) {
    input.folders.reserve(json["folders"].size());
    for (const auto &folder : json["folders"]) {
      auto folder_path = folder.get<std::string>();
      if (notebook) {
        folder_path = notebook->GetCleanRelativePath(folder_path);
      }
      input.folders.push_back(folder_path);
    }
  }
  return input;
}

SearchFilesQuery SearchFilesQuery::FromJson(const nlohmann::json &json) {
  return FromJson(nullptr, json);
}

SearchFilesQuery SearchFilesQuery::FromJson(const Notebook *notebook, const nlohmann::json &json) {
  SearchFilesQuery query;

  if (json.contains("pattern")) {
    query.pattern = json["pattern"].get<std::string>();
  }

  if (json.contains("includeFiles")) {
    query.include_files = json["includeFiles"].get<bool>();
  }

  if (json.contains("includeFolders")) {
    query.include_folders = json["includeFolders"].get<bool>();
  }

  if (json.contains("scope")) {
    query.scope = SearchScope::FromJson(notebook, json["scope"]);
  }

  if (json.contains("maxResults")) {
    query.max_results = json["maxResults"].get<int>();
  }

  return query;
}

SearchContentQuery SearchContentQuery::FromJson(const nlohmann::json &json) {
  return FromJson(nullptr, json);
}

SearchContentQuery SearchContentQuery::FromJson(const Notebook *notebook,
                                                const nlohmann::json &json) {
  SearchContentQuery query;

  if (json.contains("pattern")) {
    query.pattern = json["pattern"].get<std::string>();
  }

  if (json.contains("excludePatterns") && json["excludePatterns"].is_array()) {
    query.exclude_patterns.reserve(json["excludePatterns"].size());
    for (const auto &pattern : json["excludePatterns"]) {
      query.exclude_patterns.push_back(pattern.get<std::string>());
    }
  }

  query.case_sensitive = json.value("caseSensitive", false);
  query.whole_word = json.value("wholeWord", false);
  query.regex = json.value("regex", false);

  if (json.contains("scope")) {
    query.scope = SearchScope::FromJson(notebook, json["scope"]);
  }

  if (json.contains("maxResults")) {
    query.max_results = json["maxResults"].get<int>();
  }

  return query;
}

SearchByTagsQuery SearchByTagsQuery::FromJson(const nlohmann::json &json) {
  return FromJson(nullptr, json);
}

SearchByTagsQuery SearchByTagsQuery::FromJson(const Notebook *notebook,
                                              const nlohmann::json &json) {
  SearchByTagsQuery query;

  if (json.contains("tags") && json["tags"].is_array()) {
    query.tags.reserve(json["tags"].size());
    for (const auto &tag : json["tags"]) {
      query.tags.push_back(tag.get<std::string>());
    }
  }

  query.tag_operator = json.value("operator", "AND");

  if (json.contains("scope")) {
    query.scope = SearchScope::FromJson(notebook, json["scope"]);
  }

  if (json.contains("maxResults")) {
    query.max_results = json["maxResults"].get<int>();
  }

  return query;
}

}  // namespace vxcore
