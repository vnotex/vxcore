#include <iostream>
#include <string>

#include "nlohmann/json.hpp"
#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_search_files_basic() {
  std::cout << "  Running test_search_files_basic..." << std::endl;
  cleanup_test_dir("test_search_basic");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_basic", "{\"name\":\"Test Search Basic\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file2.txt", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "notes.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  const char *query_json = R"({
    "pattern": "*.md",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);
  ASSERT(json_results["results"].is_array());
  ASSERT(json_results["results"].size() == 2);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_basic");
  std::cout << "  ✓ test_search_files_basic passed" << std::endl;
  return 0;
}

int test_search_files_include_folders() {
  std::cout << "  Running test_search_files_include_folders..." << std::endl;
  cleanup_test_dir("test_search_folders");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_folders", "{\"name\":\"Test Search Folders\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *docs_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &docs_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(docs_id);

  char *images_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "images", &images_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(images_id);

  const char *query_json = R"({
    "pattern": "*",
    "includeFiles": true,
    "includeFolders": true,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 3);

  int file_count = 0;
  int folder_count = 0;
  for (const auto &item : json_results["results"]) {
    if (item["type"].get<std::string>() == "folder") {
      folder_count++;
    } else {
      file_count++;
    }
  }
  ASSERT(file_count == 1);
  ASSERT(folder_count == 2);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_folders");
  std::cout << "  ✓ test_search_files_include_folders passed" << std::endl;
  return 0;
}

int test_search_files_pattern_matching() {
  std::cout << "  Running test_search_files_pattern_matching..." << std::endl;
  cleanup_test_dir("test_search_pattern");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_pattern", "{\"name\":\"Test Search Pattern\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *readme_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &readme_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(readme_id);

  char *todo_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "todo.txt", &todo_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(todo_id);

  char *notes_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "notes.md", &notes_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notes_id);

  char *subfolder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &subfolder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(subfolder_id);

  char *deep_readme_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "subfolder", "deep_readme.md", &deep_readme_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(deep_readme_id);

  const char *query_json = R"({
    "pattern": "readme",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": true
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);

  bool found_readme = false;
  bool found_deep_readme = false;
  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    if (path.find("readme.md") != std::string::npos) {
      found_readme = true;
    }
    if (path.find("deep_readme.md") != std::string::npos) {
      found_deep_readme = true;
    }
  }
  ASSERT(found_readme);
  ASSERT(found_deep_readme);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_pattern");
  std::cout << "  ✓ test_search_files_pattern_matching passed" << std::endl;
  return 0;
}

int test_search_files_name_vs_path_ranking() {
  std::cout << "  Running test_search_files_name_vs_path_ranking..." << std::endl;
  cleanup_test_dir("test_search_ranking");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_ranking", "{\"name\":\"Test Search Ranking\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *other_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "other.md", &other_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(other_id);

  char *readme_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "readme", &readme_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(readme_folder_id);

  char *file_in_readme_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "readme", "file.md", &file_in_readme_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_in_readme_id);

  char *readme_file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &readme_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(readme_file_id);

  const char *query_json = R"({
    "pattern": "readme",
    "includeFiles": true,
    "includeFolders": true,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": true
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() >= 2);

  int first_name_match_index = -1;
  int first_path_match_index = -1;

  for (size_t i = 0; i < json_results["results"].size(); i++) {
    std::string path = json_results["results"][i]["path"].get<std::string>();
    size_t last_slash = path.find_last_of('/');
    std::string name = (last_slash == std::string::npos) ? path : path.substr(last_slash + 1);

    bool name_has_readme = (name.find("readme") != std::string::npos);
    bool path_has_readme = (path.find("readme") != std::string::npos);

    if (name_has_readme && first_name_match_index == -1) {
      first_name_match_index = static_cast<int>(i);
    }
    if (!name_has_readme && path_has_readme && first_path_match_index == -1) {
      first_path_match_index = static_cast<int>(i);
    }
  }

  if (first_name_match_index != -1 && first_path_match_index != -1) {
    ASSERT(first_name_match_index < first_path_match_index);
  }

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_ranking");
  std::cout << "  ✓ test_search_files_name_vs_path_ranking passed" << std::endl;
  return 0;
}

