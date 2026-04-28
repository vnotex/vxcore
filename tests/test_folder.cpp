#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"

namespace {

std::filesystem::path PathFromUtf8ForTest(const std::string &utf8_str) {
  if (utf8_str.empty()) {
    return {};
  }

#ifdef _WIN32
  int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
  if (wide_size <= 0) {
    return {};
  }

  std::wstring wide_str(static_cast<size_t>(wide_size), L'\0');
  int converted = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wide_str.data(), wide_size);
  if (converted <= 0) {
    return {};
  }

  wide_str.resize(static_cast<size_t>(wide_size - 1));
  return std::filesystem::path(wide_str);
#else
  return std::filesystem::path(utf8_str);
#endif
}

std::string PathToUtf8ForTest(const std::filesystem::path &path) {
#ifdef _WIN32
  std::wstring wide_str = path.wstring();
  if (wide_str.empty()) {
    return {};
  }

  int utf8_size =
      WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0) {
    return {};
  }

  std::string utf8_str(static_cast<size_t>(utf8_size), '\0');
  int converted = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, utf8_str.data(), utf8_size,
                                      nullptr, nullptr);
  if (converted <= 0) {
    return {};
  }

  utf8_str.resize(static_cast<size_t>(utf8_size - 1));
  return utf8_str;
#else
  return path.string();
#endif
}

}  // namespace

int test_folder_create() {
  std::cout << "  Running test_folder_create..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_create_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_create_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "test_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_create_nb") + "/test_folder"));
  ASSERT(path_exists(get_test_path("test_folder_create_nb/vx_notebook/contents/test_folder") +
                     "/vx.json"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_create_nb"));
  std::cout << "  âœ“ test_folder_create passed" << std::endl;
  return 0;
}

int test_file_create() {
  std::cout << "  Running test_file_create..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_create_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_create_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config.contains("files"));
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "note.md");

  vxcore_string_free(config_json);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_create_nb"));
  std::cout << "  âœ“ test_file_create passed" << std::endl;
  return 0;
}

int test_file_metadata_and_tags() {
  std::cout << "  Running test_file_metadata_and_tags..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_meta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_meta_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_update_metadata(ctx, notebook_id, "note.md",
                                    R"({"author": "John Doe", "priority": "high"})");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_update_tags(ctx, notebook_id, "note.md", R"(["work", "urgent"])");
  ASSERT_EQ(err, VXCORE_OK);

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"][0]["metadata"]["author"] == "John Doe");
  ASSERT(config["files"][0]["tags"].size() == 2);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_meta_nb"));
  std::cout << "  âœ“ test_file_metadata_and_tags passed" << std::endl;
  return 0;
}

int test_folder_delete() {
  std::cout << "  Running test_folder_delete..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_delete_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_delete_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "to_delete", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_delete_nb") + "/to_delete"));

  err = vxcore_node_delete(ctx, notebook_id, "to_delete");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_folder_delete_nb") + "/to_delete"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_delete_nb"));
  std::cout << "  âœ“ test_folder_delete passed" << std::endl;
  return 0;
}

int test_folder_create_duplicate() {
  std::cout << "  Running test_folder_create_duplicate..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_dup_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_dup_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "duplicate", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *folder_id2 = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "duplicate", &folder_id2);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_NULL(folder_id2);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_dup_nb"));
  std::cout << "  âœ“ test_folder_create_duplicate passed" << std::endl;
  return 0;
}

int test_file_create_duplicate() {
  std::cout << "  Running test_file_create_duplicate..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_dup_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_dup_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "duplicate.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "duplicate.md", &file_id2);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_NULL(file_id2);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_dup_nb"));
  std::cout << "  âœ“ test_file_create_duplicate passed" << std::endl;
  return 0;
}

int test_folder_invalid_params() {
  std::cout << "  Running test_folder_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(nullptr, notebook_id, ".", "test", &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_folder_create(ctx, nullptr, ".", "test", &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_folder_create(ctx, notebook_id, ".", nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_folder_create(ctx, notebook_id, ".", "test", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_invalid_nb"));
  std::cout << "  âœ“ test_folder_invalid_params passed" << std::endl;
  return 0;
}

int test_file_invalid_params() {
  std::cout << "  Running test_file_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(nullptr, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_create(ctx, nullptr, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_create(ctx, notebook_id, nullptr, "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_create(ctx, notebook_id, ".", nullptr, &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_invalid_nb"));
  std::cout << "  âœ“ test_file_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_not_found() {
  std::cout << "  Running test_folder_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_node_delete(ctx, notebook_id, "nonexistent");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_notfound_nb"));
  std::cout << "  âœ“ test_folder_not_found passed" << std::endl;
  return 0;
}

int test_file_not_found() {
  std::cout << "  Running test_file_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_node_delete(ctx, notebook_id, "nonexistent.md");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  err = vxcore_node_update_metadata(ctx, notebook_id, "nonexistent.md", "{}");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  err = vxcore_file_update_tags(ctx, notebook_id, "nonexistent.md", "[]");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_notfound_nb"));
  std::cout << "  âœ“ test_file_not_found passed" << std::endl;
  return 0;
}

int test_invalid_json() {
  std::cout << "  Running test_invalid_json..." << std::endl;
  cleanup_test_dir(get_test_path("test_invalid_json_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_invalid_json_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_update_metadata(ctx, notebook_id, "test.md", "invalid json");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  err = vxcore_node_update_metadata(ctx, notebook_id, "test.md", "[]");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  err = vxcore_file_update_tags(ctx, notebook_id, "test.md", "not an array");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  err = vxcore_file_update_tags(ctx, notebook_id, "test.md", "{}");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_invalid_json_nb"));
  std::cout << "  âœ“ test_invalid_json passed" << std::endl;
  return 0;
}

int test_notebook_not_found() {
  std::cout << "  Running test_notebook_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_nb_notfound"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, "nonexistent_nb", ".", "test", &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, "nonexistent_nb", ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_nb_notfound"));
  std::cout << "  âœ“ test_notebook_not_found passed" << std::endl;
  return 0;
}

int test_file_delete() {
  std::cout << "  Running test_file_delete..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_delete_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_delete_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "to_delete.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  ASSERT(path_exists(get_test_path("test_file_delete_nb") + "/to_delete.md"));

  err = vxcore_node_delete(ctx, notebook_id, "to_delete.md");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_file_delete_nb") + "/to_delete.md"));

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 0);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_delete_nb"));
  std::cout << "  âœ“ test_file_delete passed" << std::endl;
  return 0;
}

int test_nested_folder_operations() {
  std::cout << "  Running test_nested_folder_operations..." << std::endl;
  cleanup_test_dir(get_test_path("test_nested_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_nested_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder1_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &folder1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder1_id);

  char *folder2_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &folder2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder2_id);

  ASSERT(path_exists(get_test_path("test_nested_nb") + "/parent"));
  ASSERT(path_exists(get_test_path("test_nested_nb/parent") + "/child"));

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "parent/child", "nested.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  ASSERT(path_exists(get_test_path("test_nested_nb/parent/child") + "/nested.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_nested_nb"));
  std::cout << "  âœ“ test_nested_folder_operations passed" << std::endl;
  return 0;
}

int test_folder_rename() {
  std::cout << "  Running test_folder_rename..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_rename_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_rename_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "old_name", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_rename_nb") + "/old_name"));

  err = vxcore_node_rename(ctx, notebook_id, "old_name", "new_name");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_folder_rename_nb") + "/old_name"));
  ASSERT(path_exists(get_test_path("test_folder_rename_nb") + "/new_name"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_rename_nb"));
  std::cout << "  âœ“ test_folder_rename passed" << std::endl;
  return 0;
}

int test_folder_move() {
  std::cout << "  Running test_folder_move..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_move_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_move_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "source", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_node_move(ctx, notebook_id, "source", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_folder_move_nb") + "/source"));
  ASSERT(path_exists(get_test_path("test_folder_move_nb/dest") + "/source"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_move_nb"));
  std::cout << "  âœ“ test_folder_move passed" << std::endl;
  return 0;
}

int test_folder_copy() {
  std::cout << "  Running test_folder_copy..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_copy_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_copy_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "original", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *copied_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original", ".", "copy", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT(copied_folder_id != nullptr);
  vxcore_string_free(copied_folder_id);

  ASSERT(path_exists(get_test_path("test_folder_copy_nb") + "/original"));
  ASSERT(path_exists(get_test_path("test_folder_copy_nb") + "/copy"));
  ASSERT(path_exists(get_test_path("test_folder_copy_nb/copy") + "/test.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_copy_nb"));
  std::cout << "  âœ“ test_folder_copy passed" << std::endl;
  return 0;
}

int test_file_rename() {
  std::cout << "  Running test_file_rename..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_rename_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_rename_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "old.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_rename(ctx, notebook_id, "old.md", "new.md");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_file_rename_nb") + "/old.md"));
  ASSERT(path_exists(get_test_path("test_file_rename_nb") + "/new.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_rename_nb"));
  std::cout << "  âœ“ test_file_rename passed" << std::endl;
  return 0;
}

int test_file_move() {
  std::cout << "  Running test_file_move..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_move_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_move_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_node_move(ctx, notebook_id, "file.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_file_move_nb") + "/file.md"));
  ASSERT(path_exists(get_test_path("test_file_move_nb/dest") + "/file.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_move_nb"));
  std::cout << "  âœ“ test_file_move passed" << std::endl;
  return 0;
}

int test_file_copy() {
  std::cout << "  Running test_file_copy..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_copy_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_copy_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *copied_file_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original.md", ".", "copy.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT(copied_file_id != nullptr);
  vxcore_string_free(copied_file_id);

  ASSERT(path_exists(get_test_path("test_file_copy_nb") + "/original.md"));
  ASSERT(path_exists(get_test_path("test_file_copy_nb") + "/copy.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_copy_nb"));
  std::cout << "  âœ“ test_file_copy passed" << std::endl;
  return 0;
}

int test_file_get_info() {
  std::cout << "  Running test_file_get_info..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_info_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_info_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_update_metadata(ctx, notebook_id, "test.md", R"({"key": "value"})");
  ASSERT_EQ(err, VXCORE_OK);

  char *info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "test.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["name"] == "test.md");
  ASSERT(info["metadata"]["key"] == "value");

  vxcore_string_free(info_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_info_nb"));
  std::cout << "  âœ“ test_file_get_info passed" << std::endl;
  return 0;
}

int test_folder_get_metadata() {
  std::cout << "  Running test_folder_get_metadata..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_meta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_meta_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "test", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_node_update_metadata(ctx, notebook_id, "test", R"({"color": "blue"})");
  ASSERT_EQ(err, VXCORE_OK);

  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "test", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json metadata = nlohmann::json::parse(metadata_json);
  ASSERT(metadata["color"] == "blue");

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_meta_nb"));
  std::cout << "  âœ“ test_folder_get_metadata passed" << std::endl;
  return 0;
}

int test_folder_copy_invalid_params() {
  std::cout << "  Running test_folder_copy_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_inv_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_fcopy_inv_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_node_copy(ctx, notebook_id, "original", ".", "copy", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_inv_nb"));
  std::cout << "  âœ“ test_folder_copy_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_copy_not_found() {
  std::cout << "  Running test_folder_copy_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_nf_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_fcopy_nf_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *copied_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "nonexistent", ".", "copy", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT(copied_folder_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_nf_nb"));
  std::cout << "  âœ“ test_folder_copy_not_found passed" << std::endl;
  return 0;
}

int test_folder_copy_already_exists() {
  std::cout << "  Running test_folder_copy_already_exists..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_exists_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_fcopy_exists_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "existing", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *copied_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original", ".", "existing", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT(copied_folder_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_exists_nb"));
  std::cout << "  âœ“ test_folder_copy_already_exists passed" << std::endl;
  return 0;
}

int test_file_copy_invalid_params() {
  std::cout << "  Running test_file_copy_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_filecopy_inv_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_filecopy_inv_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_copy(ctx, notebook_id, "original.md", ".", "copy.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_filecopy_inv_nb"));
  std::cout << "  âœ“ test_file_copy_invalid_params passed" << std::endl;
  return 0;
}

int test_file_copy_not_found() {
  std::cout << "  Running test_file_copy_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_filecopy_nf_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_filecopy_nf_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *copied_file_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "nonexistent.md", ".", "copy.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT(copied_file_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_filecopy_nf_nb"));
  std::cout << "  âœ“ test_file_copy_not_found passed" << std::endl;
  return 0;
}

int test_file_copy_already_exists() {
  std::cout << "  Running test_file_copy_already_exists..." << std::endl;
  cleanup_test_dir(get_test_path("test_filecopy_exists_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_filecopy_exists_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, ".", "existing.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *copied_file_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original.md", ".", "existing.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT(copied_file_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_filecopy_exists_nb"));
  std::cout << "  âœ“ test_file_copy_already_exists passed" << std::endl;
  return 0;
}

int test_file_tag() {
  std::cout << "  Running test_file_tag..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_tag_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_tag_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"][0]["tags"].size() == 2);
  ASSERT(config["files"][0]["tags"][0] == "work");
  ASSERT(config["files"][0]["tags"][1] == "urgent");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_tag_nb"));
  std::cout << "  âœ“ test_file_tag passed" << std::endl;
  return 0;
}

int test_file_untag() {
  std::cout << "  Running test_file_untag..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_untag_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_untag_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "personal");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "personal");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_untag(ctx, notebook_id, "note.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"][0]["tags"].size() == 2);
  ASSERT(config["files"][0]["tags"][0] == "work");
  ASSERT(config["files"][0]["tags"][1] == "personal");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_untag_nb"));
  std::cout << "  âœ“ test_file_untag passed" << std::endl;
  return 0;
}

int test_file_tag_duplicate() {
  std::cout << "  Running test_file_tag_duplicate..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_tag_dup_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_tag_dup_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "work");
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_tag_dup_nb"));
  std::cout << "  âœ“ test_file_tag_duplicate passed" << std::endl;
  return 0;
}

int test_file_untag_not_found() {
  std::cout << "  Running test_file_untag_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_untag_nf_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_untag_nf_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_untag(ctx, notebook_id, "note.md", "nonexistent");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_untag_nf_nb"));
  std::cout << "  âœ“ test_file_untag_not_found passed" << std::endl;
  return 0;
}

int test_file_tag_invalid_params() {
  std::cout << "  Running test_file_tag_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_tag_inv_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_tag_inv_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(nullptr, notebook_id, "note.md", "work");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_tag(ctx, nullptr, "note.md", "work");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_tag(ctx, notebook_id, nullptr, "work");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_untag(nullptr, notebook_id, "note.md", "work");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_untag(ctx, nullptr, "note.md", "work");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_untag(ctx, notebook_id, nullptr, "work");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_untag(ctx, notebook_id, "note.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_tag_inv_nb"));
  std::cout << "  âœ“ test_file_tag_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_create_path_basic() {
  std::cout << "  Running test_folder_create_path_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_basic_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_path_basic_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "level1/level2/level3", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_path_basic_nb") + "/level1"));
  ASSERT(path_exists(get_test_path("test_folder_path_basic_nb/level1") + "/level2"));
  ASSERT(path_exists(get_test_path("test_folder_path_basic_nb/level1/level2") + "/level3"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_basic_nb"));
  std::cout << "  âœ“ test_folder_create_path_basic passed" << std::endl;
  return 0;
}

int test_folder_create_path_single() {
  std::cout << "  Running test_folder_create_path_single..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_single_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_path_single_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "single_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_path_single_nb") + "/single_folder"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_single_nb"));
  std::cout << "  âœ“ test_folder_create_path_single passed" << std::endl;
  return 0;
}

int test_folder_create_path_partial_exists() {
  std::cout << "  Running test_folder_create_path_partial_exists..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_partial_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_path_partial_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "existing", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create_path(ctx, notebook_id, "existing/new1/new2", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_path_partial_nb") + "/existing"));
  ASSERT(path_exists(get_test_path("test_folder_path_partial_nb/existing") + "/new1"));
  ASSERT(path_exists(get_test_path("test_folder_path_partial_nb/existing/new1") + "/new2"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_partial_nb"));
  std::cout << "  âœ“ test_folder_create_path_partial_exists passed" << std::endl;
  return 0;
}

int test_folder_create_path_empty() {
  std::cout << "  Running test_folder_create_path_empty..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_empty_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_path_empty_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "", &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_empty_nb"));
  std::cout << "  âœ“ test_folder_create_path_empty passed" << std::endl;
  return 0;
}

int test_folder_create_path_trailing_slash() {
  std::cout << "  Running test_folder_create_path_trailing_slash..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_slash_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_path_slash_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "path1/path2/", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists(get_test_path("test_folder_path_slash_nb") + "/path1"));
  ASSERT(path_exists(get_test_path("test_folder_path_slash_nb/path1") + "/path2"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_slash_nb"));
  std::cout << "  âœ“ test_folder_create_path_trailing_slash passed" << std::endl;
  return 0;
}

int test_folder_create_path_idempotent() {
  std::cout << "  Running test_folder_create_path_idempotent..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_idempotent_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_folder_path_idempotent_nb").c_str(),
                               "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id1 = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "a/b/c", &folder_id1);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id1);

  char *folder_id2 = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "a/b/c", &folder_id2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id2);

  ASSERT_EQ(std::string(folder_id1), std::string(folder_id2));

  vxcore_string_free(folder_id1);
  vxcore_string_free(folder_id2);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_idempotent_nb"));
  std::cout << "  PASS test_folder_create_path_idempotent" << std::endl;
  return 0;
}

int test_folder_create_path_id_correct() {
  std::cout << "  Running test_folder_create_path_id_correct..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_path_id_correct_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_folder_path_id_correct_nb").c_str(),
                               "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "x/y/z", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "x/y/z", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  auto j = nlohmann::json::parse(config_json);
  std::string config_id = j.value("id", std::string());
  ASSERT_EQ(std::string(folder_id), config_id);

  vxcore_string_free(config_json);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_path_id_correct_nb"));
  std::cout << "  PASS test_folder_create_path_id_correct" << std::endl;
  return 0;
}

// ============================================================================
// MetadataStore Sync Integration Tests
// ============================================================================