int test_search_files_exclude_patterns() {
  std::cout << "  Running test_search_files_exclude_patterns..." << std::endl;
  cleanup_test_dir("test_search_exclude");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_exclude", "{\"name\":\"Test Search Exclude\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *node_modules_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "node_modules", &node_modules_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(node_modules_id);

  char *lib_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "node_modules", "lib.js", &lib_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(lib_id);

  const char *query_json = R"({
    "pattern": "*",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": true,
      "excludePatterns": ["node_modules"]
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);

  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    ASSERT(path.find("node_modules") == std::string::npos);
  }

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_exclude");
  std::cout << "  ✓ test_search_files_exclude_patterns passed" << std::endl;
  return 0;
}

int test_search_files_max_results() {
  std::cout << "  Running test_search_files_max_results..." << std::endl;
  cleanup_test_dir("test_search_max");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_max", "{\"name\":\"Test Search Max\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  for (int i = 1; i <= 10; i++) {
    char *file_id = nullptr;
    std::string filename = "file" + std::to_string(i) + ".md";
    err = vxcore_file_create(ctx, notebook_id, ".", filename.c_str(), &file_id);
    ASSERT_EQ(err, VXCORE_OK);
    vxcore_string_free(file_id);
  }

  const char *query_json = R"({
    "pattern": "*.md",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 5,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 5);
  ASSERT(json_results["results"].size() == 5);
  ASSERT(json_results["truncated"].get<bool>() == true);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_max");
  std::cout << "  ✓ test_search_files_max_results passed" << std::endl;
  return 0;
}

int test_search_by_tags_single_tag() {
  std::cout << "  Running test_search_by_tags_single_tag..." << std::endl;
  cleanup_test_dir("test_search_tags_single");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_single",
                               "{\"name\":\"Test Search Tags Single\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file3.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "todo");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file2.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file3.md", "todo");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["important"],
    "operator": "AND",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);
  ASSERT(json_results["results"].size() == 2);

  for (const auto &item : json_results["results"]) {
    ASSERT(item["type"].get<std::string>() == "file");
    auto tags = item["tags"];
    ASSERT(tags.is_array());
    ASSERT(std::find(tags.begin(), tags.end(), "important") != tags.end());
  }

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_single");
  std::cout << "  ✓ test_search_by_tags_single_tag passed" << std::endl;
  return 0;
}

int test_search_by_tags_and_operator() {
  std::cout << "  Running test_search_by_tags_and_operator..." << std::endl;
  cleanup_test_dir("test_search_tags_and");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_and", "{\"name\":\"Test Search Tags AND\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file3.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "important");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file2.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file3.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["important", "urgent"],
    "operator": "AND",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 1);
  ASSERT(json_results["results"].size() == 1);

  auto &item = json_results["results"][0];
  std::string path = item["path"].get<std::string>();
  ASSERT(path.find("file1.md") != std::string::npos);

  auto tags = item["tags"];
  ASSERT(std::find(tags.begin(), tags.end(), "important") != tags.end());
  ASSERT(std::find(tags.begin(), tags.end(), "urgent") != tags.end());

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_and");
  std::cout << "  ✓ test_search_by_tags_and_operator passed" << std::endl;
  return 0;
}

int test_search_by_tags_or_operator() {
  std::cout << "  Running test_search_by_tags_or_operator..." << std::endl;
  cleanup_test_dir("test_search_tags_or");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_or", "{\"name\":\"Test Search Tags OR\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file3.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "todo");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file2.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file3.md", "todo");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["important", "urgent"],
    "operator": "OR",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);
  ASSERT(json_results["results"].size() == 2);

  bool found_file1 = false;
  bool found_file2 = false;
  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    if (path.find("file1.md") != std::string::npos) {
      found_file1 = true;
    }
    if (path.find("file2.md") != std::string::npos) {
      found_file2 = true;
    }
  }
  ASSERT(found_file1);
  ASSERT(found_file2);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_or");
  std::cout << "  ✓ test_search_by_tags_or_operator passed" << std::endl;
  return 0;
}