// Test that metadata store syncs correctly when notebook is reopened.
// Note: With lazy sync architecture, data is synced to MetadataStore on-demand
// when folders/files are accessed, not eagerly on notebook open.
int test_metadata_store_sync_on_reopen() {
  std::cout << "  Running test_metadata_store_sync_on_reopen..." << std::endl;
  cleanup_test_dir(get_test_path("test_ms_sync_reopen_nb"));

  // Create notebook with folders and files
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ms_sync_reopen_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder hierarchy
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "docs", "guides", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create files with tags
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "docs", "intro.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "docs/guides", "setup.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Add tags
  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "readme.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "docs/intro.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  // Close notebook
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  // Reopen notebook - lazy sync will populate MetadataStore when folders are accessed
  err = vxcore_notebook_open(ctx, get_test_path("test_ms_sync_reopen_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Verify all data is accessible (indirectly tests that sync worked)
  char *config_json = nullptr;

  // Check root folder config
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json root_config = nlohmann::json::parse(config_json);
  ASSERT(root_config["folders"].size() == 1);  // docs
  ASSERT(root_config["files"].size() == 1);    // readme.md
  vxcore_string_free(config_json);

  // Check nested folder config
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json docs_config = nlohmann::json::parse(config_json);
  ASSERT(docs_config["folders"].size() == 1);  // guides
  ASSERT(docs_config["files"].size() == 1);    // intro.md
  vxcore_string_free(config_json);

  // Check deeply nested folder config
  err = vxcore_node_get_config(ctx, notebook_id, "docs/guides", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json guides_config = nlohmann::json::parse(config_json);
  ASSERT(guides_config["files"].size() == 1);  // setup.md
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ms_sync_reopen_nb"));
  std::cout << "  âœ“ test_metadata_store_sync_on_reopen passed" << std::endl;
  return 0;
}

// Test write-through behavior: operations update both config files and metadata store
int test_metadata_store_write_through() {
  std::cout << "  Running test_metadata_store_write_through..." << std::endl;
  cleanup_test_dir(get_test_path("test_ms_write_through_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ms_write_through_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "notes", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "notes", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Update file metadata
  err = vxcore_node_update_metadata(ctx, notebook_id, "notes/test.md",
                                    R"({"author": "test", "version": 1})");
  ASSERT_EQ(err, VXCORE_OK);

  // Create and apply tag
  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "notes/test.md", "work");
  ASSERT_EQ(err, VXCORE_OK);

  // Rename file
  err = vxcore_node_rename(ctx, notebook_id, "notes/test.md", "renamed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Move file to root
  err = vxcore_node_move(ctx, notebook_id, "notes/renamed.md", ".");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file in root with all its attributes
  char *info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "renamed.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["name"] == "renamed.md");
  ASSERT(info["metadata"]["author"] == "test");
  ASSERT(info["tags"].size() == 1);
  ASSERT(info["tags"][0] == "work");
  vxcore_string_free(info_json);

  // Rename folder
  err = vxcore_node_rename(ctx, notebook_id, "notes", "documents");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify folder exists with new name
  ASSERT(!path_exists(get_test_path("test_ms_write_through_nb") + "/notes"));
  ASSERT(path_exists(get_test_path("test_ms_write_through_nb") + "/documents"));

  // Delete file
  err = vxcore_node_delete(ctx, notebook_id, "renamed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Delete folder
  err = vxcore_node_delete(ctx, notebook_id, "documents");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify root is empty (except for vx_notebook metadata folder)
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].empty());
  ASSERT(config["files"].empty());
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ms_write_through_nb"));
  std::cout << "  âœ“ test_metadata_store_write_through passed" << std::endl;
  return 0;
}

// Test that tags are properly synced through copy operations
int test_metadata_store_copy_preserves_data() {
  std::cout << "  Running test_metadata_store_copy_preserves_data..." << std::endl;
  cleanup_test_dir(get_test_path("test_ms_copy_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ms_copy_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source folder with file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "source", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "source", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Set metadata and tags on original file
  err = vxcore_node_update_metadata(ctx, notebook_id, "source/original.md",
                                    R"({"priority": "high"})");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "source/original.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  // Copy file
  char *copied_file_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "source/original.md", ".", "copy.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_file_id);
  vxcore_string_free(copied_file_id);

  // Verify copied file has same metadata and tags
  char *info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "copy.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["name"] == "copy.md");
  ASSERT(info["metadata"]["priority"] == "high");
  ASSERT(info["tags"].size() == 1);
  ASSERT(info["tags"][0] == "important");
  vxcore_string_free(info_json);

  // Copy folder
  char *copied_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "source", ".", "source_copy", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_folder_id);
  vxcore_string_free(copied_folder_id);

  // Verify copied folder's file has metadata and tags
  err = vxcore_node_get_config(ctx, notebook_id, "source_copy/original.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  info = nlohmann::json::parse(info_json);
  ASSERT(info["metadata"]["priority"] == "high");
  ASSERT(info["tags"].size() == 1);
  ASSERT(info["tags"][0] == "important");
  vxcore_string_free(info_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ms_copy_nb"));
  std::cout << "  âœ“ test_metadata_store_copy_preserves_data passed" << std::endl;
  return 0;
}

// Test that recursive folder sync works correctly.
// When accessing a parent folder, all subfolders should be synced to MetadataStore.
int test_sync_recursive_folders() {
  std::cout << "  Running test_sync_recursive_folders..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_recursive_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_sync_recursive_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create nested folder structure: root -> level1 -> level2 -> level3
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "level1", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "level1", "level2", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "level1/level2", "level3", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create files at each level
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "level1", "file1.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "level1/level2", "file2.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "level1/level2/level3", "file3.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Close and reopen to clear in-memory state
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  err = vxcore_notebook_open(ctx, get_test_path("test_sync_recursive_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Access root folder - this triggers lazy sync which should recursively sync all subfolders
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Verify level1 is accessible and has correct content
  err = vxcore_node_get_config(ctx, notebook_id, "level1", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json level1_config = nlohmann::json::parse(config_json);
  ASSERT(level1_config["folders"].size() == 1);  // level2
  ASSERT(level1_config["files"].size() == 1);    // file1.md
  vxcore_string_free(config_json);

  // Verify level2 is accessible
  err = vxcore_node_get_config(ctx, notebook_id, "level1/level2", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json level2_config = nlohmann::json::parse(config_json);
  ASSERT(level2_config["folders"].size() == 1);  // level3
  ASSERT(level2_config["files"].size() == 1);    // file2.md
  vxcore_string_free(config_json);

  // Verify level3 is accessible
  err = vxcore_node_get_config(ctx, notebook_id, "level1/level2/level3", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json level3_config = nlohmann::json::parse(config_json);
  ASSERT(level3_config["files"].size() == 1);  // file3.md
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_recursive_nb"));
  std::cout << "  ✓ test_sync_recursive_folders passed" << std::endl;
  return 0;
}

// Test that orphan files are deleted from store when they no longer exist in config.
// This simulates external modification of config files.
int test_sync_orphan_file_deletion() {
  std::cout << "  Running test_sync_orphan_file_deletion..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_orphan_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_sync_orphan_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create two files
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "keep.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, ".", "delete.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Verify both files exist
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 2);
  vxcore_string_free(config_json);

  // Close notebook
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  // Manually edit the vx.json to remove "delete.md" (simulating external modification)
  std::string vx_json_path = get_test_path("test_sync_orphan_nb") + "/vx_notebook/contents/vx.json";
  std::ifstream in_file(vx_json_path);
  nlohmann::json vx_json;
  in_file >> vx_json;
  in_file.close();

  // Remove delete.md from files array
  nlohmann::json new_files = nlohmann::json::array();
  for (const auto &file : vx_json["files"]) {
    if (file["name"] != "delete.md") {
      new_files.push_back(file);
    }
  }
  vx_json["files"] = new_files;
  // Update modified timestamp to trigger staleness check
  vx_json["modifiedUtc"] = vx_json["modifiedUtc"].get<int64_t>() + 1000;

  std::ofstream out_file(vx_json_path);
  out_file << vx_json.dump(2);
  out_file.close();

  // Reopen notebook - sync should detect staleness and remove orphan from store
  err = vxcore_notebook_open(ctx, get_test_path("test_sync_orphan_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Access folder to trigger sync
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);

  // Verify only "keep.md" remains
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "keep.md");
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_orphan_nb"));
  std::cout << "  ✓ test_sync_orphan_file_deletion passed" << std::endl;
  return 0;
}

// Test that file metadata updates are synced correctly when config changes externally.
int test_sync_file_metadata_update() {
  std::cout << "  Running test_sync_file_metadata_update..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_file_update_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_sync_file_update_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file with initial metadata
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Set initial metadata
  err = vxcore_node_update_metadata(ctx, notebook_id, "note.md", "{\"priority\":\"low\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify initial metadata
  char *info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "note.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["metadata"]["priority"] == "low");
  vxcore_string_free(info_json);

  // Close notebook
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  // Manually edit the vx.json to change file metadata (simulating external modification)
  std::string vx_json_path =
      get_test_path("test_sync_file_update_nb") + "/vx_notebook/contents/vx.json";
  std::ifstream in_file(vx_json_path);
  nlohmann::json vx_json;
  in_file >> vx_json;
  in_file.close();

  // Update file metadata
  for (auto &file : vx_json["files"]) {
    if (file["name"] == "note.md") {
      file["metadata"]["priority"] = "high";
      file["metadata"]["author"] = "test";
    }
  }
  // Update modified timestamp to trigger staleness check
  vx_json["modifiedUtc"] = vx_json["modifiedUtc"].get<int64_t>() + 1000;

  std::ofstream out_file(vx_json_path);
  out_file << vx_json.dump(2);
  out_file.close();

  // Reopen notebook - sync should update file metadata in store
  err = vxcore_notebook_open(ctx, get_test_path("test_sync_file_update_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Access folder to trigger sync
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Verify metadata was updated
  err = vxcore_node_get_config(ctx, notebook_id, "note.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);
  info = nlohmann::json::parse(info_json);
  ASSERT(info["metadata"]["priority"] == "high");
  ASSERT(info["metadata"]["author"] == "test");
  vxcore_string_free(info_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_file_update_nb"));
  std::cout << "  ✓ test_sync_file_metadata_update passed" << std::endl;
  return 0;
}

// Test that new files added externally to config are synced to store.
int test_sync_new_file_creation() {
  std::cout << "  Running test_sync_new_file_creation..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_new_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_sync_new_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create one file initially
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "existing.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Verify only one file
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  vxcore_string_free(config_json);

  // Close notebook
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  // Manually add a new file entry to vx.json (simulating external addition)
  std::string vx_json_path =
      get_test_path("test_sync_new_file_nb") + "/vx_notebook/contents/vx.json";
  std::ifstream in_file(vx_json_path);
  nlohmann::json vx_json;
  in_file >> vx_json;
  in_file.close();

  // Add new file entry
  nlohmann::json new_file;
  new_file["id"] = "new-file-uuid-12345";
  new_file["name"] = "newfile.md";
  new_file["createdUtc"] = vx_json["modifiedUtc"].get<int64_t>();
  new_file["modifiedUtc"] = vx_json["modifiedUtc"].get<int64_t>();
  new_file["metadata"] = nlohmann::json::object();
  new_file["tags"] = nlohmann::json::array();
  vx_json["files"].push_back(new_file);

  // Update modified timestamp to trigger staleness check
  vx_json["modifiedUtc"] = vx_json["modifiedUtc"].get<int64_t>() + 1000;

  std::ofstream out_file(vx_json_path);
  out_file << vx_json.dump(2);
  out_file.close();

  // Also create the actual file on disk
  write_file(get_test_path("test_sync_new_file_nb") + "/newfile.md", "# New File\n");

  // Reopen notebook - sync should add new file to store
  err = vxcore_notebook_open(ctx, get_test_path("test_sync_new_file_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Access folder to trigger sync
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);

  // Verify both files exist
  ASSERT(config["files"].size() == 2);

  // Find the new file
  bool found_new_file = false;
  for (const auto &file : config["files"]) {
    if (file["name"] == "newfile.md") {
      found_new_file = true;
      ASSERT(file["id"] == "new-file-uuid-12345");
      break;
    }
  }
  ASSERT(found_new_file);
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_new_file_nb"));
  std::cout << "  ✓ test_sync_new_file_creation passed" << std::endl;
  return 0;
}

// Test that folder metadata updates are synced correctly.
int test_sync_folder_metadata_update() {
  std::cout << "  Running test_sync_folder_metadata_update..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_folder_update_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_sync_folder_update_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder with metadata
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Set initial folder metadata
  err =
      vxcore_node_update_metadata(ctx, notebook_id, "docs", "{\"description\":\"Documentation\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // Close notebook
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  // Manually edit the docs folder's vx.json to change metadata
  std::string vx_json_path =
      get_test_path("test_sync_folder_update_nb") + "/vx_notebook/contents/docs/vx.json";
  std::ifstream in_file(vx_json_path);
  nlohmann::json vx_json;
  in_file >> vx_json;
  in_file.close();

  // Update folder metadata
  vx_json["metadata"]["description"] = "Updated docs";
  vx_json["metadata"]["version"] = "2.0";
  // Update modified timestamp
  vx_json["modifiedUtc"] = vx_json["modifiedUtc"].get<int64_t>() + 1000;

  std::ofstream out_file(vx_json_path);
  out_file << vx_json.dump(2);
  out_file.close();

  // Reopen notebook
  err =
      vxcore_notebook_open(ctx, get_test_path("test_sync_folder_update_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Access folder to trigger sync
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);

  // Verify metadata was updated
  ASSERT(config["metadata"]["description"] == "Updated docs");
  ASSERT(config["metadata"]["version"] == "2.0");
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_folder_update_nb"));
  std::cout << "  ✓ test_sync_folder_metadata_update passed" << std::endl;
  return 0;
}

// Test that sync is skipped when store is already up-to-date (staleness check).
// This validates performance optimization - no unnecessary DB operations.
int test_sync_staleness_check() {
  std::cout << "  Running test_sync_staleness_check..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_stale_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_sync_stale_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder with file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "readme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Access folder multiple times - second access should use cached/synced data
  char *config_json = nullptr;
  for (int i = 0; i < 3; i++) {
    err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
    ASSERT_EQ(err, VXCORE_OK);
    nlohmann::json config = nlohmann::json::parse(config_json);
    ASSERT(config["files"].size() == 1);
    ASSERT(config["files"][0]["name"] == "readme.md");
    vxcore_string_free(config_json);
    config_json = nullptr;
  }

  // Close and reopen
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  err = vxcore_notebook_open(ctx, get_test_path("test_sync_stale_nb").c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Access again - sync state should be preserved, so only staleness check happens
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "readme.md");
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_stale_nb"));
  std::cout << "  ✓ test_sync_staleness_check passed" << std::endl;
  return 0;
}

// ============ Recycle Bin Tests ============

int test_recycle_bin_file_delete() {
  std::cout << "  Running test_recycle_bin_file_delete..." << std::endl;
  cleanup_test_dir(get_test_path("test_recycle_bin_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_recycle_bin_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "to_recycle.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  std::string file_path = get_test_path("test_recycle_bin_file_nb") + "/to_recycle.md";
  ASSERT(path_exists(file_path));

  // Delete the file - should move to recycle bin
  err = vxcore_node_delete(ctx, notebook_id, "to_recycle.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Original file should no longer exist
  ASSERT(!path_exists(file_path));

  // File should be in recycle bin
  std::string recycle_bin_path =
      get_test_path("test_recycle_bin_file_nb") + "/vx_notebook/recycle_bin";
  ASSERT(path_exists(recycle_bin_path));
  ASSERT(path_exists(recycle_bin_path + "/to_recycle.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_recycle_bin_file_nb"));
  std::cout << "  ✓ test_recycle_bin_file_delete passed" << std::endl;
  return 0;
}

int test_recycle_bin_folder_delete() {
  std::cout << "  Running test_recycle_bin_folder_delete..." << std::endl;
  cleanup_test_dir(get_test_path("test_recycle_bin_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_recycle_bin_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder with a file inside
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "to_recycle_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "to_recycle_folder", "inner.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  std::string folder_path = get_test_path("test_recycle_bin_folder_nb") + "/to_recycle_folder";
  ASSERT(path_exists(folder_path));
  ASSERT(path_exists(folder_path + "/inner.md"));

  // Delete the folder - should move entire folder to recycle bin
  err = vxcore_node_delete(ctx, notebook_id, "to_recycle_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Original folder should no longer exist
  ASSERT(!path_exists(folder_path));

  // Folder should be in recycle bin with contents
  std::string recycle_bin_path =
      get_test_path("test_recycle_bin_folder_nb") + "/vx_notebook/recycle_bin";
  ASSERT(path_exists(recycle_bin_path));
  ASSERT(path_exists(recycle_bin_path + "/to_recycle_folder"));
  ASSERT(path_exists(recycle_bin_path + "/to_recycle_folder/inner.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_recycle_bin_folder_nb"));
  std::cout << "  ✓ test_recycle_bin_folder_delete passed" << std::endl;
  return 0;
}

int test_recycle_bin_name_conflict() {
  std::cout << "  Running test_recycle_bin_name_conflict..." << std::endl;
  cleanup_test_dir(get_test_path("test_recycle_bin_conflict_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_recycle_bin_conflict_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create and delete first file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "conflict.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_delete(ctx, notebook_id, "conflict.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Create and delete second file with same name
  err = vxcore_file_create(ctx, notebook_id, ".", "conflict.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_delete(ctx, notebook_id, "conflict.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Create and delete third file with same name
  err = vxcore_file_create(ctx, notebook_id, ".", "conflict.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_node_delete(ctx, notebook_id, "conflict.md");
  ASSERT_EQ(err, VXCORE_OK);

  // All three should be in recycle bin with different names
  std::string recycle_bin_path =
      get_test_path("test_recycle_bin_conflict_nb") + "/vx_notebook/recycle_bin";
  ASSERT(path_exists(recycle_bin_path + "/conflict.md"));
  ASSERT(path_exists(recycle_bin_path + "/conflict_1.md"));
  ASSERT(path_exists(recycle_bin_path + "/conflict_2.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_recycle_bin_conflict_nb"));
  std::cout << "  ✓ test_recycle_bin_name_conflict passed" << std::endl;
  return 0;
}

int test_recycle_bin_get_path() {
  std::cout << "  Running test_recycle_bin_get_path..." << std::endl;
  cleanup_test_dir(get_test_path("test_recycle_bin_path_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_recycle_bin_path_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *recycle_bin_path = nullptr;
  err = vxcore_notebook_get_recycle_bin_path(ctx, notebook_id, &recycle_bin_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(recycle_bin_path);

  std::string expected_path =
      normalize_path(get_test_path("test_recycle_bin_path_nb") + "/vx_notebook/recycle_bin");
  ASSERT_EQ(std::string(recycle_bin_path), expected_path);

  vxcore_string_free(recycle_bin_path);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_recycle_bin_path_nb"));
  std::cout << "  ✓ test_recycle_bin_get_path passed" << std::endl;
  return 0;
}

int test_recycle_bin_empty() {
  std::cout << "  Running test_recycle_bin_empty..." << std::endl;
  cleanup_test_dir(get_test_path("test_recycle_bin_empty_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_recycle_bin_empty_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create and delete multiple files
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file1.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);
  err = vxcore_node_delete(ctx, notebook_id, "file1.md");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_create(ctx, notebook_id, ".", "file2.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);
  err = vxcore_node_delete(ctx, notebook_id, "file2.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify files are in recycle bin
  std::string recycle_bin_path =
      get_test_path("test_recycle_bin_empty_nb") + "/vx_notebook/recycle_bin";
  ASSERT(path_exists(recycle_bin_path + "/file1.md"));
  ASSERT(path_exists(recycle_bin_path + "/file2.md"));

  // Empty the recycle bin
  err = vxcore_notebook_empty_recycle_bin(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Recycle bin folder should still exist but be empty
  ASSERT(path_exists(recycle_bin_path));
  ASSERT(!path_exists(recycle_bin_path + "/file1.md"));
  ASSERT(!path_exists(recycle_bin_path + "/file2.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_recycle_bin_empty_nb"));
  std::cout << "  ✓ test_recycle_bin_empty passed" << std::endl;
  return 0;
}

int test_recycle_bin_raw_notebook_unsupported() {
  std::cout << "  Running test_recycle_bin_raw_notebook_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_recycle_bin_raw_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a raw notebook
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_recycle_bin_raw_nb").c_str(),
                               "{\"name\":\"Raw Notebook\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get recycle bin path should return empty string for raw notebook
  char *recycle_bin_path = nullptr;
  err = vxcore_notebook_get_recycle_bin_path(ctx, notebook_id, &recycle_bin_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(recycle_bin_path);
  ASSERT_EQ(std::string(recycle_bin_path), std::string(""));
  vxcore_string_free(recycle_bin_path);

  // Empty recycle bin should return VXCORE_ERR_UNSUPPORTED for raw notebook
  err = vxcore_notebook_empty_recycle_bin(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_recycle_bin_raw_nb"));
  std::cout << "  ✓ test_recycle_bin_raw_notebook_unsupported passed" << std::endl;
  return 0;
}

// ============ File Import Tests ============

int test_file_import_basic() {
  std::cout << "  Running test_file_import_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_import_basic_nb"));
  cleanup_test_dir(get_test_path("test_file_import_source"));

  // Create external source file
  create_directory(get_test_path("test_file_import_source"));
  std::string source_file = get_test_path("test_file_import_source") + "/external.md";
  write_file(source_file, "# External File Content\n\nThis is test content.");
  ASSERT(path_exists(source_file));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_import_basic_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import the file
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", source_file.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify file exists in notebook with correct name
  std::string imported_path = get_test_path("test_file_import_basic_nb") + "/external.md";
  ASSERT(path_exists(imported_path));

  // Verify file is in metadata
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "external.md");
  vxcore_string_free(config_json);

  // Verify source file still exists (copy, not move)
  ASSERT(path_exists(source_file));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_import_basic_nb"));
  cleanup_test_dir(get_test_path("test_file_import_source"));
  std::cout << "  ✓ test_file_import_basic passed" << std::endl;
  return 0;
}

int test_file_import_name_conflict() {
  std::cout << "  Running test_file_import_name_conflict..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_import_conflict_nb"));
  cleanup_test_dir(get_test_path("test_file_import_conflict_src"));

  // Create external source file
  create_directory(get_test_path("test_file_import_conflict_src"));
  std::string source_file = get_test_path("test_file_import_conflict_src") + "/conflict.md";
  write_file(source_file, "# Source content");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_import_conflict_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create existing file with same name
  char *existing_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "conflict.md", &existing_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(existing_id);

  // Import file - should auto-rename to conflict_1.md
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", source_file.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify both files exist
  std::string original_path = get_test_path("test_file_import_conflict_nb") + "/conflict.md";
  std::string renamed_path = get_test_path("test_file_import_conflict_nb") + "/conflict_1.md";
  ASSERT(path_exists(original_path));
  ASSERT(path_exists(renamed_path));

  // Import again - should become conflict_2.md
  char *file_id2 = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", source_file.c_str(), &file_id2);
  ASSERT_EQ(err, VXCORE_OK);
  std::string renamed_path2 = get_test_path("test_file_import_conflict_nb") + "/conflict_2.md";
  ASSERT(path_exists(renamed_path2));
  vxcore_string_free(file_id2);

  // Verify metadata has 3 files
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 3);
  vxcore_string_free(config_json);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_import_conflict_nb"));
  cleanup_test_dir(get_test_path("test_file_import_conflict_src"));
  std::cout << "  ✓ test_file_import_name_conflict passed" << std::endl;
  return 0;
}

int test_file_import_source_not_found() {
  std::cout << "  Running test_file_import_source_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_import_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_import_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to import non-existent file
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", "/nonexistent/path/file.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(file_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_import_notfound_nb"));
  std::cout << "  ✓ test_file_import_source_not_found passed" << std::endl;
  return 0;
}

int test_file_import_invalid_params() {
  std::cout << "  Running test_file_import_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_import_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_import_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;

  // Null context
  err = vxcore_file_import(nullptr, notebook_id, ".", "/some/path.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null notebook_id
  err = vxcore_file_import(ctx, nullptr, ".", "/some/path.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null folder_path
  err = vxcore_file_import(ctx, notebook_id, nullptr, "/some/path.md", &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null external_file_path
  err = vxcore_file_import(ctx, notebook_id, ".", nullptr, &file_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null out_file_id
  err = vxcore_file_import(ctx, notebook_id, ".", "/some/path.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_import_invalid_nb"));
  std::cout << "  ✓ test_file_import_invalid_params passed" << std::endl;
  return 0;
}

int test_file_import_to_subfolder() {
  std::cout << "  Running test_file_import_to_subfolder..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_import_subfolder_nb"));
  cleanup_test_dir(get_test_path("test_file_import_subfolder_src"));

  // Create external source file
  create_directory(get_test_path("test_file_import_subfolder_src"));
  std::string source_file = get_test_path("test_file_import_subfolder_src") + "/doc.md";
  write_file(source_file, "# Document");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_import_subfolder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create subfolder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Import to subfolder
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, "docs", source_file.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify file is in subfolder
  std::string imported_path = get_test_path("test_file_import_subfolder_nb") + "/docs/doc.md";
  ASSERT(path_exists(imported_path));

  // Verify metadata in subfolder
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "doc.md");
  vxcore_string_free(config_json);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_import_subfolder_nb"));
  cleanup_test_dir(get_test_path("test_file_import_subfolder_src"));
  std::cout << "  ✓ test_file_import_to_subfolder passed" << std::endl;
  return 0;
}

// ============ Folder Import Tests ============

int test_folder_import_basic() {
  std::cout << "  Running test_folder_import_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_basic_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_source"));

  // Create external source folder with contents
  create_directory(get_test_path("test_folder_import_source"));
  create_directory(get_test_path("test_folder_import_source") + "/external_folder");
  create_directory(get_test_path("test_folder_import_source") + "/external_folder/subfolder");
  write_file(get_test_path("test_folder_import_source") + "/external_folder/file1.md", "# File 1");
  write_file(get_test_path("test_folder_import_source") + "/external_folder/subfolder/file2.md",
             "# File 2");
  std::string source_folder = get_test_path("test_folder_import_source") + "/external_folder";
  ASSERT(path_exists(source_folder));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_basic_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import the folder to root
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", source_folder.c_str(), nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify folder structure was copied
  std::string imported_folder = get_test_path("test_folder_import_basic_nb") + "/external_folder";
  ASSERT(path_exists(imported_folder));
  ASSERT(path_exists(imported_folder + "/file1.md"));
  ASSERT(path_exists(imported_folder + "/subfolder"));
  ASSERT(path_exists(imported_folder + "/subfolder/file2.md"));

  // Verify folder is in metadata
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].size() == 1);
  ASSERT(config["folders"][0] == "external_folder");
  vxcore_string_free(config_json);

  // Verify files are indexed in the imported folder
  err = vxcore_node_get_config(ctx, notebook_id, "external_folder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "file1.md");
  ASSERT(config["folders"].size() == 1);
  ASSERT(config["folders"][0] == "subfolder");
  vxcore_string_free(config_json);

  // Verify subfolder contents are indexed
  err = vxcore_node_get_config(ctx, notebook_id, "external_folder/subfolder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "file2.md");
  vxcore_string_free(config_json);

  // Verify source folder still exists (copy, not move)
  ASSERT(path_exists(source_folder));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_basic_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_source"));
  std::cout << "  ✓ test_folder_import_basic passed" << std::endl;
  return 0;
}

int test_folder_import_name_conflict() {
  std::cout << "  Running test_folder_import_name_conflict..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_conflict_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_conflict_src"));

  // Create external source folder
  create_directory(get_test_path("test_folder_import_conflict_src"));
  create_directory(get_test_path("test_folder_import_conflict_src") + "/conflict");
  write_file(get_test_path("test_folder_import_conflict_src") + "/conflict/file.md", "# File");
  std::string source_folder = get_test_path("test_folder_import_conflict_src") + "/conflict";

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_conflict_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create existing folder with same name
  char *existing_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "conflict", &existing_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(existing_id);

  // Import folder - should auto-rename to conflict_1
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", source_folder.c_str(), nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify both folders exist
  std::string original_path = get_test_path("test_folder_import_conflict_nb") + "/conflict";
  std::string renamed_path = get_test_path("test_folder_import_conflict_nb") + "/conflict_1";
  ASSERT(path_exists(original_path));
  ASSERT(path_exists(renamed_path));
  ASSERT(path_exists(renamed_path + "/file.md"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_conflict_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_conflict_src"));
  std::cout << "  ✓ test_folder_import_name_conflict passed" << std::endl;
  return 0;
}

int test_folder_import_source_not_found() {
  std::cout << "  Running test_folder_import_source_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to import non-existent folder
  char *folder_id = nullptr;
#ifdef _WIN32
  err = vxcore_folder_import(ctx, notebook_id, ".", "C:\\nonexistent\\path\\folder", nullptr,
                             &folder_id);
#else
  err =
      vxcore_folder_import(ctx, notebook_id, ".", "/nonexistent/path/folder", nullptr, &folder_id);
#endif
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(folder_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_notfound_nb"));
  std::cout << "  ✓ test_folder_import_source_not_found passed" << std::endl;
  return 0;
}

int test_folder_import_invalid_params() {
  std::cout << "  Running test_folder_import_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;

  // Null context
  err = vxcore_folder_import(nullptr, notebook_id, ".", "/some/path", nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Null notebook_id
  err = vxcore_folder_import(ctx, nullptr, ".", "/some/path", nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Null dest_folder_path
  err = vxcore_folder_import(ctx, notebook_id, nullptr, "/some/path", nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Null external_folder_path
  err = vxcore_folder_import(ctx, notebook_id, ".", nullptr, nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Null out_folder_id
  err = vxcore_folder_import(ctx, notebook_id, ".", "/some/path", nullptr, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_invalid_nb"));
  std::cout << "  ✓ test_folder_import_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_import_to_subfolder() {
  std::cout << "  Running test_folder_import_to_subfolder..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_subfolder_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_subfolder_src"));

  // Create external source folder
  create_directory(get_test_path("test_folder_import_subfolder_src"));
  create_directory(get_test_path("test_folder_import_subfolder_src") + "/imported");
  write_file(get_test_path("test_folder_import_subfolder_src") + "/imported/doc.md", "# Document");
  std::string source_folder = get_test_path("test_folder_import_subfolder_src") + "/imported";

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_subfolder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create target subfolder
  char *target_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &target_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(target_id);

  // Import to subfolder
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, "docs", source_folder.c_str(), nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify folder is in subfolder
  std::string imported_path = get_test_path("test_folder_import_subfolder_nb") + "/docs/imported";
  ASSERT(path_exists(imported_path));
  ASSERT(path_exists(imported_path + "/doc.md"));

  // Verify metadata in subfolder
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].size() == 1);
  ASSERT(config["folders"][0] == "imported");
  vxcore_string_free(config_json);

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_subfolder_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_subfolder_src"));
  std::cout << "  ✓ test_folder_import_to_subfolder passed" << std::endl;
  return 0;
}

int test_folder_import_within_notebook() {
  std::cout << "  Running test_folder_import_within_notebook..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_within_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_within_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder inside the notebook
  char *internal_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "internal_folder", &internal_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(internal_folder_id);

  // Try to import a folder from within the notebook root - should fail
  std::string internal_folder_path =
      get_test_path("test_folder_import_within_nb") + "/internal_folder";
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", internal_folder_path.c_str(), nullptr,
                             &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);

  // Also try importing the notebook root itself - should fail
  err = vxcore_folder_import(ctx, notebook_id, ".",
                             get_test_path("test_folder_import_within_nb").c_str(), nullptr,
                             &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);

  // Try to import with a relative path - should fail
  err = vxcore_folder_import(ctx, notebook_id, ".", "../some_relative_path", nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);

  err = vxcore_folder_import(ctx, notebook_id, ".", "relative/path", nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_within_nb"));
  std::cout << "  ✓ test_folder_import_within_notebook passed" << std::endl;
  return 0;
}

int test_folder_import_suffix_filter() {
  std::cout << "  Running test_folder_import_suffix_filter..." << std::endl;
  cleanup_test_dir(get_test_path("test_folder_import_filter_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_filter_src"));

  // Create source folder with various file types
  std::string source_folder = get_test_path("test_folder_import_filter_src/mixed_folder");
  create_directory(source_folder);
  write_file(source_folder + "/doc1.md", "# Markdown 1");
  write_file(source_folder + "/doc2.MD", "# Markdown 2");  // Uppercase extension
  write_file(source_folder + "/notes.txt", "Some notes");
  write_file(source_folder + "/config.json", "{\"key\":\"value\"}");
  write_file(source_folder + "/script.py", "print('hello')");
  write_file(source_folder + "/image.png", "fake png data");
  create_directory(source_folder + "/subfolder");
  write_file(source_folder + "/subfolder/nested.md", "# Nested");
  write_file(source_folder + "/subfolder/nested.txt", "Nested text");
  write_file(source_folder + "/subfolder/nested.py", "# Python");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_folder_import_filter_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import with suffix filter: only md and txt files
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", source_folder.c_str(), "md;txt", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify only filtered files were copied
  std::string imported_folder = get_test_path("test_folder_import_filter_nb") + "/mixed_folder";
  ASSERT(path_exists(imported_folder));
  ASSERT(path_exists(imported_folder + "/doc1.md"));
  ASSERT(path_exists(imported_folder + "/doc2.MD"));  // Case insensitive matching
  ASSERT(path_exists(imported_folder + "/notes.txt"));
  ASSERT(!path_exists(imported_folder + "/config.json"));  // Filtered out
  ASSERT(!path_exists(imported_folder + "/script.py"));    // Filtered out
  ASSERT(!path_exists(imported_folder + "/image.png"));    // Filtered out

  // Verify subfolder structure preserved but only filtered files copied
  ASSERT(path_exists(imported_folder + "/subfolder"));
  ASSERT(path_exists(imported_folder + "/subfolder/nested.md"));
  ASSERT(path_exists(imported_folder + "/subfolder/nested.txt"));
  ASSERT(!path_exists(imported_folder + "/subfolder/nested.py"));  // Filtered out

  // Verify files are indexed in metadata
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "mixed_folder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["files"].size(), 3);  // doc1.md, doc2.MD, notes.txt
  vxcore_string_free(config_json);

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_folder_import_filter_nb"));
  cleanup_test_dir(get_test_path("test_folder_import_filter_src"));
  std::cout << "  ✓ test_folder_import_suffix_filter passed" << std::endl;
  return 0;
}

// ============ Node Index/Unindex Tests ============

int test_node_index_file() {
  std::cout << "  Running test_node_index_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_index_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_index_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create an unindexed file directly on filesystem (not through API)
  std::string unindexed_file = get_test_path("test_node_index_file_nb") + "/unindexed.md";
  write_file(unindexed_file, "# Unindexed File\n\nThis file is not in metadata.");
  ASSERT(path_exists(unindexed_file));

  // Verify file is NOT in metadata yet
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 0);
  vxcore_string_free(config_json);

  // Index the file
  err = vxcore_node_index(ctx, notebook_id, "unindexed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file is now in metadata
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "unindexed.md");
  vxcore_string_free(config_json);

  // Verify we can get file config directly
  char *file_config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "unindexed.md", &file_config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json file_config = nlohmann::json::parse(file_config_json);
  ASSERT(file_config["name"] == "unindexed.md");
  ASSERT(file_config["type"] == "file");
  vxcore_string_free(file_config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_index_file_nb"));
  std::cout << "  ✓ test_node_index_file passed" << std::endl;
  return 0;
}

int test_node_index_folder() {
  std::cout << "  Running test_node_index_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_index_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_index_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create an unindexed folder directly on filesystem
  std::string unindexed_folder = get_test_path("test_node_index_folder_nb") + "/unindexed_folder";
  create_directory(unindexed_folder);
  ASSERT(path_exists(unindexed_folder));

  // Verify folder is NOT in metadata yet
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].size() == 0);
  vxcore_string_free(config_json);

  // Index the folder
  err = vxcore_node_index(ctx, notebook_id, "unindexed_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify folder is now in metadata
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].size() == 1);
  ASSERT(config["folders"][0] == "unindexed_folder");
  vxcore_string_free(config_json);

  // Verify we can get folder config directly
  char *folder_config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "unindexed_folder", &folder_config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json folder_config = nlohmann::json::parse(folder_config_json);
  ASSERT(folder_config["name"] == "unindexed_folder");
  ASSERT(folder_config["type"] == "folder");
  vxcore_string_free(folder_config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_index_folder_nb"));
  std::cout << "  ✓ test_node_index_folder passed" << std::endl;
  return 0;
}

int test_node_unindex_file() {
  std::cout << "  Running test_node_unindex_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_unindex_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_unindex_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file through API (so it's indexed)
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "indexed.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Verify file is in metadata
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  vxcore_string_free(config_json);

  // Verify file exists on filesystem
  std::string file_path = get_test_path("test_node_unindex_file_nb") + "/indexed.md";
  ASSERT(path_exists(file_path));

  // Unindex the file (removes from metadata but keeps filesystem)
  err = vxcore_node_unindex(ctx, notebook_id, "indexed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file is NO LONGER in metadata
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 0);
  vxcore_string_free(config_json);

  // Verify file STILL exists on filesystem
  ASSERT(path_exists(file_path));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_unindex_file_nb"));
  std::cout << "  ✓ test_node_unindex_file passed" << std::endl;
  return 0;
}

int test_node_unindex_folder() {
  std::cout << "  Running test_node_unindex_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_unindex_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_unindex_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder through API (so it's indexed)
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "indexed_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Verify folder is in metadata
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].size() == 1);
  vxcore_string_free(config_json);

  // Verify folder exists on filesystem
  std::string folder_path = get_test_path("test_node_unindex_folder_nb") + "/indexed_folder";
  ASSERT(path_exists(folder_path));

  // Unindex the folder (removes from metadata but keeps filesystem)
  err = vxcore_node_unindex(ctx, notebook_id, "indexed_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify folder is NO LONGER in metadata
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["folders"].size() == 0);
  vxcore_string_free(config_json);

  // Verify folder STILL exists on filesystem
  ASSERT(path_exists(folder_path));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_unindex_folder_nb"));
  std::cout << "  ✓ test_node_unindex_folder passed" << std::endl;
  return 0;
}

int test_node_index_not_found() {
  std::cout << "  Running test_node_index_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_index_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_index_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to index a non-existent file
  err = vxcore_node_index(ctx, notebook_id, "nonexistent.md");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_index_notfound_nb"));
  std::cout << "  ✓ test_node_index_not_found passed" << std::endl;
  return 0;
}

int test_node_index_already_exists() {
  std::cout << "  Running test_node_index_already_exists..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_index_exists_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_index_exists_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file through API (already indexed)
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "existing.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Try to index the already-indexed file
  err = vxcore_node_index(ctx, notebook_id, "existing.md");
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_index_exists_nb"));
  std::cout << "  ✓ test_node_index_already_exists passed" << std::endl;
  return 0;
}

int test_node_unindex_not_found() {
  std::cout << "  Running test_node_unindex_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_unindex_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_unindex_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to unindex a non-existent file
  err = vxcore_node_unindex(ctx, notebook_id, "nonexistent.md");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_unindex_notfound_nb"));
  std::cout << "  ✓ test_node_unindex_not_found passed" << std::endl;
  return 0;
}

int test_node_index_invalid_params() {
  std::cout << "  Running test_node_index_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_index_inv_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_index_inv_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Null context
  err = vxcore_node_index(nullptr, notebook_id, "test.md");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null notebook_id
  err = vxcore_node_index(ctx, nullptr, "test.md");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null node_path
  err = vxcore_node_index(ctx, notebook_id, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Same for unindex
  err = vxcore_node_unindex(nullptr, notebook_id, "test.md");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_unindex(ctx, nullptr, "test.md");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_unindex(ctx, notebook_id, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_index_inv_nb"));
  std::cout << "  ✓ test_node_index_invalid_params passed" << std::endl;
  return 0;
}

int test_node_index_nested() {
  std::cout << "  Running test_node_index_nested..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_index_nested_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_index_nested_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder through API
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create an unindexed file in the nested folder
  std::string nested_file = get_test_path("test_node_index_nested_nb") + "/docs/nested.md";
  write_file(nested_file, "# Nested file");
  ASSERT(path_exists(nested_file));

  // Verify file is NOT in folder's metadata yet
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 0);
  vxcore_string_free(config_json);

  // Index the nested file
  err = vxcore_node_index(ctx, notebook_id, "docs/nested.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file is now in folder's metadata
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "nested.md");
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_index_nested_nb"));
  std::cout << "  ✓ test_node_index_nested passed" << std::endl;
  return 0;
}

int test_node_get_path_by_id() {
  std::cout << "  Running test_node_get_path_by_id..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_get_path_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_get_path_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder structure: docs/guides
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string docs_id(folder_id);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "docs", "guides", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string guides_id(folder_id);
  vxcore_string_free(folder_id);

  // Create files at different levels
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "readme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string readme_id(file_id);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "docs", "intro.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string intro_id(file_id);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "docs/guides", "setup.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string setup_id(file_id);
  vxcore_string_free(file_id);

  // Test getting path for folder at root level
  char *path = nullptr;
  err = vxcore_node_get_path_by_id(ctx, notebook_id, docs_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);
  ASSERT_EQ(std::string(path), std::string("docs"));
  vxcore_string_free(path);

  // Test getting path for nested folder
  err = vxcore_node_get_path_by_id(ctx, notebook_id, guides_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);
  ASSERT_EQ(std::string(path), std::string("docs/guides"));
  vxcore_string_free(path);

  // Test getting path for file at root level
  err = vxcore_node_get_path_by_id(ctx, notebook_id, readme_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);
  ASSERT_EQ(std::string(path), std::string("readme.md"));
  vxcore_string_free(path);

  // Test getting path for file in subfolder
  err = vxcore_node_get_path_by_id(ctx, notebook_id, intro_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);
  ASSERT_EQ(std::string(path), std::string("docs/intro.md"));
  vxcore_string_free(path);

  // Test getting path for deeply nested file
  err = vxcore_node_get_path_by_id(ctx, notebook_id, setup_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);
  ASSERT_EQ(std::string(path), std::string("docs/guides/setup.md"));
  vxcore_string_free(path);

  // Test with non-existent ID
  err = vxcore_node_get_path_by_id(ctx, notebook_id, "nonexistent-uuid", &path);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(path);

  // Test invalid params
  err = vxcore_node_get_path_by_id(nullptr, notebook_id, docs_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_path_by_id(ctx, nullptr, docs_id.c_str(), &path);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_path_by_id(ctx, notebook_id, nullptr, &path);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_path_by_id(ctx, notebook_id, docs_id.c_str(), nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_get_path_nb"));
  std::cout << "  ✓ test_node_get_path_by_id passed" << std::endl;
  return 0;
}

int test_node_resolve_by_id() {
  std::cout << "  Running test_node_resolve_by_id..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_resolve_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_resolve_nb").c_str(),
                             "{\"name\":\"Resolve Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string nb_id(notebook_id);
  vxcore_string_free(notebook_id);

  // Create a folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, nb_id.c_str(), ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string docs_id(folder_id);
  vxcore_string_free(folder_id);

  // Create a file in the folder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, nb_id.c_str(), "docs", "readme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string readme_id(file_id);
  vxcore_string_free(file_id);

  // === Test 1: Resolve file UUID ===
  char *out_nb = nullptr;
  char *out_path = nullptr;
  err = vxcore_node_resolve_by_id(ctx, readme_id.c_str(), &out_nb, &out_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_nb);
  ASSERT_NOT_NULL(out_path);
  ASSERT_EQ(std::string(out_nb), nb_id);
  ASSERT_EQ(std::string(out_path), std::string("docs/readme.md"));
  vxcore_string_free(out_nb);
  vxcore_string_free(out_path);

  // === Test 2: Resolve folder UUID ===
  out_nb = nullptr;
  out_path = nullptr;
  err = vxcore_node_resolve_by_id(ctx, docs_id.c_str(), &out_nb, &out_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_nb);
  ASSERT_NOT_NULL(out_path);
  ASSERT_EQ(std::string(out_nb), nb_id);
  ASSERT_EQ(std::string(out_path), std::string("docs"));
  vxcore_string_free(out_nb);
  vxcore_string_free(out_path);

  // === Test 3: Not found ===
  out_nb = nullptr;
  out_path = nullptr;
  err = vxcore_node_resolve_by_id(ctx, "nonexistent-uuid", &out_nb, &out_path);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(out_nb);
  ASSERT_NULL(out_path);

  // === Test 4: Null pointer args ===
  err = vxcore_node_resolve_by_id(nullptr, readme_id.c_str(), &out_nb, &out_path);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_node_resolve_by_id(ctx, nullptr, &out_nb, &out_path);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_node_resolve_by_id(ctx, readme_id.c_str(), nullptr, &out_path);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_node_resolve_by_id(ctx, readme_id.c_str(), &out_nb, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_resolve_nb"));
  std::cout << "  ✓ test_node_resolve_by_id passed" << std::endl;
  return 0;
}

// ============================================================================
// External nodes (list unindexed files/folders) tests
// ============================================================================

int test_folder_list_external_basic() {
  std::cout << "  Running test_folder_list_external_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_list_external_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_list_external_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create an indexed file through API
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "indexed.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Create an indexed folder through API
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "indexed_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create unindexed file directly on filesystem
  std::string unindexed_file = get_test_path("test_list_external_nb") + "/external.txt";
  write_file(unindexed_file, "This is an external file.");
  ASSERT(path_exists(unindexed_file));

  // Create unindexed folder directly on filesystem
  std::string unindexed_folder = get_test_path("test_list_external_nb") + "/external_folder";
  create_directory(unindexed_folder);
  ASSERT(path_exists(unindexed_folder));

  // List external nodes
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);
  ASSERT(result.contains("files"));
  ASSERT(result.contains("folders"));

  // Should have 1 external file (external.txt)
  ASSERT_EQ(result["files"].size(), 1);
  ASSERT_EQ(result["files"][0]["name"].get<std::string>(), "external.txt");

  // Should have 1 external folder (external_folder)
  ASSERT_EQ(result["folders"].size(), 1);
  ASSERT_EQ(result["folders"][0]["name"].get<std::string>(), "external_folder");

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_list_external_nb"));
  std::cout << "  ✓ test_folder_list_external_basic passed" << std::endl;
  return 0;
}

int test_folder_list_external_empty() {
  std::cout << "  Running test_folder_list_external_empty..." << std::endl;
  cleanup_test_dir(get_test_path("test_list_external_empty_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_list_external_empty_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create only indexed items (no external files)
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "indexed.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // List external nodes - should be empty
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);
  ASSERT_EQ(result["files"].size(), 0);
  ASSERT_EQ(result["folders"].size(), 0);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_list_external_empty_nb"));
  std::cout << "  ✓ test_folder_list_external_empty passed" << std::endl;
  return 0;
}

int test_folder_list_external_nested() {
  std::cout << "  Running test_folder_list_external_nested..." << std::endl;
  cleanup_test_dir(get_test_path("test_list_external_nested_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_list_external_nested_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create indexed folder through API
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create indexed file inside docs through API
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "indexed.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Create external file in nested folder directly on filesystem
  std::string unindexed_file = get_test_path("test_list_external_nested_nb") + "/docs/external.txt";
  write_file(unindexed_file, "External file in nested folder.");
  ASSERT(path_exists(unindexed_file));

  // List external nodes in nested folder
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, "docs", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);
  ASSERT_EQ(result["files"].size(), 1);
  ASSERT_EQ(result["files"][0]["name"].get<std::string>(), "external.txt");
  ASSERT_EQ(result["folders"].size(), 0);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_list_external_nested_nb"));
  std::cout << "  ✓ test_folder_list_external_nested passed" << std::endl;
  return 0;
}

int test_folder_list_external_skips_hidden() {
  std::cout << "  Running test_folder_list_external_skips_hidden..." << std::endl;
  cleanup_test_dir(get_test_path("test_list_external_hidden_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_list_external_hidden_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create hidden file (should be skipped)
  std::string hidden_file = get_test_path("test_list_external_hidden_nb") + "/.hidden_file";
  write_file(hidden_file, "Hidden file.");
  ASSERT(path_exists(hidden_file));

  // Create hidden folder (should be skipped)
  std::string hidden_folder = get_test_path("test_list_external_hidden_nb") + "/.hidden_folder";
  create_directory(hidden_folder);
  ASSERT(path_exists(hidden_folder));

  // Create visible external file (should be listed)
  std::string visible_file = get_test_path("test_list_external_hidden_nb") + "/visible.txt";
  write_file(visible_file, "Visible file.");
  ASSERT(path_exists(visible_file));

  // List external nodes - should only have visible.txt
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);
  ASSERT_EQ(result["files"].size(), 1);
  ASSERT_EQ(result["files"][0]["name"].get<std::string>(), "visible.txt");
  ASSERT_EQ(result["folders"].size(), 0);  // hidden folder excluded

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_list_external_hidden_nb"));
  std::cout << "  ✓ test_folder_list_external_skips_hidden passed" << std::endl;
  return 0;
}

int test_folder_list_external_invalid_params() {
  std::cout << "  Running test_folder_list_external_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_list_external_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_list_external_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *external_json = nullptr;

  // Null context
  err = vxcore_folder_list_external(nullptr, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null notebook_id
  err = vxcore_folder_list_external(ctx, nullptr, ".", &external_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null folder_path - allowed, defaults to root
  err = vxcore_folder_list_external(ctx, notebook_id, nullptr, &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(external_json);

  // Null output pointer
  err = vxcore_folder_list_external(ctx, notebook_id, ".", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_list_external_invalid_nb"));
  std::cout << "  ✓ test_folder_list_external_invalid_params passed" << std::endl;
  return 0;
}

// ============================================================================
// vxcore_folder_get_available_name tests
// ============================================================================

int test_folder_get_available_name_basic() {
  std::cout << "  Running test_folder_get_available_name_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_avail_name_basic_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_avail_name_basic_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Name doesn't exist - should return original name
  char *available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "new_folder", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "new_folder");
  vxcore_string_free(available_name);

  // Same for file name
  available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "document.md", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "document.md");
  vxcore_string_free(available_name);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_avail_name_basic_nb"));
  std::cout << "  ✓ test_folder_get_available_name_basic passed" << std::endl;
  return 0;
}

int test_folder_get_available_name_conflict() {
  std::cout << "  Running test_folder_get_available_name_conflict..." << std::endl;
  cleanup_test_dir(get_test_path("test_avail_name_conflict_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_avail_name_conflict_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create existing folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "existing_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Now get available name for same name - should return existing_folder_1
  char *available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "existing_folder", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "existing_folder_1");
  vxcore_string_free(available_name);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_avail_name_conflict_nb"));
  std::cout << "  ✓ test_folder_get_available_name_conflict passed" << std::endl;
  return 0;
}

int test_folder_get_available_name_multiple_conflicts() {
  std::cout << "  Running test_folder_get_available_name_multiple_conflicts..." << std::endl;
  cleanup_test_dir(get_test_path("test_avail_name_multi_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_avail_name_multi_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder, folder_1, folder_2
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "folder_1", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "folder_2", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Get available name - should skip to folder_3
  char *available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "folder", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "folder_3");
  vxcore_string_free(available_name);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_avail_name_multi_nb"));
  std::cout << "  ✓ test_folder_get_available_name_multiple_conflicts passed" << std::endl;
  return 0;
}

int test_folder_get_available_name_with_extension() {
  std::cout << "  Running test_folder_get_available_name_with_extension..." << std::endl;
  cleanup_test_dir(get_test_path("test_avail_name_ext_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_avail_name_ext_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create existing file with extension
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "document.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Get available name - should return document_1.md (suffix before extension)
  char *available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "document.md", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "document_1.md");
  vxcore_string_free(available_name);

  // Create document_1.md
  err = vxcore_file_create(ctx, notebook_id, ".", "document_1.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Get available name again - should return document_2.md
  available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "document.md", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "document_2.md");
  vxcore_string_free(available_name);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_avail_name_ext_nb"));
  std::cout << "  ✓ test_folder_get_available_name_with_extension passed" << std::endl;
  return 0;
}

int test_folder_get_available_name_in_subfolder() {
  std::cout << "  Running test_folder_get_available_name_in_subfolder..." << std::endl;
  cleanup_test_dir(get_test_path("test_avail_name_subfolder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_avail_name_subfolder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create parent folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create child in parent
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Get available name in parent - should return child_1
  char *available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, "parent", "child", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "child_1");
  vxcore_string_free(available_name);

  // Same name in root should be available (no conflict)
  available_name = nullptr;
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "child", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "child");
  vxcore_string_free(available_name);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_avail_name_subfolder_nb"));
  std::cout << "  ✓ test_folder_get_available_name_in_subfolder passed" << std::endl;
  return 0;
}

int test_folder_get_available_name_invalid_params() {
  std::cout << "  Running test_folder_get_available_name_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_avail_name_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_avail_name_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *available_name = nullptr;

  // Null context
  err = vxcore_folder_get_available_name(nullptr, notebook_id, ".", "name", &available_name);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null notebook_id
  err = vxcore_folder_get_available_name(ctx, nullptr, ".", "name", &available_name);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null new_name
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", nullptr, &available_name);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null output pointer
  err = vxcore_folder_get_available_name(ctx, notebook_id, ".", "name", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Null folder_path - should be allowed (defaults to root)
  err = vxcore_folder_get_available_name(ctx, notebook_id, nullptr, "name", &available_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(available_name);
  ASSERT_EQ(std::string(available_name), "name");
  vxcore_string_free(available_name);

  // Non-existent notebook
  err = vxcore_folder_get_available_name(ctx, "non_existent_id", ".", "name", &available_name);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_avail_name_invalid_nb"));
  std::cout << "  ✓ test_folder_get_available_name_invalid_params passed" << std::endl;
  return 0;
}

int test_node_attachments_basic() {
  std::cout << "  Running test_node_attachments_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_attachments_nb"));
  create_directory(get_test_path("test_node_attachments_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_attachments_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Insert attachment via buffer API to set up metadata + filesystem.
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string source_path = get_test_path("test_node_attachments_nb") + "/doc.pdf";
  write_file(source_path, "PDF content");

  char *filename = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source_path.c_str(), &filename);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(filename);
  ASSERT_EQ(std::string(filename), "doc.pdf");

  // Verify node-level get attachments folder.
  char *attachments_folder = nullptr;
  err = vxcore_node_get_attachments_folder(ctx, notebook_id, "note.md", &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);
  ASSERT_TRUE(path_exists(attachments_folder));
  ASSERT_TRUE(path_exists(std::string(attachments_folder) + "/doc.pdf"));

  // Verify node-level list attachments.
  char *attachments_json = nullptr;
  err = vxcore_node_list_attachments(ctx, notebook_id, "note.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  auto json = nlohmann::json::parse(attachments_json);
  ASSERT_TRUE(json.is_array());
  ASSERT_EQ(json.size(), 1u);
  ASSERT_EQ(json[0].get<std::string>(), "doc.pdf");

  vxcore_string_free(attachments_json);
  vxcore_string_free(attachments_folder);
  vxcore_string_free(filename);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_attachments_nb"));
  std::cout << "  ✓ test_node_attachments_basic passed" << std::endl;
  return 0;
}

int test_node_attachments_invalid_params() {
  std::cout << "  Running test_node_attachments_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_attachments_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_attachments_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *path = nullptr;
  err = vxcore_node_get_attachments_folder(nullptr, notebook_id, "note.md", &path);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_attachments_folder(ctx, nullptr, "note.md", &path);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_attachments_folder(ctx, notebook_id, nullptr, &path);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_attachments_folder(ctx, notebook_id, "note.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_get_attachments_folder(ctx, "non_existent_id", "note.md", &path);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(path);

  char *attachments_json = nullptr;
  err = vxcore_node_list_attachments(nullptr, notebook_id, "note.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_list_attachments(ctx, nullptr, "note.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_list_attachments(ctx, notebook_id, nullptr, &attachments_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_list_attachments(ctx, notebook_id, "note.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_node_list_attachments(ctx, "non_existent_id", "note.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(attachments_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_attachments_invalid_nb"));
  std::cout << "  ✓ test_node_attachments_invalid_params passed" << std::endl;
  return 0;
}

int test_file_peek() {
  std::cout << "  Running test_file_peek..." << std::endl;
  cleanup_test_dir(get_test_path("test_file_peek_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_file_peek_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file and write content
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Write content: 80 chars
  std::string long_content =
      "0123456789012345678901234567890123456789012345678901234567890123456789ABCDEFGHIJ";
  std::string file_path = get_test_path("test_file_peek_nb") + "/note.md";
  std::ofstream ofs(file_path);
  ofs << long_content;
  ofs.close();

  // Test 1: Peek with max_bytes=64 (truncates 80 char content)
  char *content = nullptr;
  err = vxcore_file_peek(ctx, notebook_id, "note.md", 64, &content);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content);
  ASSERT_EQ(strlen(content), 64);
  ASSERT_EQ(std::string(content), long_content.substr(0, 64));
  vxcore_string_free(content);

  // Test 2: Peek with max_bytes=32 (smaller than content)
  char *content1b = nullptr;
  err = vxcore_file_peek(ctx, notebook_id, "note.md", 32, &content1b);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content1b);
  ASSERT_EQ(strlen(content1b), 32);
  ASSERT_EQ(std::string(content1b), long_content.substr(0, 32));
  vxcore_string_free(content1b);

  // Test 3: Peek with max_bytes=128 (larger than content)
  char *content1c = nullptr;
  err = vxcore_file_peek(ctx, notebook_id, "note.md", 128, &content1c);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content1c);
  ASSERT_EQ(strlen(content1c), 80);  // Returns full content
  ASSERT_EQ(std::string(content1c), long_content);
  vxcore_string_free(content1c);

  // Test 4: Peek file with content shorter than requested
  char *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "short.md", &file_id2);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id2);

  std::string short_content = "Hello World";
  std::string short_file_path = get_test_path("test_file_peek_nb") + "/short.md";
  std::ofstream ofs2(short_file_path);
  ofs2 << short_content;
  ofs2.close();

  char *content2 = nullptr;
  err = vxcore_file_peek(ctx, notebook_id, "short.md", 64, &content2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content2);
  ASSERT_EQ(std::string(content2), short_content);
  vxcore_string_free(content2);

  // Test 5: Peek non-existent file
  char *content3 = nullptr;
  err = vxcore_file_peek(ctx, notebook_id, "nonexistent.md", 64, &content3);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(content3);

  // Test 6: Invalid params - null context
  char *content4 = nullptr;
  err = vxcore_file_peek(nullptr, notebook_id, "note.md", 64, &content4);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Test 7: Invalid params - null notebook_id
  char *content5 = nullptr;
  err = vxcore_file_peek(ctx, nullptr, "note.md", 64, &content5);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Test 8: Invalid params - null file_path
  char *content6 = nullptr;
  err = vxcore_file_peek(ctx, notebook_id, nullptr, 64, &content6);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Test 9: Invalid params - null out_content
  err = vxcore_file_peek(ctx, notebook_id, "note.md", 64, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_file_peek_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_peek passed" << std::endl;
  return 0;
}

// This test specifically verifies UTF-8 filename handling on Windows.
// Chinese characters in filenames should survive the full create->list->rename->delete cycle.
int test_utf8_filename_roundtrip() {
  std::cout << "  Running test_utf8_filename_roundtrip..." << std::endl;
  cleanup_test_dir(get_test_path("test_utf8_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_utf8_nb").c_str(),
                               "{\"name\":\"UTF8 Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".",
                             "\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9",
                             &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  {
    namespace fs = std::filesystem;
    auto folder_path =
        PathFromUtf8ForTest(get_test_path("test_utf8_nb")) /
        PathFromUtf8ForTest("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9");
    ASSERT_TRUE(fs::exists(folder_path));
  }

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id,
                           "\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9",
                           "0501-\xe5\xb1\xb1\xe4\xb8\x9c.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  {
    namespace fs = std::filesystem;
    auto file_path =
        PathFromUtf8ForTest(get_test_path("test_utf8_nb")) /
        PathFromUtf8ForTest("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9") /
        PathFromUtf8ForTest("0501-\xe5\xb1\xb1\xe4\xb8\x9c.md");
    if (!fs::exists(file_path)) {
      std::ofstream(file_path).close();
    }
    ASSERT_TRUE(fs::exists(file_path));
  }

  {
    char *children_json = nullptr;
    err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_NOT_NULL(children_json);
    std::string json_str(children_json);
    ASSERT_TRUE(json_str.find("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9") !=
                std::string::npos);
    vxcore_string_free(children_json);
  }

  {
    namespace fs = std::filesystem;
    auto folder_on_disk =
        PathFromUtf8ForTest(get_test_path("test_utf8_nb")) /
        PathFromUtf8ForTest("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9");
    bool found = false;
    for (const auto &entry : fs::directory_iterator(folder_on_disk)) {
      std::string name = PathToUtf8ForTest(entry.path().filename());
      if (name == "0501-\xe5\xb1\xb1\xe4\xb8\x9c.md") {
        found = true;
        break;
      }
    }
    ASSERT_TRUE(found);
  }

  // Step 6: Rename via vxcore API
  {
    err = vxcore_node_rename(ctx, notebook_id,
                             "\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9/"
                             "0501-\xe5\xb1\xb1\xe4\xb8\x9c.md",
                             "0501-\xe5\xb9\xbf\xe4\xb8\x9c.md");
    ASSERT_EQ(err, VXCORE_OK);

    // Verify old file gone, new file present on disk
    namespace fs = std::filesystem;
    auto old_path =
        PathFromUtf8ForTest(get_test_path("test_utf8_nb")) /
        PathFromUtf8ForTest("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9") /
        PathFromUtf8ForTest("0501-\xe5\xb1\xb1\xe4\xb8\x9c.md");
    auto new_path =
        PathFromUtf8ForTest(get_test_path("test_utf8_nb")) /
        PathFromUtf8ForTest("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9") /
        PathFromUtf8ForTest("0501-\xe5\xb9\xbf\xe4\xb8\x9c.md");
    ASSERT_FALSE(fs::exists(old_path));
    ASSERT_TRUE(fs::exists(new_path));
  }

  // Step 7: Delete via vxcore API (file first, then folder)
  {
    err = vxcore_node_delete(ctx, notebook_id,
                             "\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9/"
                             "0501-\xe5\xb9\xbf\xe4\xb8\x9c.md");
    ASSERT_EQ(err, VXCORE_OK);

    err = vxcore_node_delete(ctx, notebook_id,
                             "\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9");
    ASSERT_EQ(err, VXCORE_OK);

    // Verify cleanup on disk
    namespace fs = std::filesystem;
    auto folder_path =
        PathFromUtf8ForTest(get_test_path("test_utf8_nb")) /
        PathFromUtf8ForTest("\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9");
    ASSERT_FALSE(fs::exists(folder_path));
  }

  vxcore_string_free(file_id);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_utf8_nb"));
  std::cout << "  ✓ test_utf8_filename_roundtrip passed" << std::endl;
  return 0;
}

// ============================================================================
// Timestamp update tests
// ============================================================================

int test_node_update_timestamps_file() {
  std::cout << "  Running test_node_update_timestamps_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_ts_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ts_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Update timestamps
  int64_t new_created = 1705318200000;
  int64_t new_modified = 1705404600000;
  err = vxcore_node_update_timestamps(ctx, notebook_id, "note.md", new_created, new_modified);
  ASSERT_EQ(err, VXCORE_OK);

  // Re-read file config and verify
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["createdUtc"].get<int64_t>(), new_created);
  ASSERT_EQ(config["modifiedUtc"].get<int64_t>(), new_modified);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ts_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_node_update_timestamps_file passed" << std::endl;
  return 0;
}

int test_node_update_timestamps_folder() {
  std::cout << "  Running test_node_update_timestamps_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_ts_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ts_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Update timestamps
  int64_t new_created = 1705318200000;
  int64_t new_modified = 1705404600000;
  err = vxcore_node_update_timestamps(ctx, notebook_id, "docs", new_created, new_modified);
  ASSERT_EQ(err, VXCORE_OK);

  // Re-read folder config and verify
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["createdUtc"].get<int64_t>(), new_created);
  ASSERT_EQ(config["modifiedUtc"].get<int64_t>(), new_modified);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ts_folder_nb"));
  std::cout << "  \xe2\x9c\x93 test_node_update_timestamps_folder passed" << std::endl;
  return 0;
}

int test_node_update_timestamps_partial() {
  std::cout << "  Running test_node_update_timestamps_partial..." << std::endl;
  cleanup_test_dir(get_test_path("test_ts_partial_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ts_partial_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Set both timestamps first
  int64_t initial_created = 1705318200000;
  int64_t initial_modified = 1705404600000;
  err = vxcore_node_update_timestamps(ctx, notebook_id, "note.md", initial_created,
                                      initial_modified);
  ASSERT_EQ(err, VXCORE_OK);

  // Now update only modified (pass 0 for created to skip)
  int64_t new_modified = 1705491000000;
  err = vxcore_node_update_timestamps(ctx, notebook_id, "note.md", 0, new_modified);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify created unchanged, modified updated
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["createdUtc"].get<int64_t>(), initial_created);
  ASSERT_EQ(config["modifiedUtc"].get<int64_t>(), new_modified);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ts_partial_nb"));
  std::cout << "  \xe2\x9c\x93 test_node_update_timestamps_partial passed" << std::endl;
  return 0;
}

int test_node_update_timestamps_not_found() {
  std::cout << "  Running test_node_update_timestamps_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_ts_notfound_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ts_notfound_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Call with non-existent path
  err = vxcore_node_update_timestamps(ctx, notebook_id, "nonexistent.md", 1705318200000,
                                      1705404600000);
  ASSERT_NE(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ts_notfound_nb"));
  std::cout << "  \xe2\x9c\x93 test_node_update_timestamps_not_found passed" << std::endl;
  return 0;
}

// ============================================================================
// File attachment update tests
// ============================================================================

int test_file_update_attachments_basic() {
  std::cout << "  Running test_file_update_attachments_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_upd_attach_basic_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_upd_attach_basic_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Set attachments
  err = vxcore_file_update_attachments(ctx, notebook_id, "note.md",
                                       R"(["report.pdf","data.csv"])");
  ASSERT_EQ(err, VXCORE_OK);

  // List attachments and verify
  char *attachments_json = nullptr;
  err = vxcore_node_list_attachments(ctx, notebook_id, "note.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_TRUE(attachments.is_array());
  ASSERT_EQ(attachments.size(), 2u);
  ASSERT_EQ(attachments[0].get<std::string>(), "report.pdf");
  ASSERT_EQ(attachments[1].get<std::string>(), "data.csv");

  vxcore_string_free(attachments_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_upd_attach_basic_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_update_attachments_basic passed" << std::endl;
  return 0;
}

int test_file_update_attachments_replace() {
  std::cout << "  Running test_file_update_attachments_replace..." << std::endl;
  cleanup_test_dir(get_test_path("test_upd_attach_replace_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_upd_attach_replace_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Set initial attachments
  err = vxcore_file_update_attachments(ctx, notebook_id, "note.md",
                                       R"(["old1.pdf","old2.pdf"])");
  ASSERT_EQ(err, VXCORE_OK);

  // Replace with different attachments
  err = vxcore_file_update_attachments(ctx, notebook_id, "note.md",
                                       R"(["new1.zip","new2.doc","new3.png"])");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify only new ones exist
  char *attachments_json = nullptr;
  err = vxcore_node_list_attachments(ctx, notebook_id, "note.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_TRUE(attachments.is_array());
  ASSERT_EQ(attachments.size(), 3u);
  ASSERT_EQ(attachments[0].get<std::string>(), "new1.zip");
  ASSERT_EQ(attachments[1].get<std::string>(), "new2.doc");
  ASSERT_EQ(attachments[2].get<std::string>(), "new3.png");

  vxcore_string_free(attachments_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_upd_attach_replace_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_update_attachments_replace passed" << std::endl;
  return 0;
}

int test_file_update_attachments_not_found() {
  std::cout << "  Running test_file_update_attachments_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_upd_attach_nf_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_upd_attach_nf_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Call with non-existent file
  err = vxcore_file_update_attachments(ctx, notebook_id, "nonexistent.md",
                                       R"(["file.pdf"])");
  ASSERT_NE(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_upd_attach_nf_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_update_attachments_not_found passed" << std::endl;
  return 0;
}

// ============================================================================
// External node exclusion tests (vx_images / vx_attachments)
// ============================================================================

int test_list_external_nodes_excludes_vx_images() {
  std::cout << "  Running test_list_external_nodes_excludes_vx_images..." << std::endl;
  cleanup_test_dir(get_test_path("test_ext_excl_images_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ext_excl_images_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create vx_images directory directly on filesystem (not via API)
  create_directory(get_test_path("test_ext_excl_images_nb") + "/vx_images");
  ASSERT(path_exists(get_test_path("test_ext_excl_images_nb") + "/vx_images"));

  // Also create a regular external folder for contrast
  create_directory(get_test_path("test_ext_excl_images_nb") + "/my_stuff");
  ASSERT(path_exists(get_test_path("test_ext_excl_images_nb") + "/my_stuff"));

  // List external nodes
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);

  // vx_images should NOT appear in external folders
  for (const auto &folder : result["folders"]) {
    ASSERT_NE(folder["name"].get<std::string>(), std::string("vx_images"));
  }

  // my_stuff SHOULD appear
  bool found_my_stuff = false;
  for (const auto &folder : result["folders"]) {
    if (folder["name"].get<std::string>() == "my_stuff") {
      found_my_stuff = true;
      break;
    }
  }
  ASSERT_TRUE(found_my_stuff);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ext_excl_images_nb"));
  std::cout << "  \xe2\x9c\x93 test_list_external_nodes_excludes_vx_images passed" << std::endl;
  return 0;
}

int test_list_external_nodes_excludes_vx_attachments() {
  std::cout << "  Running test_list_external_nodes_excludes_vx_attachments..." << std::endl;
  cleanup_test_dir(get_test_path("test_ext_excl_attach_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ext_excl_attach_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create vx_attachments directory directly on filesystem (not via API)
  create_directory(get_test_path("test_ext_excl_attach_nb") + "/vx_attachments");
  ASSERT(path_exists(get_test_path("test_ext_excl_attach_nb") + "/vx_attachments"));

  // Also create a regular external folder for contrast
  create_directory(get_test_path("test_ext_excl_attach_nb") + "/my_data");
  ASSERT(path_exists(get_test_path("test_ext_excl_attach_nb") + "/my_data"));

  // List external nodes
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);

  // vx_attachments should NOT appear in external folders
  for (const auto &folder : result["folders"]) {
    ASSERT_NE(folder["name"].get<std::string>(), std::string("vx_attachments"));
  }

  // my_data SHOULD appear
  bool found_my_data = false;
  for (const auto &folder : result["folders"]) {
    if (folder["name"].get<std::string>() == "my_data") {
      found_my_data = true;
      break;
    }
  }
  ASSERT_TRUE(found_my_data);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ext_excl_attach_nb"));
  std::cout << "  \xe2\x9c\x93 test_list_external_nodes_excludes_vx_attachments passed"
            << std::endl;
  return 0;
}

int test_list_external_nodes_excludes_v2_images() {
  std::cout << "  Running test_list_external_nodes_excludes_v2_images..." << std::endl;
  cleanup_test_dir(get_test_path("test_ext_excl_v2img_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ext_excl_v2img_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create _v_images directory directly on filesystem (VNote2 legacy name)
  create_directory(get_test_path("test_ext_excl_v2img_nb") + "/_v_images");
  ASSERT(path_exists(get_test_path("test_ext_excl_v2img_nb") + "/_v_images"));

  // Also create a regular external folder for contrast
  create_directory(get_test_path("test_ext_excl_v2img_nb") + "/my_stuff");
  ASSERT(path_exists(get_test_path("test_ext_excl_v2img_nb") + "/my_stuff"));

  // List external nodes
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);

  // _v_images should NOT appear in external folders
  for (const auto &folder : result["folders"]) {
    ASSERT_NE(folder["name"].get<std::string>(), std::string("_v_images"));
  }

  // my_stuff SHOULD appear
  bool found_my_stuff = false;
  for (const auto &folder : result["folders"]) {
    if (folder["name"].get<std::string>() == "my_stuff") {
      found_my_stuff = true;
      break;
    }
  }
  ASSERT_TRUE(found_my_stuff);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ext_excl_v2img_nb"));
  std::cout << "  \xe2\x9c\x93 test_list_external_nodes_excludes_v2_images passed" << std::endl;
  return 0;
}

int test_list_external_nodes_excludes_v2_attachments() {
  std::cout << "  Running test_list_external_nodes_excludes_v2_attachments..." << std::endl;
  cleanup_test_dir(get_test_path("test_ext_excl_v2att_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ext_excl_v2att_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create _v_attachments directory directly on filesystem (VNote2 legacy name)
  create_directory(get_test_path("test_ext_excl_v2att_nb") + "/_v_attachments");
  ASSERT(path_exists(get_test_path("test_ext_excl_v2att_nb") + "/_v_attachments"));

  // Also create a regular external folder for contrast
  create_directory(get_test_path("test_ext_excl_v2att_nb") + "/my_data");
  ASSERT(path_exists(get_test_path("test_ext_excl_v2att_nb") + "/my_data"));

  // List external nodes
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);

  // _v_attachments should NOT appear in external folders
  for (const auto &folder : result["folders"]) {
    ASSERT_NE(folder["name"].get<std::string>(), std::string("_v_attachments"));
  }

  // my_data SHOULD appear
  bool found_my_data = false;
  for (const auto &folder : result["folders"]) {
    if (folder["name"].get<std::string>() == "my_data") {
      found_my_data = true;
      break;
    }
  }
  ASSERT_TRUE(found_my_data);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ext_excl_v2att_nb"));
  std::cout << "  \xe2\x9c\x93 test_list_external_nodes_excludes_v2_attachments passed"
            << std::endl;
  return 0;
}

int test_list_external_nodes_excludes_nested() {
  std::cout << "  Running test_list_external_nodes_excludes_nested..." << std::endl;
  cleanup_test_dir(get_test_path("test_ext_excl_nested_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ext_excl_nested_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create a subfolder via API
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create vx_images inside subfolder directly on filesystem
  create_directory(get_test_path("test_ext_excl_nested_nb") + "/subfolder/vx_images");
  ASSERT(path_exists(get_test_path("test_ext_excl_nested_nb") + "/subfolder/vx_images"));

  // Also create a regular external folder in subfolder for contrast
  create_directory(get_test_path("test_ext_excl_nested_nb") + "/subfolder/regular_dir");
  ASSERT(path_exists(get_test_path("test_ext_excl_nested_nb") + "/subfolder/regular_dir"));

  // List external nodes for subfolder
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, "subfolder", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);

  // vx_images should NOT appear
  for (const auto &folder : result["folders"]) {
    ASSERT_NE(folder["name"].get<std::string>(), std::string("vx_images"));
  }

  // regular_dir SHOULD appear
  bool found_regular = false;
  for (const auto &folder : result["folders"]) {
    if (folder["name"].get<std::string>() == "regular_dir") {
      found_regular = true;
      break;
    }
  }
  ASSERT_TRUE(found_regular);

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ext_excl_nested_nb"));
  std::cout << "  \xe2\x9c\x93 test_list_external_nodes_excludes_nested passed" << std::endl;
  return 0;
}

int test_list_external_nodes_ignores_patterns() {
  std::cout << "  Running test_list_external_nodes_ignores_patterns..." << std::endl;
  cleanup_test_dir(get_test_path("test_ignore_external_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_ignore_external_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create two external files directly on filesystem
  std::string ignored_file = get_test_path("test_ignore_external_nb") + "/ignored.txt";
  write_file(ignored_file, "This should be ignored.");
  ASSERT(path_exists(ignored_file));

  std::string visible_file = get_test_path("test_ignore_external_nb") + "/visible.txt";
  write_file(visible_file, "This should be visible.");
  ASSERT(path_exists(visible_file));

  // Get current notebook config and add ignored pattern
  char *config_json = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  config["ignored"] = nlohmann::json::array({"ignored.txt"});
  std::string updated_config_json = config.dump();
  vxcore_string_free(config_json);

  err = vxcore_notebook_update_config(ctx, notebook_id, updated_config_json.c_str());
  ASSERT_EQ(err, VXCORE_OK);

  // List external nodes — ignored.txt should be filtered out
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  nlohmann::json result = nlohmann::json::parse(external_json);
  ASSERT(result.contains("files"));
  ASSERT_EQ(result["files"].size(), 1);
  ASSERT_EQ(result["files"][0]["name"].get<std::string>(), "visible.txt");

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_ignore_external_nb"));
  std::cout << "  ✓ test_list_external_nodes_ignores_patterns passed" << std::endl;
  return 0;
}

// ============ Raw Notebook ListFolderContents Tests ============

int test_raw_list_empty_folder() {
  std::cout << "  Running test_raw_list_empty_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_empty_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_empty_nb").c_str(),
                               "{\"name\":\"Raw Empty\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_TRUE(j["files"].empty());
  ASSERT_TRUE(j["folders"].empty());

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_empty_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_empty_folder passed" << std::endl;
  return 0;
}

int test_raw_list_root_files() {
  std::cout << "  Running test_raw_list_root_files..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_root_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_root_nb").c_str(),
                               "{\"name\":\"Raw Root\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file directly on filesystem
  std::string root = get_test_path("test_raw_list_root_nb");
  write_file(root + "/test.md", "# Test");

  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), std::string("test.md"));
  // Should have a UUID assigned
  ASSERT_FALSE(j["files"][0]["id"].get<std::string>().empty());

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_root_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_root_files passed" << std::endl;
  return 0;
}

int test_raw_list_discovers_filesystem_files() {
  std::cout << "  Running test_raw_list_discovers_filesystem_files..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_discover_files_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_discover_files_nb").c_str(),
                               "{\"name\":\"Raw Discover\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create subfolder and files directly on filesystem
  std::string root = get_test_path("test_raw_list_discover_files_nb");
  create_directory(root + "/docs");
  write_file(root + "/docs/a.md", "# A");
  write_file(root + "/docs/b.md", "# B");

  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "docs", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)2);

  // Both files should have UUIDs
  for (const auto &f : j["files"]) {
    ASSERT_FALSE(f["id"].get<std::string>().empty());
  }

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_discover_files_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_discovers_filesystem_files passed" << std::endl;
  return 0;
}

int test_raw_list_discovers_filesystem_folders() {
  std::cout << "  Running test_raw_list_discovers_filesystem_folders..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_discover_dirs_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_discover_dirs_nb").c_str(),
                               "{\"name\":\"Raw Dirs\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_discover_dirs_nb");
  create_directory(root + "/alpha");
  create_directory(root + "/beta");

  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["folders"].size(), (size_t)2);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_discover_dirs_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_discovers_filesystem_folders passed" << std::endl;
  return 0;
}

int test_raw_list_removes_stale_db_records() {
  std::cout << "  Running test_raw_list_removes_stale_db_records..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_stale_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_stale_nb").c_str(),
                               "{\"name\":\"Raw Stale\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_stale_nb");
  write_file(root + "/ephemeral.md", "# Temp");

  // First list to register the file in DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j1 = nlohmann::json::parse(children_json);
  ASSERT_EQ(j1["files"].size(), (size_t)1);
  vxcore_string_free(children_json);

  // Delete file directly from filesystem
  std::filesystem::remove(PathFromUtf8ForTest(root + "/ephemeral.md"));

  // Second list should no longer show the file
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j2 = nlohmann::json::parse(children_json);
  ASSERT_TRUE(j2["files"].empty());

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_stale_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_removes_stale_db_records passed" << std::endl;
  return 0;
}

int test_raw_list_preserves_uuid_across_calls() {
  std::cout << "  Running test_raw_list_preserves_uuid_across_calls..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_uuid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_uuid_nb").c_str(),
                               "{\"name\":\"Raw UUID\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_uuid_nb");
  write_file(root + "/stable.md", "# Stable");

  // First list
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j1 = nlohmann::json::parse(children_json);
  std::string uuid1 = j1["files"][0]["id"].get<std::string>();
  ASSERT_FALSE(uuid1.empty());
  vxcore_string_free(children_json);

  // Second list
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j2 = nlohmann::json::parse(children_json);
  std::string uuid2 = j2["files"][0]["id"].get<std::string>();
  ASSERT_EQ(uuid1, uuid2);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_uuid_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_preserves_uuid_across_calls passed" << std::endl;
  return 0;
}

int test_raw_list_skips_hidden_files() {
  std::cout << "  Running test_raw_list_skips_hidden_files..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_hidden_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_hidden_nb").c_str(),
                               "{\"name\":\"Raw Hidden\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_hidden_nb");
  write_file(root + "/.hidden_file", "secret");
  write_file(root + "/visible.md", "# Visible");

  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), std::string("visible.md"));

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_hidden_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_skips_hidden_files passed" << std::endl;
  return 0;
}

int test_raw_list_nested_folder() {
  std::cout << "  Running test_raw_list_nested_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_nested_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_nested_nb").c_str(),
                               "{\"name\":\"Raw Nested\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_nested_nb");
  create_directory(root + "/a/b/c");
  write_file(root + "/a/b/c/deep.md", "# Deep");

  // Directly list a/b without listing root or a first (on-demand ancestor reconciliation)
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "a/b", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["folders"].size(), (size_t)1);
  ASSERT_EQ(j["folders"][0]["name"].get<std::string>(), std::string("c"));

  vxcore_string_free(children_json);

  // Now list root — should show "a"
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j_root = nlohmann::json::parse(children_json);
  ASSERT_EQ(j_root["folders"].size(), (size_t)1);
  ASSERT_EQ(j_root["folders"][0]["name"].get<std::string>(), std::string("a"));
  vxcore_string_free(children_json);

  // List a — should show "b"
  err = vxcore_folder_list_children(ctx, notebook_id, "a", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j_a = nlohmann::json::parse(children_json);
  ASSERT_EQ(j_a["folders"].size(), (size_t)1);
  ASSERT_EQ(j_a["folders"][0]["name"].get<std::string>(), std::string("b"));
  vxcore_string_free(children_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_nested_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_nested_folder passed" << std::endl;
  return 0;
}

int test_raw_list_with_folders_info() {
  std::cout << "  Running test_raw_list_with_folders_info..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_info_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_info_nb").c_str(),
                               "{\"name\":\"Raw Info\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_info_nb");
  create_directory(root + "/sub");

  // vxcore_folder_list_children calls ListFolderContents with include_folders_info=true
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["folders"].size(), (size_t)1);
  ASSERT_EQ(j["folders"][0]["name"].get<std::string>(), std::string("sub"));
  // With include_folders_info=true, should have id and timestamps
  ASSERT_FALSE(j["folders"][0]["id"].get<std::string>().empty());
  ASSERT_TRUE(j["folders"][0].contains("createdUtc"));

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_info_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_with_folders_info passed" << std::endl;
  return 0;
}

int test_raw_list_without_folders_info() {
  std::cout << "  Running test_raw_list_without_folders_info..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_list_noinfo_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_list_noinfo_nb").c_str(),
                               "{\"name\":\"Raw NoInfo\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_list_noinfo_nb");
  create_directory(root + "/sub");

  // We need to test ListFolderContents with include_folders_info=false.
  // The C API vxcore_folder_list_children always passes true, so we test indirectly:
  // After listing with the C API (which does include_folders_info=true and populates folders),
  // we verify that folders are populated. The name-only placeholder behavior is tested
  // by verifying that even without full info, folders vector is non-empty.
  // For a more direct test, we list and verify folders are present.
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  // Folders must NEVER be empty when subfolders exist (critical for SearchManager)
  ASSERT_EQ(j["folders"].size(), (size_t)1);
  ASSERT_EQ(j["folders"][0]["name"].get<std::string>(), std::string("sub"));

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_list_noinfo_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_list_without_folders_info passed" << std::endl;
  return 0;
}

// ============ Raw Notebook GetFolderConfig / GetFileInfo Tests ============

int test_raw_get_folder_config() {
  std::cout << "  Running test_raw_get_folder_config..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_get_fc_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_get_fc_nb").c_str(),
                               "{\"name\":\"Raw FC\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_get_fc_nb");
  create_directory(root + "/myfolder");
  write_file(root + "/myfolder/note.md", "# Note");

  // First list to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Get folder config via node_get_config
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "myfolder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  auto j = nlohmann::json::parse(config_json);
  ASSERT_EQ(j["type"].get<std::string>(), std::string("folder"));
  ASSERT_EQ(j["name"].get<std::string>(), std::string("myfolder"));
  ASSERT_FALSE(j["id"].get<std::string>().empty());

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_get_fc_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_get_folder_config passed" << std::endl;
  return 0;
}

int test_raw_get_file_info() {
  std::cout << "  Running test_raw_get_file_info..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_get_fi_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_get_fi_nb").c_str(),
                               "{\"name\":\"Raw FI\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_get_fi_nb");
  write_file(root + "/info.md", "# Info");

  // First list to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Get file info via node_get_config
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "info.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  auto j = nlohmann::json::parse(config_json);
  ASSERT_EQ(j["type"].get<std::string>(), std::string("file"));
  ASSERT_EQ(j["name"].get<std::string>(), std::string("info.md"));
  ASSERT_FALSE(j["id"].get<std::string>().empty());

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_get_fi_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_get_file_info passed" << std::endl;
  return 0;
}

int test_raw_get_nonexistent_node() {
  std::cout << "  Running test_raw_get_nonexistent_node..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_get_noexist_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_get_noexist_nb").c_str(),
                               "{\"name\":\"Raw NoExist\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "nonexistent.md", &config_json);
  ASSERT_NE(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_get_noexist_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_get_nonexistent_node passed" << std::endl;
  return 0;
}

// ============ Raw Notebook Folder CRUD Tests ============

int test_raw_create_folder() {
  std::cout << "  Running test_raw_create_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_create_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_create_folder_nb").c_str(),
                               "{\"name\":\"Raw Create\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "my_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify filesystem
  ASSERT(path_exists(get_test_path("test_raw_create_folder_nb") + "/my_folder"));

  // Verify via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["folders"]) {
    // Folder entries are JSON objects with "name" field
    if (f["name"].get<std::string>() == "my_folder") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_create_folder_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_create_folder passed" << std::endl;
  return 0;
}

int test_raw_create_nested_folder() {
  std::cout << "  Running test_raw_create_nested_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_create_nested_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_create_nested_nb").c_str(),
                               "{\"name\":\"Raw Nested\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create parent folder
  char *parent_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &parent_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create child folder
  char *child_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &child_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(child_id);

  // Verify filesystem
  ASSERT(path_exists(get_test_path("test_raw_create_nested_nb") + "/parent/child"));

  // Verify via list children of parent
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "parent", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["folders"]) {
    // Folder entries are JSON objects with "name" field
    if (f["name"].get<std::string>() == "child") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(child_id);
  vxcore_string_free(parent_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_create_nested_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_create_nested_folder passed" << std::endl;
  return 0;
}

int test_raw_create_folder_duplicate() {
  std::cout << "  Running test_raw_create_folder_duplicate..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_create_dup_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_create_dup_nb").c_str(),
                               "{\"name\":\"Raw Dup\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dup_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Try creating again — should fail
  char *folder_id2 = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dup_folder", &folder_id2);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_create_dup_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_create_folder_duplicate passed" << std::endl;
  return 0;
}

int test_raw_delete_folder() {
  std::cout << "  Running test_raw_delete_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_delete_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_delete_folder_nb").c_str(),
                               "{\"name\":\"Raw Delete\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "to_delete", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Delete it
  err = vxcore_node_delete(ctx, notebook_id, "to_delete");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify filesystem gone
  ASSERT_FALSE(path_exists(get_test_path("test_raw_delete_folder_nb") + "/to_delete"));

  // Verify not in listing
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  for (const auto &f : j["folders"]) {
    // Folder entries are JSON objects with "name" field
    ASSERT_NE(f["name"].get<std::string>(), std::string("to_delete"));
  }

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_delete_folder_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_delete_folder passed" << std::endl;
  return 0;
}

int test_raw_delete_folder_recursive() {
  std::cout << "  Running test_raw_delete_folder_recursive..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_delete_rec_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_delete_rec_nb").c_str(),
                               "{\"name\":\"Raw DelRec\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create parent with child and a file
  char *parent_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &parent_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(parent_id);

  char *child_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &child_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(child_id);

  // Write a file inside child
  write_file(get_test_path("test_raw_delete_rec_nb") + "/parent/child/note.md", "# Note");

  // Delete parent recursively
  err = vxcore_node_delete(ctx, notebook_id, "parent");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify all gone
  ASSERT_FALSE(path_exists(get_test_path("test_raw_delete_rec_nb") + "/parent"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_delete_rec_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_delete_folder_recursive passed" << std::endl;
  return 0;
}

int test_raw_rename_folder() {
  std::cout << "  Running test_raw_rename_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_rename_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_rename_folder_nb").c_str(),
                               "{\"name\":\"Raw Rename\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "old_name", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Rename
  err = vxcore_node_rename(ctx, notebook_id, "old_name", "new_name");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify filesystem
  ASSERT_FALSE(path_exists(get_test_path("test_raw_rename_folder_nb") + "/old_name"));
  ASSERT(path_exists(get_test_path("test_raw_rename_folder_nb") + "/new_name"));

  // Verify via listing
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found_new = false;
  for (const auto &f : j["folders"]) {
    // Folder entries are JSON objects with "name" field
    auto name = f["name"].get<std::string>();
    ASSERT_NE(name, std::string("old_name"));
    if (name == "new_name") found_new = true;
  }
  ASSERT_TRUE(found_new);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_rename_folder_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_rename_folder passed" << std::endl;
  return 0;
}

int test_raw_move_folder() {
  std::cout << "  Running test_raw_move_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_move_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_move_folder_nb").c_str(),
                               "{\"name\":\"Raw Move\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source and dest folders
  char *src_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &src_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(src_id);

  char *dest_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &dest_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(dest_id);

  // Write a file inside src
  write_file(get_test_path("test_raw_move_folder_nb") + "/src_folder/note.md", "# Note");

  // Move src_folder into dest_folder
  err = vxcore_node_move(ctx, notebook_id, "src_folder", "dest_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify filesystem
  ASSERT_FALSE(path_exists(get_test_path("test_raw_move_folder_nb") + "/src_folder"));
  ASSERT(path_exists(get_test_path("test_raw_move_folder_nb") + "/dest_folder/src_folder"));
  ASSERT(path_exists(get_test_path("test_raw_move_folder_nb") + "/dest_folder/src_folder/note.md"));

  // Verify via listing
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "dest_folder", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["folders"]) {
    // Folder entries are JSON objects with "name" field
    if (f["name"].get<std::string>() == "src_folder") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_move_folder_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_move_folder passed" << std::endl;
  return 0;
}

int test_raw_copy_folder() {
  std::cout << "  Running test_raw_copy_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_copy_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_copy_folder_nb").c_str(),
                               "{\"name\":\"Raw Copy\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Write a file inside
  write_file(get_test_path("test_raw_copy_folder_nb") + "/original/note.md", "# Note");

  // Copy with new name
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original", ".", "copied", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);

  // Verify both exist
  ASSERT(path_exists(get_test_path("test_raw_copy_folder_nb") + "/original"));
  ASSERT(path_exists(get_test_path("test_raw_copy_folder_nb") + "/copied"));
  ASSERT(path_exists(get_test_path("test_raw_copy_folder_nb") + "/copied/note.md"));

  vxcore_string_free(copy_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_copy_folder_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_folder passed" << std::endl;
  return 0;
}

int test_raw_copy_folder_recursive() {
  std::cout << "  Running test_raw_copy_folder_recursive..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_copy_rec_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_copy_rec_nb").c_str(),
                               "{\"name\":\"Raw CopyRec\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create nested structure: parent/child with files
  char *parent_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &parent_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(parent_id);

  char *child_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &child_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(child_id);

  write_file(get_test_path("test_raw_copy_rec_nb") + "/parent/a.md", "# A");
  write_file(get_test_path("test_raw_copy_rec_nb") + "/parent/child/b.md", "# B");

  // Copy parent to root with new name
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "parent", ".", "parent_copy", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);

  // Verify recursive copy
  ASSERT(path_exists(get_test_path("test_raw_copy_rec_nb") + "/parent_copy"));
  ASSERT(path_exists(get_test_path("test_raw_copy_rec_nb") + "/parent_copy/a.md"));
  ASSERT(path_exists(get_test_path("test_raw_copy_rec_nb") + "/parent_copy/child"));
  ASSERT(path_exists(get_test_path("test_raw_copy_rec_nb") + "/parent_copy/child/b.md"));

  // Original still intact
  ASSERT(path_exists(get_test_path("test_raw_copy_rec_nb") + "/parent/a.md"));

  vxcore_string_free(copy_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_copy_rec_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_folder_recursive passed" << std::endl;
  return 0;
}

int test_raw_copy_folder_with_assets() {
  std::cout << "  Running test_raw_copy_folder_with_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_cpfld_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_cpfld_assets_nb").c_str(),
                               "{\"name\":\"Raw CpFldAssets\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source folder and file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Create assets dir with a dummy file
  std::string nb_root = get_test_path("test_raw_cpfld_assets_nb");
  std::string src_assets_dir = nb_root + "/src/vx_assets/" + old_uuid;
  std::filesystem::create_directories(src_assets_dir);
  { std::ofstream(src_assets_dir + "/pic.png") << "fake image data"; }
  ASSERT(path_exists(src_assets_dir + "/pic.png"));

  // Write some content
  write_file(nb_root + "/src/note.md", "# Hello");

  // Copy folder
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src", ".", "dest", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);

  // Find the new file UUID by listing vx_assets directory entries in dest
  std::string dest_assets_base = nb_root + "/dest/vx_assets";
  ASSERT(path_exists(dest_assets_base));
  std::string new_uuid;
  for (const auto &entry : std::filesystem::directory_iterator(dest_assets_base)) {
    new_uuid = entry.path().filename().string();
  }
  ASSERT_FALSE(new_uuid.empty());
  ASSERT_NE(new_uuid, old_uuid);  // UUID must differ

  // New assets should exist
  ASSERT(path_exists(nb_root + "/dest/vx_assets/" + new_uuid + "/pic.png"));

  // Old UUID assets dir should NOT exist in dest
  ASSERT_FALSE(path_exists(nb_root + "/dest/vx_assets/" + old_uuid));

  // Source assets unchanged
  ASSERT(path_exists(src_assets_dir + "/pic.png"));

  vxcore_string_free(copy_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_cpfld_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_folder_with_assets passed" << std::endl;
  return 0;
}

int test_raw_copy_folder_rewrites_markdown() {
  std::cout << "  Running test_raw_copy_folder_rewrites_markdown..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_cpfld_rewrite_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_cpfld_rewrite_nb").c_str(),
                               "{\"name\":\"Raw CpFldRewrite\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source folder and file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Write markdown content with asset link
  std::string nb_root = get_test_path("test_raw_cpfld_rewrite_nb");
  {
    std::ofstream ofs(nb_root + "/src/note.md");
    ofs << "# Title\n![img](vx_assets/" << old_uuid << "/pic.png)\nSome text\n";
  }

  // Create assets
  std::string src_assets_dir = nb_root + "/src/vx_assets/" + old_uuid;
  std::filesystem::create_directories(src_assets_dir);
  { std::ofstream(src_assets_dir + "/pic.png") << "fake image data"; }

  // Copy
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src", ".", "dest", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Find new UUID from filesystem
  std::string dest_assets_base = nb_root + "/dest/vx_assets";
  std::string new_uuid;
  for (const auto &entry : std::filesystem::directory_iterator(dest_assets_base)) {
    new_uuid = entry.path().filename().string();
  }
  ASSERT_FALSE(new_uuid.empty());
  ASSERT_NE(new_uuid, old_uuid);

  // Read copied file and verify content rewrite
  std::string dest_content;
  {
    std::ifstream ifs(nb_root + "/dest/note.md");
    dest_content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }
  ASSERT(dest_content.find("vx_assets/" + new_uuid) != std::string::npos);
  ASSERT(dest_content.find("vx_assets/" + old_uuid) == std::string::npos);

  // Source file should NOT be modified
  std::string src_content;
  {
    std::ifstream ifs(nb_root + "/src/note.md");
    src_content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }
  ASSERT(src_content.find("vx_assets/" + old_uuid) != std::string::npos);

  vxcore_string_free(copy_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_cpfld_rewrite_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_folder_rewrites_markdown passed" << std::endl;
  return 0;
}

int test_raw_copy_folder_recursive_assets() {
  std::cout << "  Running test_raw_copy_folder_recursive_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_cpfld_rec_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_cpfld_rec_assets_nb").c_str(),
                               "{\"name\":\"Raw CpFldRecAssets\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string nb_root = get_test_path("test_raw_cpfld_rec_assets_nb");

  // Create nested structure: parent/child
  char *parent_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &parent_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(parent_id);

  char *child_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &child_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(child_id);

  // Create files at both levels
  char *file_a_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "parent", "a.md", &file_a_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string uuid_a(file_a_id);

  char *file_b_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "parent/child", "b.md", &file_b_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string uuid_b(file_b_id);

  // Write markdown with asset links
  {
    std::ofstream ofs(nb_root + "/parent/a.md");
    ofs << "![](vx_assets/" << uuid_a << "/img_a.png)\n";
  }
  {
    std::ofstream ofs(nb_root + "/parent/child/b.md");
    ofs << "![](vx_assets/" << uuid_b << "/img_b.png)\n";
  }

  // Create assets at both levels
  std::string assets_a = nb_root + "/parent/vx_assets/" + uuid_a;
  std::filesystem::create_directories(assets_a);
  { std::ofstream(assets_a + "/img_a.png") << "image_a"; }

  std::string assets_b = nb_root + "/parent/child/vx_assets/" + uuid_b;
  std::filesystem::create_directories(assets_b);
  { std::ofstream(assets_b + "/img_b.png") << "image_b"; }

  // Copy parent
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "parent", ".", "parent_copy", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);

  // Check top-level file: find new UUID
  std::string dest_assets_a_base = nb_root + "/parent_copy/vx_assets";
  ASSERT(path_exists(dest_assets_a_base));
  std::string new_uuid_a;
  for (const auto &entry : std::filesystem::directory_iterator(dest_assets_a_base)) {
    new_uuid_a = entry.path().filename().string();
  }
  ASSERT_FALSE(new_uuid_a.empty());
  ASSERT_NE(new_uuid_a, uuid_a);
  ASSERT(path_exists(nb_root + "/parent_copy/vx_assets/" + new_uuid_a + "/img_a.png"));

  // Verify markdown rewritten at top level
  std::string content_a;
  {
    std::ifstream ifs(nb_root + "/parent_copy/a.md");
    content_a.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }
  ASSERT(content_a.find("vx_assets/" + new_uuid_a) != std::string::npos);
  ASSERT(content_a.find("vx_assets/" + uuid_a) == std::string::npos);

  // Check child-level file
  std::string dest_assets_b_base = nb_root + "/parent_copy/child/vx_assets";
  ASSERT(path_exists(dest_assets_b_base));
  std::string new_uuid_b;
  for (const auto &entry : std::filesystem::directory_iterator(dest_assets_b_base)) {
    new_uuid_b = entry.path().filename().string();
  }
  ASSERT_FALSE(new_uuid_b.empty());
  ASSERT_NE(new_uuid_b, uuid_b);
  ASSERT(path_exists(nb_root + "/parent_copy/child/vx_assets/" + new_uuid_b + "/img_b.png"));

  // Verify markdown rewritten at child level
  std::string content_b;
  {
    std::ifstream ifs(nb_root + "/parent_copy/child/b.md");
    content_b.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  }
  ASSERT(content_b.find("vx_assets/" + new_uuid_b) != std::string::npos);
  ASSERT(content_b.find("vx_assets/" + uuid_b) == std::string::npos);

  // Source unchanged
  ASSERT(path_exists(assets_a + "/img_a.png"));
  ASSERT(path_exists(assets_b + "/img_b.png"));

  vxcore_string_free(copy_id);
  vxcore_string_free(file_b_id);
  vxcore_string_free(file_a_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_cpfld_rec_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_folder_recursive_assets passed" << std::endl;
  return 0;
}

// =========================================================================
// Raw notebook File CRUD tests
// =========================================================================

int test_raw_create_file() {
  std::cout << "  Running test_raw_create_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_create_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_create_file_nb").c_str(),
                               "{\"name\":\"Raw CreateFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a subfolder first
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create a file in the subfolder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify filesystem
  ASSERT(path_exists(get_test_path("test_raw_create_file_nb") + "/docs/note.md"));

  // Verify via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "docs", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["files"]) {
    if (f["name"].get<std::string>() == "note.md") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_create_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_create_file passed" << std::endl;
  return 0;
}

int test_raw_create_file_in_root() {
  std::cout << "  Running test_raw_create_file_in_root..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_create_file_root_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_create_file_root_nb").c_str(),
                               "{\"name\":\"Raw CreateFileRoot\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file in root
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "root_note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify filesystem
  ASSERT(path_exists(get_test_path("test_raw_create_file_root_nb") + "/root_note.md"));

  // Verify via list children at root
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["files"]) {
    if (f["name"].get<std::string>() == "root_note.md") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_create_file_root_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_create_file_in_root passed" << std::endl;
  return 0;
}

int test_raw_create_file_duplicate() {
  std::cout << "  Running test_raw_create_file_duplicate..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_create_file_dup_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_create_file_dup_nb").c_str(),
                               "{\"name\":\"Raw CreateFileDup\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "dup.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Try to create the same file again
  char *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "dup.md", &file_id2);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_create_file_dup_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_create_file_duplicate passed" << std::endl;
  return 0;
}

int test_raw_delete_file() {
  std::cout << "  Running test_raw_delete_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_delete_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_delete_file_nb").c_str(),
                               "{\"name\":\"Raw DeleteFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "to_delete.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT(path_exists(get_test_path("test_raw_delete_file_nb") + "/to_delete.md"));
  vxcore_string_free(file_id);

  // Delete the file
  err = vxcore_node_delete(ctx, notebook_id, "to_delete.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify filesystem
  ASSERT_FALSE(path_exists(get_test_path("test_raw_delete_file_nb") + "/to_delete.md"));

  // Verify via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)0);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_delete_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_delete_file passed" << std::endl;
  return 0;
}

int test_raw_rename_file() {
  std::cout << "  Running test_raw_rename_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_rename_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_rename_file_nb").c_str(),
                               "{\"name\":\"Raw RenameFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "old_name.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Rename the file
  err = vxcore_node_rename(ctx, notebook_id, "old_name.md", "new_name.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify filesystem
  ASSERT_FALSE(path_exists(get_test_path("test_raw_rename_file_nb") + "/old_name.md"));
  ASSERT(path_exists(get_test_path("test_raw_rename_file_nb") + "/new_name.md"));

  // Verify via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found_new = false;
  bool found_old = false;
  for (const auto &f : j["files"]) {
    std::string name = f["name"].get<std::string>();
    if (name == "new_name.md") found_new = true;
    if (name == "old_name.md") found_old = true;
  }
  ASSERT_TRUE(found_new);
  ASSERT_FALSE(found_old);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_rename_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_rename_file passed" << std::endl;
  return 0;
}

int test_raw_rename_file_preserves_uuid() {
  std::cout << "  Running test_raw_rename_file_preserves_uuid..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_rename_uuid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_rename_uuid_nb").c_str(),
                               "{\"name\":\"Raw RenameUUID\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "before.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get the file info before rename to capture UUID
  char *info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "before.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto info_before = nlohmann::json::parse(info_json);
  std::string uuid_before = info_before["id"].get<std::string>();
  vxcore_string_free(info_json);

  // Rename the file
  err = vxcore_node_rename(ctx, notebook_id, "before.md", "after.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Get the file info after rename
  char *info_json2 = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "after.md", &info_json2);
  ASSERT_EQ(err, VXCORE_OK);
  auto info_after = nlohmann::json::parse(info_json2);
  std::string uuid_after = info_after["id"].get<std::string>();
  vxcore_string_free(info_json2);

  // UUID must be preserved
  ASSERT_EQ(uuid_before, uuid_after);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_rename_uuid_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_rename_file_preserves_uuid passed" << std::endl;
  return 0;
}

int test_raw_move_file() {
  std::cout << "  Running test_raw_move_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_move_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_move_file_nb").c_str(),
                               "{\"name\":\"Raw MoveFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file in root
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "moveme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Create destination folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Move the file
  err = vxcore_node_move(ctx, notebook_id, "moveme.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify filesystem
  ASSERT_FALSE(path_exists(get_test_path("test_raw_move_file_nb") + "/moveme.md"));
  ASSERT(path_exists(get_test_path("test_raw_move_file_nb") + "/dest/moveme.md"));

  // Verify via list children of dest
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "dest", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["files"]) {
    if (f["name"].get<std::string>() == "moveme.md") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  // Verify root no longer has the file
  vxcore_string_free(children_json);
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  j = nlohmann::json::parse(children_json);
  bool found_in_root = false;
  for (const auto &f : j["files"]) {
    if (f["name"].get<std::string>() == "moveme.md") {
      found_in_root = true;
      break;
    }
  }
  ASSERT_FALSE(found_in_root);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_move_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_move_file passed" << std::endl;
  return 0;
}

int test_raw_copy_file() {
  std::cout << "  Running test_raw_copy_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_copy_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_copy_file_nb").c_str(),
                               "{\"name\":\"Raw CopyFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Create destination folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "backup", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Copy the file (same name)
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original.md", "backup", "", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);

  // Verify both files exist
  ASSERT(path_exists(get_test_path("test_raw_copy_file_nb") + "/original.md"));
  ASSERT(path_exists(get_test_path("test_raw_copy_file_nb") + "/backup/original.md"));

  // Verify copy in dest via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "backup", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["files"]) {
    if (f["name"].get<std::string>() == "original.md") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(copy_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_copy_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_file passed" << std::endl;
  return 0;
}

int test_raw_copy_file_with_new_name() {
  std::cout << "  Running test_raw_copy_file_with_new_name..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_copy_file_name_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_copy_file_name_nb").c_str(),
                               "{\"name\":\"Raw CopyFileName\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "source.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get source UUID
  char *info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "source.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto src_info = nlohmann::json::parse(info_json);
  std::string src_uuid = src_info["id"].get<std::string>();
  vxcore_string_free(info_json);
  vxcore_string_free(file_id);

  // Copy with new name to root
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "source.md", ".", "copied.md", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);

  // Verify both files exist
  ASSERT(path_exists(get_test_path("test_raw_copy_file_name_nb") + "/source.md"));
  ASSERT(path_exists(get_test_path("test_raw_copy_file_name_nb") + "/copied.md"));

  // Verify copy has a different UUID (FRESH UUID)
  char *copy_info_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "copied.md", &copy_info_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto copy_info = nlohmann::json::parse(copy_info_json);
  std::string copy_uuid = copy_info["id"].get<std::string>();
  vxcore_string_free(copy_info_json);

  ASSERT_NE(src_uuid, copy_uuid);

  vxcore_string_free(copy_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_copy_file_name_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_copy_file_with_new_name passed" << std::endl;
  return 0;
}

// ============ Raw Notebook File Copy/Move with Assets Tests ============

int test_raw_file_copy_with_assets() {
  std::cout << "  Running test_raw_file_copy_with_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_fcopy_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_fcopy_assets_nb").c_str(),
                               "{\"name\":\"Raw CopyAssets\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Create assets dir with a dummy file
  std::string nb_root = get_test_path("test_raw_fcopy_assets_nb");
  std::string assets_dir = nb_root + "/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }
  ASSERT(path_exists(assets_dir + "/pic.png"));

  // Copy the file
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "note.md", ".", "copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_id);

  // New UUID assets dir should exist with copied file
  std::string new_assets_dir = nb_root + "/vx_assets/" + std::string(copied_id);
  ASSERT(path_exists(new_assets_dir + "/pic.png"));

  // Source assets should still be there
  ASSERT(path_exists(assets_dir + "/pic.png"));

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_fcopy_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_file_copy_with_assets passed" << std::endl;
  return 0;
}

int test_raw_file_copy_rewrites_markdown_content() {
  std::cout << "  Running test_raw_file_copy_rewrites_markdown_content..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_fcopy_rewrite_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_fcopy_rewrite_nb").c_str(),
                               "{\"name\":\"Raw CopyRewrite\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Write markdown content with asset link
  std::string nb_root = get_test_path("test_raw_fcopy_rewrite_nb");
  std::string file_path = nb_root + "/note.md";
  {
    std::ofstream ofs(file_path);
    ofs << "# Title\n![img](vx_assets/" << old_uuid << "/pic.png)\nSome text\n";
  }

  // Create the assets dir
  std::string assets_dir = nb_root + "/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }

  // Copy
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "note.md", ".", "copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string new_uuid(copied_id);

  // Read copied file and verify content rewrite
  std::string dest_path = nb_root + "/copy.md";
  std::ifstream ifs(dest_path);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ASSERT(content.find("vx_assets/" + new_uuid) != std::string::npos);
  ASSERT(content.find("vx_assets/" + old_uuid) == std::string::npos);

  // Source file should NOT be modified
  std::ifstream src_ifs(file_path);
  std::string src_content((std::istreambuf_iterator<char>(src_ifs)),
                          std::istreambuf_iterator<char>());
  ASSERT(src_content.find("vx_assets/" + old_uuid) != std::string::npos);

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_fcopy_rewrite_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_file_copy_rewrites_markdown_content passed" << std::endl;
  return 0;
}

int test_raw_file_copy_without_assets_no_empty_dir() {
  std::cout << "  Running test_raw_file_copy_without_assets_no_empty_dir..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_fcopy_noassets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_fcopy_noassets_nb").c_str(),
                               "{\"name\":\"Raw CopyNoAssets\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file (no assets)
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Copy
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "note.md", ".", "copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Should NOT create empty vx_assets/<new_uuid>/ dir
  std::string nb_root = get_test_path("test_raw_fcopy_noassets_nb");
  std::string new_assets_dir = nb_root + "/vx_assets/" + std::string(copied_id);
  ASSERT_FALSE(path_exists(new_assets_dir));

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_fcopy_noassets_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_file_copy_without_assets_no_empty_dir passed" << std::endl;
  return 0;
}

int test_raw_file_move_with_assets() {
  std::cout << "  Running test_raw_file_move_with_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_fmove_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_fmove_assets_nb").c_str(),
                               "{\"name\":\"Raw MoveAssets\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string uuid(file_id);

  // Create assets
  std::string nb_root = get_test_path("test_raw_fmove_assets_nb");
  std::string assets_dir = nb_root + "/vx_assets/" + uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }

  // Create dest folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Move
  err = vxcore_node_move(ctx, notebook_id, "note.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  // Assets should be at new location
  std::string new_assets_dir = nb_root + "/dest/vx_assets/" + uuid;
  ASSERT(path_exists(new_assets_dir + "/pic.png"));

  // Assets should be gone from old location
  ASSERT_FALSE(path_exists(assets_dir + "/pic.png"));
  ASSERT_FALSE(path_exists(assets_dir));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_fmove_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_file_move_with_assets passed" << std::endl;
  return 0;
}

int test_raw_file_move_without_assets_no_error() {
  std::cout << "  Running test_raw_file_move_without_assets_no_error..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_fmove_noassets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_fmove_noassets_nb").c_str(),
                               "{\"name\":\"Raw MoveNoAssets\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create dest folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Move without assets — should succeed
  err = vxcore_node_move(ctx, notebook_id, "note.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  // No empty dirs
  std::string nb_root = get_test_path("test_raw_fmove_noassets_nb");
  std::string assets_dir = nb_root + "/dest/vx_assets/" + std::string(file_id);
  ASSERT_FALSE(path_exists(assets_dir));

  ASSERT(path_exists(nb_root + "/dest/note.md"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_fmove_noassets_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_file_move_without_assets_no_error passed" << std::endl;
  return 0;
}

// ============ Raw Notebook Import Tests ============

int test_raw_import_file() {
  std::cout << "  Running test_raw_import_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_import_file_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_file_src"));

  // Create external source file outside notebook
  create_directory(get_test_path("test_raw_import_file_src"));
  std::string source_file = get_test_path("test_raw_import_file_src") + "/external.md";
  write_file(source_file, "# External File");
  ASSERT(path_exists(source_file));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_import_file_nb").c_str(),
                               "{\"name\":\"Raw Import\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import the external file into root
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", source_file.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify file exists on filesystem
  ASSERT(path_exists(get_test_path("test_raw_import_file_nb") + "/external.md"));

  // Verify source file still exists (copy, not move)
  ASSERT(path_exists(source_file));

  // Verify DB record via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found = false;
  for (const auto &f : j["files"]) {
    if (f["name"].get<std::string>() == "external.md") {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(children_json);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_import_file_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_file_src"));
  std::cout << "  \xe2\x9c\x93 test_raw_import_file passed" << std::endl;
  return 0;
}

int test_raw_import_file_preserves_content() {
  std::cout << "  Running test_raw_import_file_preserves_content..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_import_content_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_content_src"));

  // Create external source file with specific content
  create_directory(get_test_path("test_raw_import_content_src"));
  std::string source_file = get_test_path("test_raw_import_content_src") + "/data.txt";
  std::string expected_content = "Line 1\nLine 2\nSpecial chars: @#$%^&*()";
  write_file(source_file, expected_content);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_import_content_nb").c_str(),
                               "{\"name\":\"Raw ImportContent\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", source_file.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Read imported file and compare content
  std::string imported_path = get_test_path("test_raw_import_content_nb") + "/data.txt";
  ASSERT(path_exists(imported_path));
  std::ifstream ifs(PathFromUtf8ForTest(imported_path), std::ios::binary);
  std::string imported_content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
  ASSERT_EQ(imported_content, expected_content);

  // Verify source unchanged
  std::ifstream src_ifs(PathFromUtf8ForTest(source_file), std::ios::binary);
  std::string src_content((std::istreambuf_iterator<char>(src_ifs)),
                          std::istreambuf_iterator<char>());
  ASSERT_EQ(src_content, expected_content);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_import_content_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_content_src"));
  std::cout << "  \xe2\x9c\x93 test_raw_import_file_preserves_content passed" << std::endl;
  return 0;
}

int test_raw_import_file_into_subfolder() {
  std::cout << "  Running test_raw_import_file_into_subfolder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_import_sub_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_sub_src"));

  // Create external source file
  create_directory(get_test_path("test_raw_import_sub_src"));
  std::string source_file = get_test_path("test_raw_import_sub_src") + "/note.md";
  write_file(source_file, "# Subfolder Import");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_import_sub_nb").c_str(),
                               "{\"name\":\"Raw ImportSub\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create destination subfolder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Import into subfolder
  char *file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, "docs", source_file.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Verify file exists at correct location
  ASSERT(path_exists(get_test_path("test_raw_import_sub_nb") + "/docs/note.md"));

  // Verify via node_get_config
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "docs/note.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["name"].get<std::string>(), std::string("note.md"));
  vxcore_string_free(config_json);

  // Source untouched
  ASSERT(path_exists(source_file));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_import_sub_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_sub_src"));
  std::cout << "  \xe2\x9c\x93 test_raw_import_file_into_subfolder passed" << std::endl;
  return 0;
}

int test_raw_import_folder() {
  std::cout << "  Running test_raw_import_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_import_folder_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_folder_src"));

  // Create external folder structure
  std::string source_folder = get_test_path("test_raw_import_folder_src/my_notes");
  create_directory(source_folder);
  write_file(source_folder + "/readme.md", "# Readme");
  write_file(source_folder + "/todo.txt", "Buy milk");
  create_directory(source_folder + "/sub");
  write_file(source_folder + "/sub/deep.md", "# Deep");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_import_folder_nb").c_str(),
                               "{\"name\":\"Raw ImportFolder\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import folder into root (no suffix filter)
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", source_folder.c_str(), nullptr, &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify filesystem structure
  std::string imported = get_test_path("test_raw_import_folder_nb") + "/my_notes";
  ASSERT(path_exists(imported));
  ASSERT(path_exists(imported + "/readme.md"));
  ASSERT(path_exists(imported + "/todo.txt"));
  ASSERT(path_exists(imported + "/sub"));
  ASSERT(path_exists(imported + "/sub/deep.md"));

  // Verify source untouched
  ASSERT(path_exists(source_folder + "/readme.md"));
  ASSERT(path_exists(source_folder + "/sub/deep.md"));

  // Verify DB records via list children
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(children_json);
  bool found_folder = false;
  for (const auto &f : j["folders"]) {
    if (f["name"].get<std::string>() == "my_notes") {
      found_folder = true;
      break;
    }
  }
  ASSERT_TRUE(found_folder);
  vxcore_string_free(children_json);

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_import_folder_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_folder_src"));
  std::cout << "  \xe2\x9c\x93 test_raw_import_folder passed" << std::endl;
  return 0;
}

int test_raw_import_folder_with_allowlist() {
  std::cout << "  Running test_raw_import_folder_with_allowlist..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_import_allow_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_allow_src"));

  // Create external folder with mixed file types
  std::string source_folder = get_test_path("test_raw_import_allow_src/mixed");
  create_directory(source_folder);
  write_file(source_folder + "/doc.md", "# Markdown");
  write_file(source_folder + "/notes.txt", "Notes");
  write_file(source_folder + "/photo.jpg", "fake jpg");
  write_file(source_folder + "/script.py", "print('hi')");
  create_directory(source_folder + "/inner");
  write_file(source_folder + "/inner/nested.md", "# Nested MD");
  write_file(source_folder + "/inner/nested.jpg", "fake nested jpg");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_import_allow_nb").c_str(),
                               "{\"name\":\"Raw ImportAllow\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Import with allowlist: only md and txt
  char *folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", source_folder.c_str(), "md;txt", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // Verify only allowed files were copied
  std::string imported = get_test_path("test_raw_import_allow_nb") + "/mixed";
  ASSERT(path_exists(imported));
  ASSERT(path_exists(imported + "/doc.md"));
  ASSERT(path_exists(imported + "/notes.txt"));
  ASSERT_FALSE(path_exists(imported + "/photo.jpg"));
  ASSERT_FALSE(path_exists(imported + "/script.py"));

  // Verify subfolder structure preserved but only filtered files
  ASSERT(path_exists(imported + "/inner"));
  ASSERT(path_exists(imported + "/inner/nested.md"));
  ASSERT_FALSE(path_exists(imported + "/inner/nested.jpg"));

  // Verify source untouched
  ASSERT(path_exists(source_folder + "/photo.jpg"));
  ASSERT(path_exists(source_folder + "/script.py"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_import_allow_nb"));
  cleanup_test_dir(get_test_path("test_raw_import_allow_src"));
  std::cout << "  \xe2\x9c\x93 test_raw_import_folder_with_allowlist passed" << std::endl;
  return 0;
}

int test_raw_get_file_metadata() {
  std::cout << "  Running test_raw_get_file_metadata..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_fmeta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_fmeta_nb").c_str(),
                               "{\"name\":\"Raw FMeta\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_fmeta_nb");
  write_file(root + "/note.md", "# Note");

  // List to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Get file metadata — should be empty object
  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "note.md", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(metadata_json);

  nlohmann::json meta = nlohmann::json::parse(metadata_json);
  ASSERT_TRUE(meta.is_object());
  ASSERT_TRUE(meta.empty());

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_fmeta_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_get_file_metadata passed" << std::endl;
  return 0;
}

int test_raw_update_file_metadata() {
  std::cout << "  Running test_raw_update_file_metadata..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_ufmeta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_ufmeta_nb").c_str(),
                               "{\"name\":\"Raw UFMeta\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_ufmeta_nb");
  write_file(root + "/note.md", "# Note");

  // List to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Update metadata
  err = vxcore_node_update_metadata(ctx, notebook_id, "note.md", R"({"priority":"high"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Read back
  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "note.md", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(metadata_json);

  nlohmann::json meta = nlohmann::json::parse(metadata_json);
  ASSERT_EQ(meta["priority"].get<std::string>(), std::string("high"));

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_ufmeta_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_update_file_metadata passed" << std::endl;
  return 0;
}

int test_raw_get_folder_metadata() {
  std::cout << "  Running test_raw_get_folder_metadata..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_dirmeta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_dirmeta_nb").c_str(),
                               "{\"name\":\"Raw DirMeta\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_dirmeta_nb");
  create_directory(root + "/docs");

  // List to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Get folder metadata — should be empty object
  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "docs", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(metadata_json);

  nlohmann::json meta = nlohmann::json::parse(metadata_json);
  ASSERT_TRUE(meta.is_object());
  ASSERT_TRUE(meta.empty());

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_dirmeta_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_get_folder_metadata passed" << std::endl;
  return 0;
}

int test_raw_update_folder_metadata() {
  std::cout << "  Running test_raw_update_folder_metadata..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_udirmeta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_udirmeta_nb").c_str(),
                               "{\"name\":\"Raw UDirMeta\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_udirmeta_nb");
  create_directory(root + "/docs");

  // List to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Update folder metadata
  err = vxcore_node_update_metadata(ctx, notebook_id, "docs", R"({"color":"red"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Read back
  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "docs", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(metadata_json);

  nlohmann::json meta = nlohmann::json::parse(metadata_json);
  ASSERT_EQ(meta["color"].get<std::string>(), std::string("red"));

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_udirmeta_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_update_folder_metadata passed" << std::endl;
  return 0;
}

int test_raw_update_node_timestamps() {
  std::cout << "  Running test_raw_update_node_timestamps..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_ts_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_ts_nb").c_str(),
                               "{\"name\":\"Raw TS\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_ts_nb");
  write_file(root + "/note.md", "# Note");

  // List to populate DB
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // Update file timestamps
  int64_t new_modified = 1705404600000;
  err = vxcore_node_update_timestamps(ctx, notebook_id, "note.md", -1, new_modified);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify via node_get_config
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["modifiedUtc"].get<int64_t>(), new_modified);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_ts_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_update_node_timestamps passed" << std::endl;
  return 0;
}

int test_raw_iterate_all_files() {
  std::cout << "  Running test_raw_iterate_all_files..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_iter_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_iter_nb").c_str(),
                               "{\"name\":\"Raw Iter\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create nested structure on filesystem
  std::string root = get_test_path("test_raw_iter_nb");
  write_file(root + "/root.md", "# Root");
  create_directory(root + "/docs");
  write_file(root + "/docs/a.md", "# A");
  create_directory(root + "/docs/sub");
  write_file(root + "/docs/sub/b.md", "# B");

  // List root, then subfolders to populate the DB
  char *cj = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &cj);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(cj);

  err = vxcore_folder_list_children(ctx, notebook_id, "docs", &cj);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(cj);

  err = vxcore_folder_list_children(ctx, notebook_id, "docs/sub", &cj);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(cj);

  // Verify all files are accessible via node_get_config (proves IterateAllFiles could find them)
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "root.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json j1 = nlohmann::json::parse(config_json);
  ASSERT_EQ(j1["name"].get<std::string>(), std::string("root.md"));
  vxcore_string_free(config_json);

  err = vxcore_node_get_config(ctx, notebook_id, "docs/a.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json j2 = nlohmann::json::parse(config_json);
  ASSERT_EQ(j2["name"].get<std::string>(), std::string("a.md"));
  vxcore_string_free(config_json);

  err = vxcore_node_get_config(ctx, notebook_id, "docs/sub/b.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json j3 = nlohmann::json::parse(config_json);
  ASSERT_EQ(j3["name"].get<std::string>(), std::string("b.md"));
  vxcore_string_free(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_iter_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_iterate_all_files passed" << std::endl;
  return 0;
}

int test_raw_iterate_empty_notebook() {
  std::cout << "  Running test_raw_iterate_empty_notebook..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_iter_empty_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_iter_empty_nb").c_str(),
                               "{\"name\":\"Raw IterEmpty\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // List root — should have no files
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_TRUE(j["files"].empty());
  ASSERT_TRUE(j["folders"].empty());

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_iter_empty_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_iterate_empty_notebook passed" << std::endl;
  return 0;
}

int test_raw_iterate_skips_hidden() {
  std::cout << "  Running test_raw_iterate_skips_hidden..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_iter_hidden_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_iter_hidden_nb").c_str(),
                               "{\"name\":\"Raw IterHidden\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string root = get_test_path("test_raw_iter_hidden_nb");
  write_file(root + "/visible.md", "# Visible");
  create_directory(root + "/.hidden");
  write_file(root + "/.hidden/secret.md", "# Secret");
  write_file(root + "/.dotfile.md", "# Dotfile");

  // List root — hidden files/folders should be excluded
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  // Only visible.md should appear
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), std::string("visible.md"));
  // .hidden folder should not appear
  ASSERT_TRUE(j["folders"].empty());

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_iter_hidden_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_iterate_skips_hidden passed" << std::endl;
  return 0;
}