int test_search_by_tags_recursive() {
  std::cout << "  Running test_search_by_tags_recursive..." << std::endl;
  cleanup_test_dir("test_search_tags_recursive");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_recursive",
                               "{\"name\":\"Test Search Tags Recursive\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *subfolder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &subfolder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(subfolder_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "subfolder", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  err = vxcore_tag_create(ctx, notebook_id, "project");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "project");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "subfolder/file2.md", "project");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["project"],
    "operator": "AND",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": true
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_recursive");
  std::cout << "  ✓ test_search_by_tags_recursive passed" << std::endl;
  return 0;
}

int test_search_by_tags_with_exclude_patterns() {
  std::cout << "  Running test_search_by_tags_with_exclude_patterns..." << std::endl;
  cleanup_test_dir("test_search_tags_exclude");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_exclude",
                               "{\"name\":\"Test Search Tags Exclude\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *archive_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "archive", &archive_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(archive_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "archive", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "archive/file2.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["important"],
    "operator": "AND",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": true,
      "excludePatterns": ["archive"]
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 1);

  auto &item = json_results["results"][0];
  std::string path = item["path"].get<std::string>();
  ASSERT(path.find("file1.md") != std::string::npos);
  ASSERT(path.find("archive") == std::string::npos);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_exclude");
  std::cout << "  ✓ test_search_by_tags_with_exclude_patterns passed" << std::endl;
  return 0;
}

int test_search_by_tags_max_results() {
  std::cout << "  Running test_search_by_tags_max_results..." << std::endl;
  cleanup_test_dir("test_search_tags_max");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_max", "{\"name\":\"Test Search Tags Max\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "test");
  ASSERT_EQ(err, VXCORE_OK);

  for (int i = 1; i <= 10; i++) {
    char *file_id = nullptr;
    std::string filename = "file" + std::to_string(i) + ".md";
    err = vxcore_file_create(ctx, notebook_id, ".", filename.c_str(), &file_id);
    ASSERT_EQ(err, VXCORE_OK);
    vxcore_string_free(file_id);

    err = vxcore_file_tag(ctx, notebook_id, filename.c_str(), "test");
    ASSERT_EQ(err, VXCORE_OK);
  }

  const char *query_json = R"({
    "tags": ["test"],
    "operator": "AND",
    "maxResults": 5,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 5);
  ASSERT(json_results["results"].size() == 5);
  ASSERT(json_results["truncated"].get<bool>() == true);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_max");
  std::cout << "  ✓ test_search_by_tags_max_results passed" << std::endl;
  return 0;
}

int test_search_by_tags_empty_results() {
  std::cout << "  Running test_search_by_tags_empty_results..." << std::endl;
  cleanup_test_dir("test_search_tags_empty");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, "test_search_tags_empty", "{\"name\":\"Test Search Tags Empty\"}",
                             VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  err = vxcore_tag_create(ctx, notebook_id, "existing");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "file1.md", "existing");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["nonexistent"],
    "operator": "AND",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 0);
  ASSERT(json_results["results"].size() == 0);
  ASSERT(json_results["truncated"].get<bool>() == false);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_empty");
  std::cout << "  ✓ test_search_by_tags_empty_results passed" << std::endl;
  return 0;
}

int test_search_files_with_file_patterns() {
  std::cout << "  Running test_search_files_with_file_patterns..." << std::endl;
  cleanup_test_dir("test_search_file_patterns");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_file_patterns",
                               "{\"name\":\"Test Search File Patterns\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "todo.txt", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "notes.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  char *file4_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "script.py", &file4_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file4_id);

  const char *query_json = R"({
    "pattern": "*",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false,
      "filePatterns": ["*.md"]
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);

  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    ASSERT(path.find(".md") != std::string::npos);
  }

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_file_patterns");
  std::cout << "  ✓ test_search_files_with_file_patterns passed" << std::endl;
  return 0;
}

int test_search_files_file_patterns_recursive() {
  std::cout << "  Running test_search_files_file_patterns_recursive..." << std::endl;
  cleanup_test_dir("test_search_file_patterns_recursive");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_file_patterns_recursive",
                               "{\"name\":\"Test Search File Patterns Recursive\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "root.cpp", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "root.h", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  char *subfolder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &subfolder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(subfolder_id);

  char *file4_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "main.cpp", &file4_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file4_id);

  char *file5_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "utils.h", &file5_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file5_id);

  char *file6_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "notes.txt", &file6_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file6_id);

  const char *query_json = R"({
    "pattern": "*",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": true,
      "filePatterns": ["*.cpp", "*.h"]
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 4);

  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    bool is_cpp = path.find(".cpp") != std::string::npos;
    bool is_h = path.find(".h") != std::string::npos;
    ASSERT(is_cpp || is_h);
  }

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_file_patterns_recursive");
  std::cout << "  ✓ test_search_files_file_patterns_recursive passed" << std::endl;
  return 0;
}