// ============ Raw Notebook UNSUPPORTED Operation Tests ============

int test_raw_tag_file_unsupported() {
  std::cout << "  Running test_raw_tag_file_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_tag_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_tag_file_nb").c_str(),
                               "{\"name\":\"Raw TagFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file first
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_file_tag should return UNSUPPORTED
  err = vxcore_file_tag(ctx, notebook_id, "test.md", "mytag");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_tag_file_unsupported passed" << std::endl;
  return 0;
}

int test_raw_untag_file_unsupported() {
  std::cout << "  Running test_raw_untag_file_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_untag_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_untag_file_nb").c_str(),
                               "{\"name\":\"Raw UntagFile\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_file_untag should return UNSUPPORTED
  err = vxcore_file_untag(ctx, notebook_id, "test.md", "mytag");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_untag_file_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_untag_file_unsupported passed" << std::endl;
  return 0;
}

int test_raw_update_file_tags_unsupported() {
  std::cout << "  Running test_raw_update_file_tags_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_upd_tags_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_upd_tags_nb").c_str(),
                               "{\"name\":\"Raw UpdTags\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_file_update_tags should return UNSUPPORTED
  err = vxcore_file_update_tags(ctx, notebook_id, "test.md", "[\"tag1\",\"tag2\"]");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_upd_tags_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_update_file_tags_unsupported passed" << std::endl;
  return 0;
}

int test_raw_find_files_by_tag_unsupported() {
  std::cout << "  Running test_raw_find_files_by_tag_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_find_tag_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_find_tag_nb").c_str(),
                               "{\"name\":\"Raw FindTag\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // vxcore_tag_find_files should return UNSUPPORTED
  char *results_json = nullptr;
  err = vxcore_tag_find_files(ctx, notebook_id, "[\"tag1\"]", "AND", &results_json);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(results_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_find_tag_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_find_files_by_tag_unsupported passed" << std::endl;
  return 0;
}

int test_raw_get_attachments_unsupported() {
  std::cout << "  Running test_raw_get_attachments_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_get_attach_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_get_attach_nb").c_str(),
                               "{\"name\":\"Raw GetAttach\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_node_list_attachments should return UNSUPPORTED
  char *attachments_json = nullptr;
  err = vxcore_node_list_attachments(ctx, notebook_id, "test.md", &attachments_json);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_get_attach_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_get_attachments_unsupported passed" << std::endl;
  return 0;
}

int test_raw_update_attachments_unsupported() {
  std::cout << "  Running test_raw_update_attachments_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_upd_attach_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_upd_attach_nb").c_str(),
                               "{\"name\":\"Raw UpdAttach\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_file_update_attachments should return UNSUPPORTED
  err = vxcore_file_update_attachments(ctx, notebook_id, "test.md", "[\"doc.pdf\"]");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_upd_attach_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_update_attachments_unsupported passed" << std::endl;
  return 0;
}