int test_search_by_tags_with_file_patterns() {
  std::cout << "  Running test_search_by_tags_with_file_patterns..." << std::endl;
  cleanup_test_dir("test_search_tags_file_patterns");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_tags_file_patterns",
                               "{\"name\":\"Test Search Tags File Patterns\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "doc1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "doc2.txt", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "doc3.md", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "doc1.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "doc2.txt", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "doc3.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  const char *query_json = R"({
    "tags": ["important"],
    "operator": "AND",
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false,
      "filePatterns": ["*.md"]
    }
  })";

  char *results = nullptr;
  err = vxcore_search_by_tags(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);

  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    ASSERT(path.find(".md") != std::string::npos);
    auto tags = item["tags"];
    ASSERT(std::find(tags.begin(), tags.end(), "important") != tags.end());
  }

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_tags_file_patterns");
  std::cout << "  ✓ test_search_by_tags_with_file_patterns passed" << std::endl;
  return 0;
}

int test_search_file_and_exclude_patterns() {
  std::cout << "  Running test_search_file_and_exclude_patterns..." << std::endl;
  cleanup_test_dir("test_search_include_exclude");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_search_include_exclude",
                               "{\"name\":\"Test Search Include Exclude\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.cpp", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file1_id);

  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "main.cpp", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file2_id);

  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "utils.h", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file3_id);

  char *file4_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &file4_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file4_id);

  const char *query_json = R"({
    "pattern": "*",
    "includeFiles": true,
    "includeFolders": false,
    "maxResults": 100,
    "scope": {
      "folderPath": ".",
      "recursive": false,
      "filePatterns": ["*.cpp", "*.h"],
      "excludePatterns": ["test.cpp"]
    }
  })";

  char *results = nullptr;
  err = vxcore_search_files(ctx, notebook_id, query_json, nullptr, &results);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results);

  auto json_results = nlohmann::json::parse(results);
  ASSERT(json_results["totalResults"].get<int>() == 2);

  bool found_main = false;
  bool found_utils = false;
  for (const auto &item : json_results["results"]) {
    std::string path = item["path"].get<std::string>();
    ASSERT(path.find("test.cpp") == std::string::npos);
    ASSERT(path.find("readme.md") == std::string::npos);
    if (path.find("main.cpp") != std::string::npos) {
      found_main = true;
    }
    if (path.find("utils.h") != std::string::npos) {
      found_utils = true;
    }
  }
  ASSERT(found_main);
  ASSERT(found_utils);

  vxcore_string_free(results);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_search_include_exclude");
  std::cout << "  ✓ test_search_file_and_exclude_patterns passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "\nRunning search tests..." << std::endl;

  RUN_TEST(test_search_files_basic);
  RUN_TEST(test_search_files_include_folders);
  RUN_TEST(test_search_files_pattern_matching);
  RUN_TEST(test_search_files_name_vs_path_ranking);
  RUN_TEST(test_search_files_exclude_patterns);
  RUN_TEST(test_search_files_max_results);
  RUN_TEST(test_search_files_with_file_patterns);
  RUN_TEST(test_search_files_file_patterns_recursive);

  RUN_TEST(test_search_by_tags_single_tag);
  RUN_TEST(test_search_by_tags_and_operator);
  RUN_TEST(test_search_by_tags_or_operator);
  RUN_TEST(test_search_by_tags_recursive);
  RUN_TEST(test_search_by_tags_with_exclude_patterns);
  RUN_TEST(test_search_by_tags_max_results);
  RUN_TEST(test_search_by_tags_empty_results);
  RUN_TEST(test_search_by_tags_with_file_patterns);
  RUN_TEST(test_search_file_and_exclude_patterns);

  std::cout << "All search tests passed!" << std::endl;
  return 0;
}