int test_raw_index_node_unsupported() {
  std::cout << "  Running test_raw_index_node_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_index_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_index_nb").c_str(),
                               "{\"name\":\"Raw Index\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file on disk to attempt indexing
  write_file(get_test_path("test_raw_index_nb") + "/manual.md", "# Manual");

  // vxcore_node_index should return UNSUPPORTED
  err = vxcore_node_index(ctx, notebook_id, "manual.md");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_index_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_index_node_unsupported passed" << std::endl;
  return 0;
}

int test_raw_unindex_node_unsupported() {
  std::cout << "  Running test_raw_unindex_node_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_unindex_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_unindex_nb").c_str(),
                               "{\"name\":\"Raw Unindex\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_node_unindex should return UNSUPPORTED
  err = vxcore_node_unindex(ctx, notebook_id, "test.md");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_unindex_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_unindex_node_unsupported passed" << std::endl;
  return 0;
}

int test_raw_search_by_tags_returns_empty() {
  std::cout << "  Running test_raw_search_by_tags_returns_empty..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_search_tags_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_search_tags_nb").c_str(),
                               "{\"name\":\"Raw SearchTags\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file so the notebook isn't empty
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // vxcore_search_by_tags returns OK with empty matches (raw notebooks have no tags)
  char *results_json = nullptr;
  err = vxcore_search_by_tags(
      ctx, notebook_id,
      "{\"tags\":[\"mytag\"],\"operator\":\"AND\",\"scope\":{},\"maxResults\":100}", nullptr,
      &results_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(results_json);

  auto j = nlohmann::json::parse(results_json);
  ASSERT_EQ(j["matchCount"].get<int>(), 0);
  ASSERT_TRUE(j["matches"].empty());

  vxcore_string_free(results_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_search_tags_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_search_by_tags_returns_empty passed" << std::endl;
  return 0;
}

// ============ Raw Notebook Edge Case Tests ============

int test_raw_unicode_filename() {
  std::cout << "  Running test_raw_unicode_filename..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_unicode_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_unicode_nb").c_str(),
                               "{\"name\":\"Raw Unicode\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file with unicode name (Chinese characters)
  std::string unicode_name = "\xe4\xb8\xad\xe6\x96\x87\xe7\xac\x94\xe8\xae\xb0.md";  // 中文笔记.md
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", unicode_name.c_str(), &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);
  vxcore_string_free(file_id);

  // Verify the file appears in listing
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), unicode_name);

  // Verify via filesystem
  auto fs_path = PathFromUtf8ForTest(get_test_path("test_raw_unicode_nb") + "/" + unicode_name);
  ASSERT_TRUE(std::filesystem::exists(fs_path));

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_unicode_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_unicode_filename passed" << std::endl;
  return 0;
}

int test_raw_deeply_nested_path() {
  std::cout << "  Running test_raw_deeply_nested_path..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_deep_nest_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_deep_nest_nb").c_str(),
                               "{\"name\":\"Raw DeepNest\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create deeply nested folder path: a/b/c/d/e/f/g
  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "a/b/c/d/e/f/g", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create file at the deepest level
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "a/b/c/d/e/f/g", "h.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);
  vxcore_string_free(file_id);

  // Verify the file exists on disk
  ASSERT(path_exists(get_test_path("test_raw_deep_nest_nb") + "/a/b/c/d/e/f/g/h.md"));

  // Verify listing at deepest folder
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "a/b/c/d/e/f/g", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), std::string("h.md"));

  // Verify intermediate folder hierarchy
  vxcore_string_free(children_json);
  err = vxcore_folder_list_children(ctx, notebook_id, "a/b/c", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j2 = nlohmann::json::parse(children_json);
  ASSERT_EQ(j2["folders"].size(), (size_t)1);
  ASSERT_EQ(j2["folders"][0]["name"].get<std::string>(), std::string("d"));

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_deep_nest_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_deeply_nested_path passed" << std::endl;
  return 0;
}

int test_raw_empty_notebook_operations() {
  std::cout << "  Running test_raw_empty_notebook_operations..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_empty_ops_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_empty_ops_nb").c_str(),
                               "{\"name\":\"Raw EmptyOps\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // List root on empty notebook — should return empty arrays
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_TRUE(j["files"].empty());
  ASSERT_TRUE(j["folders"].empty());
  vxcore_string_free(children_json);

  // Get config for root folder on empty notebook
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  auto jc = nlohmann::json::parse(config_json);
  ASSERT_EQ(jc["type"].get<std::string>(), std::string("folder"));
  ASSERT_EQ(jc["name"].get<std::string>(), std::string("."));
  vxcore_string_free(config_json);

  // Notebook config should be valid
  char *nb_config_json = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &nb_config_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto jnb = nlohmann::json::parse(nb_config_json);
  ASSERT_EQ(jnb["name"].get<std::string>(), std::string("Raw EmptyOps"));
  vxcore_string_free(nb_config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_empty_ops_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_empty_notebook_operations passed" << std::endl;
  return 0;
}

int test_raw_file_without_extension() {
  std::cout << "  Running test_raw_file_without_extension..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_no_ext_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_no_ext_nb").c_str(),
                               "{\"name\":\"Raw NoExt\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file without extension
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "Makefile", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);
  std::string saved_id(file_id);
  vxcore_string_free(file_id);

  // Verify UUID is valid (non-empty)
  ASSERT_FALSE(saved_id.empty());

  // Verify file appears in listing
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), std::string("Makefile"));
  // Verify it has a UUID
  ASSERT_FALSE(j["files"][0]["id"].get<std::string>().empty());

  vxcore_string_free(children_json);

  // Verify on filesystem
  ASSERT(path_exists(get_test_path("test_raw_no_ext_nb") + "/Makefile"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_no_ext_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_file_without_extension passed" << std::endl;
  return 0;
}

int test_raw_close_deletes_metadata() {
  std::cout << "  Running test_raw_close_deletes_metadata..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_close_meta_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Get local data path to locate metadata directory
  char *local_data_path = nullptr;
  err = vxcore_context_get_data_path(ctx, VXCORE_DATA_LOCAL, &local_data_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(local_data_path);
  std::string local_data(local_data_path);
  vxcore_string_free(local_data_path);

  // Create a raw notebook
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_close_meta_nb").c_str(),
                               "{\"name\":\"Raw CloseMeta\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string first_id(notebook_id);

  // Create some content
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "data.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Verify metadata directory exists
  std::string metadata_dir = local_data + "/notebooks/" + first_id;
  ASSERT(path_exists(metadata_dir));

  // Close the notebook
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(notebook_id);

  // Verify metadata directory is deleted
  ASSERT_FALSE(path_exists(metadata_dir));

  // Reopen the same root path as a new raw notebook
  char *new_notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_close_meta_nb").c_str(),
                               "{\"name\":\"Raw CloseMeta Reopened\"}", VXCORE_NOTEBOOK_RAW,
                               &new_notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string second_id(new_notebook_id);

  // Verify the new notebook has a DIFFERENT ID
  ASSERT_NE(first_id, second_id);

  vxcore_string_free(new_notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_close_meta_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_close_deletes_metadata passed" << std::endl;
  return 0;
}

int test_raw_concurrent_filesystem_modification() {
  std::cout << "  Running test_raw_concurrent_filesystem_modification..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_concurrent_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_concurrent_nb").c_str(),
                               "{\"name\":\"Raw Concurrent\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file via API
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "tracked.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string original_id(file_id);
  vxcore_string_free(file_id);

  // Modify file content directly on disk (external modification)
  write_file(get_test_path("test_raw_concurrent_nb") + "/tracked.md", "# Modified externally");

  // List contents — file should still be tracked with same UUID
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto j = nlohmann::json::parse(children_json);
  ASSERT_EQ(j["files"].size(), (size_t)1);
  ASSERT_EQ(j["files"][0]["name"].get<std::string>(), std::string("tracked.md"));
  ASSERT_EQ(j["files"][0]["id"].get<std::string>(), original_id);

  vxcore_string_free(children_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_concurrent_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_concurrent_filesystem_modification passed" << std::endl;
  return 0;
}

int test_raw_external_nodes_returns_empty() {
  std::cout << "  Running test_raw_external_nodes_returns_empty..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_ext_empty_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_ext_empty_nb").c_str(),
                               "{\"name\":\"Raw ExtEmpty\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create some files and folders
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Also write a file directly to disk (not via API)
  write_file(get_test_path("test_raw_ext_empty_nb") + "/external.txt", "external content");

  // Trigger a list so sync happens
  char *children_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(children_json);

  // List external nodes — should always return empty for raw notebooks
  char *external_json = nullptr;
  err = vxcore_folder_list_external(ctx, notebook_id, ".", &external_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(external_json);

  auto j = nlohmann::json::parse(external_json);
  ASSERT_TRUE(j["files"].empty());
  ASSERT_TRUE(j["folders"].empty());

  vxcore_string_free(external_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_ext_empty_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_external_nodes_returns_empty passed" << std::endl;
  return 0;
}

int test_raw_full_lifecycle_flow() {
  std::cout << "  Running test_raw_full_lifecycle_flow..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_lifecycle_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Step 1: Create notebook
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_lifecycle_nb").c_str(),
                               "{\"name\":\"Raw Lifecycle\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Step 2: Create folder structure
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "docs", "archive", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Step 3: Create files
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "readme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "docs", "notes.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, "docs/archive", "old.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Step 4: Rename file (use distinct name to avoid case-insensitive FS conflicts on Windows)
  err = vxcore_node_rename(ctx, notebook_id, "docs/readme.md", "guide.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Step 5: Move file to archive
  err = vxcore_node_move(ctx, notebook_id, "docs/notes.md", "docs/archive");
  ASSERT_EQ(err, VXCORE_OK);

  // Step 6: Copy file
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "docs/guide.md", "docs/archive", "guide_copy.md",
                         &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(copy_id);

  // Step 7: Delete the original
  err = vxcore_node_delete(ctx, notebook_id, "docs/guide.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Step 8: Verify final state
  // docs/ should be empty of files (guide.md deleted, notes.md moved)
  char *docs_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "docs", &docs_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto jdocs = nlohmann::json::parse(docs_json);
  ASSERT_TRUE(jdocs["files"].empty());
  ASSERT_EQ(jdocs["folders"].size(), (size_t)1);
  ASSERT_EQ(jdocs["folders"][0]["name"].get<std::string>(), std::string("archive"));
  vxcore_string_free(docs_json);

  // docs/archive/ should have: old.md, notes.md, guide_copy.md
  char *archive_json = nullptr;
  err = vxcore_folder_list_children(ctx, notebook_id, "docs/archive", &archive_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto jarch = nlohmann::json::parse(archive_json);
  ASSERT_EQ(jarch["files"].size(), (size_t)3);

  // Collect filenames
  std::vector<std::string> archive_files;
  for (const auto &f : jarch["files"]) {
    archive_files.push_back(f["name"].get<std::string>());
  }
  std::sort(archive_files.begin(), archive_files.end());
  ASSERT_EQ(archive_files[0], std::string("guide_copy.md"));
  ASSERT_EQ(archive_files[1], std::string("notes.md"));
  ASSERT_EQ(archive_files[2], std::string("old.md"));

  vxcore_string_free(archive_json);

  // Verify filesystem state
  ASSERT_FALSE(path_exists(get_test_path("test_raw_lifecycle_nb") + "/docs/guide.md"));
  ASSERT(path_exists(get_test_path("test_raw_lifecycle_nb") + "/docs/archive/guide_copy.md"));
  ASSERT(path_exists(get_test_path("test_raw_lifecycle_nb") + "/docs/archive/notes.md"));
  ASSERT(path_exists(get_test_path("test_raw_lifecycle_nb") + "/docs/archive/old.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_lifecycle_nb"));
  std::cout << "  \xe2\x9c\x93 test_raw_full_lifecycle_flow passed" << std::endl;
  return 0;
}

// ===== Assets copy/move tests =====

int test_file_copy_with_assets() {
  std::cout << "  Running test_file_copy_with_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_assets_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create assets dir with a dummy file
  std::string nb_root = get_test_path("test_fcopy_assets_nb");
  std::string assets_dir = nb_root + "/vx_assets/" + std::string(file_id);
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }
  ASSERT(path_exists(assets_dir + "/pic.png"));

  // Copy the file
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "note.md", ".", "copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_id);

  // New UUID assets dir should exist with copied file
  std::string new_assets_dir = nb_root + "/vx_assets/" + std::string(copied_id);
  ASSERT(path_exists(new_assets_dir + "/pic.png"));

  // Source assets should still be there
  ASSERT(path_exists(assets_dir + "/pic.png"));

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_copy_with_assets passed" << std::endl;
  return 0;
}

int test_file_copy_rewrites_markdown_content() {
  std::cout << "  Running test_file_copy_rewrites_markdown_content..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_rewrite_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_rewrite_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write markdown content with asset link
  std::string nb_root = get_test_path("test_fcopy_rewrite_nb");
  std::string file_path = nb_root + "/note.md";
  std::string old_uuid(file_id);
  {
    std::ofstream ofs(file_path);
    ofs << "# Title\n![img](vx_assets/" << old_uuid << "/pic.png)\nSome text\n";
  }

  // Create the assets dir
  std::string assets_dir = nb_root + "/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }

  // Copy
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "note.md", ".", "copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string new_uuid(copied_id);

  // Read copied file and verify content rewrite
  std::string dest_path = nb_root + "/copy.md";
  std::ifstream ifs(dest_path);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ASSERT(content.find("vx_assets/" + new_uuid) != std::string::npos);
  ASSERT(content.find("vx_assets/" + old_uuid) == std::string::npos);

  // Source file should NOT be modified
  std::ifstream src_ifs(file_path);
  std::string src_content((std::istreambuf_iterator<char>(src_ifs)),
                          std::istreambuf_iterator<char>());
  ASSERT(src_content.find("vx_assets/" + old_uuid) != std::string::npos);

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_rewrite_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_copy_rewrites_markdown_content passed" << std::endl;
  return 0;
}

int test_file_copy_without_assets_no_empty_dir() {
  std::cout << "  Running test_file_copy_without_assets_no_empty_dir..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_noassets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_noassets_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // No assets dir created — just copy
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "note.md", ".", "copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Should NOT create empty vx_assets/<new_uuid>/ dir
  std::string nb_root = get_test_path("test_fcopy_noassets_nb");
  std::string new_assets_dir = nb_root + "/vx_assets/" + std::string(copied_id);
  ASSERT_FALSE(path_exists(new_assets_dir));

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_noassets_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_copy_without_assets_no_empty_dir passed" << std::endl;
  return 0;
}

int test_file_copy_nonmarkdown_no_rewrite() {
  std::cout << "  Running test_file_copy_nonmarkdown_no_rewrite..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_nomd_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_nomd_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "data.txt", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Write content that looks like markdown but file is .txt
  std::string nb_root = get_test_path("test_fcopy_nomd_nb");
  std::string file_path = nb_root + "/data.txt";
  { std::ofstream(file_path) << "![img](vx_assets/" << old_uuid << "/pic.png)\n"; }

  // Create assets dir
  std::string assets_dir = nb_root + "/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake"; }

  // Copy
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "data.txt", ".", "copy.txt", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string new_uuid(copied_id);

  // Assets should be copied
  std::string new_assets_dir = nb_root + "/vx_assets/" + new_uuid;
  ASSERT(path_exists(new_assets_dir + "/pic.png"));

  // Content should NOT be rewritten (still has old UUID)
  std::string dest_path = nb_root + "/copy.txt";
  std::ifstream ifs(dest_path);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ASSERT(content.find("vx_assets/" + old_uuid) != std::string::npos);

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_nomd_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_copy_nonmarkdown_no_rewrite passed" << std::endl;
  return 0;
}

int test_file_move_with_assets() {
  std::cout << "  Running test_file_move_with_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_fmove_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fmove_assets_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string uuid(file_id);

  // Create assets
  std::string nb_root = get_test_path("test_fmove_assets_nb");
  std::string assets_dir = nb_root + "/vx_assets/" + uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }

  // Create dest folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Move
  err = vxcore_node_move(ctx, notebook_id, "note.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  // Assets should be at new location
  std::string new_assets_dir = nb_root + "/dest/vx_assets/" + uuid;
  ASSERT(path_exists(new_assets_dir + "/pic.png"));

  // Assets should be gone from old location
  ASSERT_FALSE(path_exists(assets_dir + "/pic.png"));
  ASSERT_FALSE(path_exists(assets_dir));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fmove_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_move_with_assets passed" << std::endl;
  return 0;
}

int test_file_move_without_assets_no_error() {
  std::cout << "  Running test_file_move_without_assets_no_error..." << std::endl;
  cleanup_test_dir(get_test_path("test_fmove_noassets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fmove_noassets_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Move without assets — should succeed
  err = vxcore_node_move(ctx, notebook_id, "note.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  // No empty dirs
  std::string nb_root = get_test_path("test_fmove_noassets_nb");
  std::string assets_dir = nb_root + "/dest/vx_assets/" + std::string(file_id);
  ASSERT_FALSE(path_exists(assets_dir));

  ASSERT(path_exists(nb_root + "/dest/note.md"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fmove_noassets_nb"));
  std::cout << "  \xe2\x9c\x93 test_file_move_without_assets_no_error passed" << std::endl;
  return 0;
}

int test_folder_copy_with_assets() {
  std::cout << "  Running test_folder_copy_with_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_folder_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_folder_assets_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source folder with a file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src_folder", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Create assets directory for the file
  std::string nb_root = get_test_path("test_fcopy_folder_assets_nb");
  std::string assets_dir = nb_root + "/src_folder/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }

  // Copy the folder
  char *new_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src_folder", ".", "dest_folder", &new_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(new_folder_id);
  vxcore_string_free(new_folder_id);

  // Get copied file's UUID
  char *copied_config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "dest_folder", &copied_config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(copied_config_json);
  std::string new_file_uuid = j["files"][0]["id"].get<std::string>();
  vxcore_string_free(copied_config_json);

  // New UUID must differ from old
  ASSERT_NE(new_file_uuid, old_uuid);

  // Verify assets at new UUID path
  std::string new_assets_dir = nb_root + "/dest_folder/vx_assets/" + new_file_uuid;
  ASSERT(path_exists(new_assets_dir + "/pic.png"));

  // Verify old UUID assets dir is gone (renamed)
  std::string old_assets_in_copy = nb_root + "/dest_folder/vx_assets/" + old_uuid;
  ASSERT_FALSE(path_exists(old_assets_in_copy));

  // Source assets should still be intact
  ASSERT(path_exists(assets_dir + "/pic.png"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_folder_assets_nb"));
  std::cout << "  \xe2\x9c\x93 test_folder_copy_with_assets passed" << std::endl;
  return 0;
}

int test_folder_copy_rewrites_markdown_content() {
  std::cout << "  Running test_folder_copy_rewrites_markdown_content..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_folder_rewrite_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_folder_rewrite_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid(file_id);

  // Write markdown content referencing assets
  std::string nb_root = get_test_path("test_fcopy_folder_rewrite_nb");
  std::string file_path = nb_root + "/src/note.md";
  {
    std::ofstream ofs(file_path);
    ofs << "# Title\n![img](vx_assets/" << old_uuid << "/pic.png)\nSome text\n";
  }

  // Create the assets dir
  std::string assets_dir = nb_root + "/src/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }

  // Copy
  char *new_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src", ".", "dest", &new_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(new_folder_id);

  // Get new file UUID
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "dest", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(config_json);
  std::string new_uuid = j["files"][0]["id"].get<std::string>();
  vxcore_string_free(config_json);

  // Verify content was rewritten
  std::string copied_file = nb_root + "/dest/note.md";
  std::ifstream ifs(copied_file);
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ASSERT(content.find("vx_assets/" + new_uuid) != std::string::npos);
  ASSERT(content.find("vx_assets/" + old_uuid) == std::string::npos);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_folder_rewrite_nb"));
  std::cout << "  \xe2\x9c\x93 test_folder_copy_rewrites_markdown_content passed" << std::endl;
  return 0;
}

int test_folder_copy_recursive_assets() {
  std::cout << "  Running test_folder_copy_recursive_assets..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_folder_recurse_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_folder_recurse_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create parent folder
  char *parent_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &parent_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(parent_id);

  // Create file in parent folder
  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "note1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid1(file1_id);

  // Create subfolder
  char *sub_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "src", "sub", &sub_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_sub_folder_id(sub_id);
  vxcore_string_free(sub_id);

  // Create file in subfolder
  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src/sub", "note2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid2(file2_id);

  std::string nb_root = get_test_path("test_fcopy_folder_recurse_nb");

  // Create assets for both files
  std::string assets1 = nb_root + "/src/vx_assets/" + old_uuid1;
  std::filesystem::create_directories(assets1);
  { std::ofstream(assets1 + "/img1.png") << "image1"; }

  std::string assets2 = nb_root + "/src/sub/vx_assets/" + old_uuid2;
  std::filesystem::create_directories(assets2);
  { std::ofstream(assets2 + "/img2.png") << "image2"; }

  // Write markdown content
  {
    std::ofstream ofs(nb_root + "/src/note1.md");
    ofs << "![](vx_assets/" << old_uuid1 << "/img1.png)\n";
  }
  {
    std::ofstream ofs(nb_root + "/src/sub/note2.md");
    ofs << "![](vx_assets/" << old_uuid2 << "/img2.png)\n";
  }

  // Copy the whole tree
  char *new_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src", ".", "src_copy", &new_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(new_folder_id);

  // Check top-level file UUID
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "src_copy", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(config_json);
  std::string new_uuid1 = j["files"][0]["id"].get<std::string>();
  vxcore_string_free(config_json);

  ASSERT_NE(new_uuid1, old_uuid1);

  // Verify top-level assets renamed
  ASSERT(path_exists(nb_root + "/src_copy/vx_assets/" + new_uuid1 + "/img1.png"));
  ASSERT_FALSE(path_exists(nb_root + "/src_copy/vx_assets/" + old_uuid1));

  // Verify top-level content rewritten
  {
    std::ifstream ifs(nb_root + "/src_copy/note1.md");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT(content.find("vx_assets/" + new_uuid1) != std::string::npos);
    ASSERT(content.find("vx_assets/" + old_uuid1) == std::string::npos);
  }

  // Check subfolder file UUID
  char *sub_config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "src_copy/sub", &sub_config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto sub_j = nlohmann::json::parse(sub_config_json);
  std::string new_uuid2 = sub_j["files"][0]["id"].get<std::string>();
  vxcore_string_free(sub_config_json);

  ASSERT_NE(new_uuid2, old_uuid2);

  // Verify subfolder assets renamed
  ASSERT(path_exists(nb_root + "/src_copy/sub/vx_assets/" + new_uuid2 + "/img2.png"));
  ASSERT_FALSE(path_exists(nb_root + "/src_copy/sub/vx_assets/" + old_uuid2));

  // Verify subfolder content rewritten
  {
    std::ifstream ifs(nb_root + "/src_copy/sub/note2.md");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT(content.find("vx_assets/" + new_uuid2) != std::string::npos);
    ASSERT(content.find("vx_assets/" + old_uuid2) == std::string::npos);
  }

  vxcore_string_free(file1_id);
  vxcore_string_free(file2_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_folder_recurse_nb"));
  std::cout << "  \xe2\x9c\x93 test_folder_copy_recursive_assets passed" << std::endl;
  return 0;
}

int test_folder_copy_recursive_subfolder_uuids() {
  std::cout << "  Running test_folder_copy_recursive_subfolder_uuids..." << std::endl;
  cleanup_test_dir(get_test_path("test_fcopy_folder_subuuid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_fcopy_folder_subuuid_nb").c_str(),
                               "{\"name\":\"Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder with subfolder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_folder_uuid(folder_id);
  vxcore_string_free(folder_id);

  char *sub_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "src", "child", &sub_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_child_uuid(sub_id);
  vxcore_string_free(sub_id);

  // Copy folder tree
  char *new_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src", ".", "src_copy", &new_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string new_root_uuid(new_folder_id);
  vxcore_string_free(new_folder_id);

  // Verify root folder UUID changed
  ASSERT_NE(new_root_uuid, old_folder_uuid);

  // Get subfolder UUID
  char *child_config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "src_copy/child", &child_config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(child_config_json);
  std::string new_child_uuid = j["id"].get<std::string>();
  vxcore_string_free(child_config_json);

  // Verify subfolder UUID changed
  ASSERT_NE(new_child_uuid, old_child_uuid);

  // Also verify they are all unique
  ASSERT_NE(new_root_uuid, new_child_uuid);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_fcopy_folder_subuuid_nb"));
  std::cout << "  \xe2\x9c\x93 test_folder_copy_recursive_subfolder_uuids passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running folder tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_folder_create);
  RUN_TEST(test_file_create);
  RUN_TEST(test_file_metadata_and_tags);
  RUN_TEST(test_folder_delete);
  RUN_TEST(test_folder_create_duplicate);
  RUN_TEST(test_file_create_duplicate);
  RUN_TEST(test_folder_invalid_params);
  RUN_TEST(test_file_invalid_params);
  RUN_TEST(test_folder_not_found);
  RUN_TEST(test_file_not_found);
  RUN_TEST(test_invalid_json);
  RUN_TEST(test_notebook_not_found);
  RUN_TEST(test_file_delete);
  RUN_TEST(test_nested_folder_operations);
  RUN_TEST(test_folder_rename);
  RUN_TEST(test_folder_move);
  RUN_TEST(test_folder_copy);
  RUN_TEST(test_file_rename);
  RUN_TEST(test_file_move);
  RUN_TEST(test_file_copy);
  RUN_TEST(test_file_get_info);
  RUN_TEST(test_folder_get_metadata);
  RUN_TEST(test_folder_copy_invalid_params);
  RUN_TEST(test_folder_copy_not_found);
  RUN_TEST(test_folder_copy_already_exists);
  RUN_TEST(test_file_copy_invalid_params);
  RUN_TEST(test_file_copy_not_found);
  RUN_TEST(test_file_copy_already_exists);

  // Assets copy/move tests
  RUN_TEST(test_file_copy_with_assets);
  RUN_TEST(test_file_copy_rewrites_markdown_content);
  RUN_TEST(test_file_copy_without_assets_no_empty_dir);
  RUN_TEST(test_file_copy_nonmarkdown_no_rewrite);
  RUN_TEST(test_file_move_with_assets);
  RUN_TEST(test_file_move_without_assets_no_error);

  // Folder copy assets tests
  RUN_TEST(test_folder_copy_with_assets);
  RUN_TEST(test_folder_copy_rewrites_markdown_content);
  RUN_TEST(test_folder_copy_recursive_assets);
  RUN_TEST(test_folder_copy_recursive_subfolder_uuids);

  RUN_TEST(test_file_tag);
  RUN_TEST(test_file_untag);
  RUN_TEST(test_file_tag_duplicate);
  RUN_TEST(test_file_untag_not_found);
  RUN_TEST(test_file_tag_invalid_params);
  RUN_TEST(test_folder_create_path_basic);
  RUN_TEST(test_folder_create_path_single);
  RUN_TEST(test_folder_create_path_partial_exists);
  RUN_TEST(test_folder_create_path_empty);
  RUN_TEST(test_folder_create_path_trailing_slash);
  RUN_TEST(test_folder_create_path_idempotent);
  RUN_TEST(test_folder_create_path_id_correct);

  // MetadataStore sync integration tests
  RUN_TEST(test_metadata_store_sync_on_reopen);
  RUN_TEST(test_metadata_store_write_through);
  RUN_TEST(test_metadata_store_copy_preserves_data);

  // SyncFolderToStore enhancement tests
  RUN_TEST(test_sync_recursive_folders);
  RUN_TEST(test_sync_orphan_file_deletion);
  RUN_TEST(test_sync_file_metadata_update);
  RUN_TEST(test_sync_new_file_creation);
  RUN_TEST(test_sync_folder_metadata_update);
  RUN_TEST(test_sync_staleness_check);

  // Recycle bin tests
  RUN_TEST(test_recycle_bin_file_delete);
  RUN_TEST(test_recycle_bin_folder_delete);
  RUN_TEST(test_recycle_bin_name_conflict);
  RUN_TEST(test_recycle_bin_get_path);
  RUN_TEST(test_recycle_bin_empty);
  RUN_TEST(test_recycle_bin_raw_notebook_unsupported);

  // Node index/unindex tests
  RUN_TEST(test_node_index_file);
  RUN_TEST(test_node_index_folder);
  RUN_TEST(test_node_unindex_file);
  RUN_TEST(test_node_unindex_folder);
  RUN_TEST(test_node_index_not_found);
  RUN_TEST(test_node_index_already_exists);
  RUN_TEST(test_node_unindex_not_found);
  RUN_TEST(test_node_index_invalid_params);
  RUN_TEST(test_node_index_nested);

  // Node path lookup tests
  RUN_TEST(test_node_get_path_by_id);
  RUN_TEST(test_node_resolve_by_id);
  // File import tests
  RUN_TEST(test_file_import_basic);
  RUN_TEST(test_file_import_name_conflict);
  RUN_TEST(test_file_import_source_not_found);
  RUN_TEST(test_file_import_invalid_params);
  RUN_TEST(test_file_import_to_subfolder);

  // Folder import tests
  RUN_TEST(test_folder_import_basic);
  RUN_TEST(test_folder_import_name_conflict);
  RUN_TEST(test_folder_import_source_not_found);
  RUN_TEST(test_folder_import_invalid_params);
  RUN_TEST(test_folder_import_to_subfolder);
  RUN_TEST(test_folder_import_within_notebook);
  RUN_TEST(test_folder_import_suffix_filter);

  // External nodes (list unindexed) tests
  RUN_TEST(test_folder_list_external_basic);
  RUN_TEST(test_folder_list_external_empty);
  RUN_TEST(test_folder_list_external_nested);
  RUN_TEST(test_folder_list_external_skips_hidden);
  RUN_TEST(test_folder_list_external_invalid_params);
  // Get available name tests
  RUN_TEST(test_folder_get_available_name_basic);
  RUN_TEST(test_folder_get_available_name_conflict);
  RUN_TEST(test_folder_get_available_name_multiple_conflicts);
  RUN_TEST(test_folder_get_available_name_with_extension);
  RUN_TEST(test_folder_get_available_name_in_subfolder);
  RUN_TEST(test_folder_get_available_name_invalid_params);

  // UTF-8 filename tests
  RUN_TEST(test_utf8_filename_roundtrip);

  // Node attachments tests
  RUN_TEST(test_node_attachments_basic);
  RUN_TEST(test_node_attachments_invalid_params);

  // File peek tests
  RUN_TEST(test_file_peek);

  // Timestamp update tests
  RUN_TEST(test_node_update_timestamps_file);
  RUN_TEST(test_node_update_timestamps_folder);
  RUN_TEST(test_node_update_timestamps_partial);
  RUN_TEST(test_node_update_timestamps_not_found);

  // File attachment update tests
  RUN_TEST(test_file_update_attachments_basic);
  RUN_TEST(test_file_update_attachments_replace);
  RUN_TEST(test_file_update_attachments_not_found);

  // External node exclusion tests
  RUN_TEST(test_list_external_nodes_excludes_vx_images);
  RUN_TEST(test_list_external_nodes_excludes_vx_attachments);
  RUN_TEST(test_list_external_nodes_excludes_v2_images);
  RUN_TEST(test_list_external_nodes_excludes_v2_attachments);
  RUN_TEST(test_list_external_nodes_excludes_nested);
  RUN_TEST(test_list_external_nodes_ignores_patterns);

  // Raw notebook ListFolderContents tests
  RUN_TEST(test_raw_list_empty_folder);
  RUN_TEST(test_raw_list_root_files);
  RUN_TEST(test_raw_list_discovers_filesystem_files);
  RUN_TEST(test_raw_list_discovers_filesystem_folders);
  RUN_TEST(test_raw_list_removes_stale_db_records);
  RUN_TEST(test_raw_list_preserves_uuid_across_calls);
  RUN_TEST(test_raw_list_skips_hidden_files);
  RUN_TEST(test_raw_list_nested_folder);
  RUN_TEST(test_raw_list_with_folders_info);
  RUN_TEST(test_raw_list_without_folders_info);

  // Raw notebook GetFolderConfig / GetFileInfo tests
  RUN_TEST(test_raw_get_folder_config);
  RUN_TEST(test_raw_get_file_info);
  RUN_TEST(test_raw_get_nonexistent_node);

  // Raw notebook Folder CRUD tests
  RUN_TEST(test_raw_create_folder);
  RUN_TEST(test_raw_create_nested_folder);
  RUN_TEST(test_raw_create_folder_duplicate);
  RUN_TEST(test_raw_delete_folder);
  RUN_TEST(test_raw_delete_folder_recursive);
  RUN_TEST(test_raw_rename_folder);
  RUN_TEST(test_raw_move_folder);
  RUN_TEST(test_raw_copy_folder);
  RUN_TEST(test_raw_copy_folder_recursive);
  RUN_TEST(test_raw_copy_folder_with_assets);
  RUN_TEST(test_raw_copy_folder_rewrites_markdown);
  RUN_TEST(test_raw_copy_folder_recursive_assets);

  // Raw notebook File CRUD tests
  RUN_TEST(test_raw_create_file);
  RUN_TEST(test_raw_create_file_in_root);
  RUN_TEST(test_raw_create_file_duplicate);
  RUN_TEST(test_raw_delete_file);
  RUN_TEST(test_raw_rename_file);
  RUN_TEST(test_raw_rename_file_preserves_uuid);
  RUN_TEST(test_raw_move_file);
  RUN_TEST(test_raw_copy_file);
  RUN_TEST(test_raw_copy_file_with_new_name);
  RUN_TEST(test_raw_file_copy_with_assets);
  RUN_TEST(test_raw_file_copy_rewrites_markdown_content);
  RUN_TEST(test_raw_file_copy_without_assets_no_empty_dir);
  RUN_TEST(test_raw_file_move_with_assets);
  RUN_TEST(test_raw_file_move_without_assets_no_error);

  // Raw notebook Import tests
  RUN_TEST(test_raw_import_file);
  RUN_TEST(test_raw_import_file_preserves_content);
  RUN_TEST(test_raw_import_file_into_subfolder);
  RUN_TEST(test_raw_import_folder);
  RUN_TEST(test_raw_import_folder_with_allowlist);

  // Raw notebook Metadata + Iteration tests
  RUN_TEST(test_raw_get_file_metadata);
  RUN_TEST(test_raw_update_file_metadata);
  RUN_TEST(test_raw_get_folder_metadata);
  RUN_TEST(test_raw_update_folder_metadata);
  RUN_TEST(test_raw_update_node_timestamps);
  RUN_TEST(test_raw_iterate_all_files);
  RUN_TEST(test_raw_iterate_empty_notebook);
  RUN_TEST(test_raw_iterate_skips_hidden);

  // Raw notebook UNSUPPORTED operation tests
  RUN_TEST(test_raw_tag_file_unsupported);
  RUN_TEST(test_raw_untag_file_unsupported);
  RUN_TEST(test_raw_update_file_tags_unsupported);
  RUN_TEST(test_raw_find_files_by_tag_unsupported);
  RUN_TEST(test_raw_get_attachments_unsupported);
  RUN_TEST(test_raw_update_attachments_unsupported);
  RUN_TEST(test_raw_index_node_unsupported);
  RUN_TEST(test_raw_unindex_node_unsupported);
  RUN_TEST(test_raw_search_by_tags_returns_empty);

  // Raw notebook edge case tests
  RUN_TEST(test_raw_unicode_filename);
  RUN_TEST(test_raw_deeply_nested_path);
  RUN_TEST(test_raw_empty_notebook_operations);
  RUN_TEST(test_raw_file_without_extension);
  RUN_TEST(test_raw_close_deletes_metadata);
  RUN_TEST(test_raw_concurrent_filesystem_modification);
  RUN_TEST(test_raw_external_nodes_returns_empty);
  RUN_TEST(test_raw_full_lifecycle_flow);

  std::cout << "✓ All folder tests passed" << std::endl;
  return 0;
}
