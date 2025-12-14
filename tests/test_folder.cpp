#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_folder_create() {
  std::cout << "  Running test_folder_create..." << std::endl;
  cleanup_test_dir("test_folder_create_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_create_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "test_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists("test_folder_create_nb/test_folder"));
  ASSERT(path_exists("test_folder_create_nb/vx_notebook/contents/test_folder/vx.json"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_create_nb");
  std::cout << "  ✓ test_folder_create passed" << std::endl;
  return 0;
}

int test_file_create() {
  std::cout << "  Running test_file_create..." << std::endl;
  cleanup_test_dir("test_file_create_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_create_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  char *config_json = nullptr;
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config.contains("files"));
  ASSERT(config["files"].size() == 1);
  ASSERT(config["files"][0]["name"] == "note.md");

  vxcore_string_free(config_json);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_create_nb");
  std::cout << "  ✓ test_file_create passed" << std::endl;
  return 0;
}

int test_file_metadata_and_tags() {
  std::cout << "  Running test_file_metadata_and_tags..." << std::endl;
  cleanup_test_dir("test_file_meta_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_meta_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_update_metadata(ctx, notebook_id, "note.md",
                                    R"({"author": "John Doe", "priority": "high"})");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_update_tags(ctx, notebook_id, "note.md", R"(["work", "urgent"])");
  ASSERT_EQ(err, VXCORE_OK);

  char *config_json = nullptr;
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"][0]["metadata"]["author"] == "John Doe");
  ASSERT(config["files"][0]["tags"].size() == 2);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_meta_nb");
  std::cout << "  ✓ test_file_metadata_and_tags passed" << std::endl;
  return 0;
}

int test_folder_delete() {
  std::cout << "  Running test_folder_delete..." << std::endl;
  cleanup_test_dir("test_folder_delete_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_delete_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "to_delete", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  ASSERT(path_exists("test_folder_delete_nb/to_delete"));

  err = vxcore_folder_delete(ctx, notebook_id, "to_delete");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists("test_folder_delete_nb/to_delete"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_delete_nb");
  std::cout << "  ✓ test_folder_delete passed" << std::endl;
  return 0;
}

int test_folder_create_duplicate() {
  std::cout << "  Running test_folder_create_duplicate..." << std::endl;
  cleanup_test_dir("test_folder_dup_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_dup_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  cleanup_test_dir("test_folder_dup_nb");
  std::cout << "  ✓ test_folder_create_duplicate passed" << std::endl;
  return 0;
}

int test_file_create_duplicate() {
  std::cout << "  Running test_file_create_duplicate..." << std::endl;
  cleanup_test_dir("test_file_dup_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_dup_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  cleanup_test_dir("test_file_dup_nb");
  std::cout << "  ✓ test_file_create_duplicate passed" << std::endl;
  return 0;
}

int test_folder_invalid_params() {
  std::cout << "  Running test_folder_invalid_params..." << std::endl;
  cleanup_test_dir("test_folder_invalid_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_invalid_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  cleanup_test_dir("test_folder_invalid_nb");
  std::cout << "  ✓ test_folder_invalid_params passed" << std::endl;
  return 0;
}

int test_file_invalid_params() {
  std::cout << "  Running test_file_invalid_params..." << std::endl;
  cleanup_test_dir("test_file_invalid_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_invalid_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  cleanup_test_dir("test_file_invalid_nb");
  std::cout << "  ✓ test_file_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_not_found() {
  std::cout << "  Running test_folder_not_found..." << std::endl;
  cleanup_test_dir("test_folder_notfound_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_notfound_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_folder_delete(ctx, notebook_id, "nonexistent");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_notfound_nb");
  std::cout << "  ✓ test_folder_not_found passed" << std::endl;
  return 0;
}

int test_file_not_found() {
  std::cout << "  Running test_file_not_found..." << std::endl;
  cleanup_test_dir("test_file_notfound_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_notfound_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_delete(ctx, notebook_id, "nonexistent.md");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  err = vxcore_file_update_metadata(ctx, notebook_id, "nonexistent.md", "{}");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  err = vxcore_file_update_tags(ctx, notebook_id, "nonexistent.md", "[]");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_notfound_nb");
  std::cout << "  ✓ test_file_not_found passed" << std::endl;
  return 0;
}

int test_invalid_json() {
  std::cout << "  Running test_invalid_json..." << std::endl;
  cleanup_test_dir("test_invalid_json_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_invalid_json_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_update_metadata(ctx, notebook_id, "test.md", "invalid json");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  err = vxcore_file_update_metadata(ctx, notebook_id, "test.md", "[]");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  err = vxcore_file_update_tags(ctx, notebook_id, "test.md", "not an array");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  err = vxcore_file_update_tags(ctx, notebook_id, "test.md", "{}");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_invalid_json_nb");
  std::cout << "  ✓ test_invalid_json passed" << std::endl;
  return 0;
}

int test_notebook_not_found() {
  std::cout << "  Running test_notebook_not_found..." << std::endl;
  cleanup_test_dir("test_nb_notfound");

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
  cleanup_test_dir("test_nb_notfound");
  std::cout << "  ✓ test_notebook_not_found passed" << std::endl;
  return 0;
}

int test_file_delete() {
  std::cout << "  Running test_file_delete..." << std::endl;
  cleanup_test_dir("test_file_delete_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_delete_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "to_delete.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  ASSERT(path_exists("test_file_delete_nb/to_delete.md"));

  err = vxcore_file_delete(ctx, notebook_id, "to_delete.md");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists("test_file_delete_nb/to_delete.md"));

  char *config_json = nullptr;
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"].size() == 0);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_delete_nb");
  std::cout << "  ✓ test_file_delete passed" << std::endl;
  return 0;
}

int test_nested_folder_operations() {
  std::cout << "  Running test_nested_folder_operations..." << std::endl;
  cleanup_test_dir("test_nested_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nested_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder1_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &folder1_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder1_id);

  char *folder2_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "parent", "child", &folder2_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder2_id);

  ASSERT(path_exists("test_nested_nb/parent"));
  ASSERT(path_exists("test_nested_nb/parent/child"));

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "parent/child", "nested.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  ASSERT(path_exists("test_nested_nb/parent/child/nested.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nested_nb");
  std::cout << "  ✓ test_nested_folder_operations passed" << std::endl;
  return 0;
}

int test_folder_rename() {
  std::cout << "  Running test_folder_rename..." << std::endl;
  cleanup_test_dir("test_folder_rename_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_rename_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "old_name", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  ASSERT(path_exists("test_folder_rename_nb/old_name"));

  err = vxcore_folder_rename(ctx, notebook_id, "old_name", "new_name");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists("test_folder_rename_nb/old_name"));
  ASSERT(path_exists("test_folder_rename_nb/new_name"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_rename_nb");
  std::cout << "  ✓ test_folder_rename passed" << std::endl;
  return 0;
}

int test_folder_move() {
  std::cout << "  Running test_folder_move..." << std::endl;
  cleanup_test_dir("test_folder_move_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_move_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "source", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_move(ctx, notebook_id, "source", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists("test_folder_move_nb/source"));
  ASSERT(path_exists("test_folder_move_nb/dest/source"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_move_nb");
  std::cout << "  ✓ test_folder_move passed" << std::endl;
  return 0;
}

int test_folder_copy() {
  std::cout << "  Running test_folder_copy..." << std::endl;
  cleanup_test_dir("test_folder_copy_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_copy_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  err = vxcore_folder_copy(ctx, notebook_id, "original", ".", "copy", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT(copied_folder_id != nullptr);
  vxcore_string_free(copied_folder_id);

  ASSERT(path_exists("test_folder_copy_nb/original"));
  ASSERT(path_exists("test_folder_copy_nb/copy"));
  ASSERT(path_exists("test_folder_copy_nb/copy/test.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_copy_nb");
  std::cout << "  ✓ test_folder_copy passed" << std::endl;
  return 0;
}

int test_file_rename() {
  std::cout << "  Running test_file_rename..." << std::endl;
  cleanup_test_dir("test_file_rename_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_rename_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "old.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_rename(ctx, notebook_id, "old.md", "new.md");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists("test_file_rename_nb/old.md"));
  ASSERT(path_exists("test_file_rename_nb/new.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_rename_nb");
  std::cout << "  ✓ test_file_rename passed" << std::endl;
  return 0;
}

int test_file_move() {
  std::cout << "  Running test_file_move..." << std::endl;
  cleanup_test_dir("test_file_move_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_move_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_file_move(ctx, notebook_id, "file.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists("test_file_move_nb/file.md"));
  ASSERT(path_exists("test_file_move_nb/dest/file.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_move_nb");
  std::cout << "  ✓ test_file_move passed" << std::endl;
  return 0;
}

int test_file_copy() {
  std::cout << "  Running test_file_copy..." << std::endl;
  cleanup_test_dir("test_file_copy_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_copy_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *copied_file_id = nullptr;
  err = vxcore_file_copy(ctx, notebook_id, "original.md", ".", "copy.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT(copied_file_id != nullptr);
  vxcore_string_free(copied_file_id);

  ASSERT(path_exists("test_file_copy_nb/original.md"));
  ASSERT(path_exists("test_file_copy_nb/copy.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_copy_nb");
  std::cout << "  ✓ test_file_copy passed" << std::endl;
  return 0;
}

int test_file_get_info() {
  std::cout << "  Running test_file_get_info..." << std::endl;
  cleanup_test_dir("test_file_info_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_info_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_update_metadata(ctx, notebook_id, "test.md", R"({"key": "value"})");
  ASSERT_EQ(err, VXCORE_OK);

  char *info_json = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "test.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["name"] == "test.md");
  ASSERT(info["metadata"]["key"] == "value");

  vxcore_string_free(info_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_info_nb");
  std::cout << "  ✓ test_file_get_info passed" << std::endl;
  return 0;
}

int test_folder_get_metadata() {
  std::cout << "  Running test_folder_get_metadata..." << std::endl;
  cleanup_test_dir("test_folder_meta_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_meta_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "test", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_update_metadata(ctx, notebook_id, "test", R"({"color": "blue"})");
  ASSERT_EQ(err, VXCORE_OK);

  char *metadata_json = nullptr;
  err = vxcore_folder_get_metadata(ctx, notebook_id, "test", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json metadata = nlohmann::json::parse(metadata_json);
  ASSERT(metadata["color"] == "blue");

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_meta_nb");
  std::cout << "  ✓ test_folder_get_metadata passed" << std::endl;
  return 0;
}

int test_folder_copy_invalid_params() {
  std::cout << "  Running test_folder_copy_invalid_params..." << std::endl;
  cleanup_test_dir("test_fcopy_inv_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_fcopy_inv_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_copy(ctx, notebook_id, "original", ".", "copy", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_fcopy_inv_nb");
  std::cout << "  ✓ test_folder_copy_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_copy_not_found() {
  std::cout << "  Running test_folder_copy_not_found..." << std::endl;
  cleanup_test_dir("test_fcopy_nf_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_fcopy_nf_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *copied_folder_id = nullptr;
  err = vxcore_folder_copy(ctx, notebook_id, "nonexistent", ".", "copy", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT(copied_folder_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_fcopy_nf_nb");
  std::cout << "  ✓ test_folder_copy_not_found passed" << std::endl;
  return 0;
}

int test_folder_copy_already_exists() {
  std::cout << "  Running test_folder_copy_already_exists..." << std::endl;
  cleanup_test_dir("test_fcopy_exists_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_fcopy_exists_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "existing", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *copied_folder_id = nullptr;
  err = vxcore_folder_copy(ctx, notebook_id, "original", ".", "existing", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT(copied_folder_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_fcopy_exists_nb");
  std::cout << "  ✓ test_folder_copy_already_exists passed" << std::endl;
  return 0;
}

int test_file_copy_invalid_params() {
  std::cout << "  Running test_file_copy_invalid_params..." << std::endl;
  cleanup_test_dir("test_filecopy_inv_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_filecopy_inv_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_copy(ctx, notebook_id, "original.md", ".", "copy.md", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_filecopy_inv_nb");
  std::cout << "  ✓ test_file_copy_invalid_params passed" << std::endl;
  return 0;
}

int test_file_copy_not_found() {
  std::cout << "  Running test_file_copy_not_found..." << std::endl;
  cleanup_test_dir("test_filecopy_nf_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_filecopy_nf_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *copied_file_id = nullptr;
  err = vxcore_file_copy(ctx, notebook_id, "nonexistent.md", ".", "copy.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT(copied_file_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_filecopy_nf_nb");
  std::cout << "  ✓ test_file_copy_not_found passed" << std::endl;
  return 0;
}

int test_file_copy_already_exists() {
  std::cout << "  Running test_file_copy_already_exists..." << std::endl;
  cleanup_test_dir("test_filecopy_exists_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_filecopy_exists_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_create(ctx, notebook_id, ".", "existing.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *copied_file_id = nullptr;
  err = vxcore_file_copy(ctx, notebook_id, "original.md", ".", "existing.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT(copied_file_id == nullptr);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_filecopy_exists_nb");
  std::cout << "  ✓ test_file_copy_already_exists passed" << std::endl;
  return 0;
}

int test_file_tag() {
  std::cout << "  Running test_file_tag..." << std::endl;
  cleanup_test_dir("test_file_tag_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_tag_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"][0]["tags"].size() == 2);
  ASSERT(config["files"][0]["tags"][0] == "work");
  ASSERT(config["files"][0]["tags"][1] == "urgent");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_tag_nb");
  std::cout << "  ✓ test_file_tag passed" << std::endl;
  return 0;
}

int test_file_untag() {
  std::cout << "  Running test_file_untag..." << std::endl;
  cleanup_test_dir("test_file_untag_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_untag_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["files"][0]["tags"].size() == 2);
  ASSERT(config["files"][0]["tags"][0] == "work");
  ASSERT(config["files"][0]["tags"][1] == "personal");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_untag_nb");
  std::cout << "  ✓ test_file_untag passed" << std::endl;
  return 0;
}

int test_file_tag_duplicate() {
  std::cout << "  Running test_file_tag_duplicate..." << std::endl;
  cleanup_test_dir("test_file_tag_dup_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_tag_dup_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  cleanup_test_dir("test_file_tag_dup_nb");
  std::cout << "  ✓ test_file_tag_duplicate passed" << std::endl;
  return 0;
}

int test_file_untag_not_found() {
  std::cout << "  Running test_file_untag_not_found..." << std::endl;
  cleanup_test_dir("test_file_untag_nf_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_untag_nf_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_untag(ctx, notebook_id, "note.md", "nonexistent");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_file_untag_nf_nb");
  std::cout << "  ✓ test_file_untag_not_found passed" << std::endl;
  return 0;
}

int test_file_tag_invalid_params() {
  std::cout << "  Running test_file_tag_invalid_params..." << std::endl;
  cleanup_test_dir("test_file_tag_inv_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_tag_inv_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
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
  cleanup_test_dir("test_file_tag_inv_nb");
  std::cout << "  ✓ test_file_tag_invalid_params passed" << std::endl;
  return 0;
}

int test_folder_create_path_basic() {
  std::cout << "  Running test_folder_create_path_basic..." << std::endl;
  cleanup_test_dir("test_folder_path_basic_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_path_basic_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "level1/level2/level3", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists("test_folder_path_basic_nb/level1"));
  ASSERT(path_exists("test_folder_path_basic_nb/level1/level2"));
  ASSERT(path_exists("test_folder_path_basic_nb/level1/level2/level3"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_path_basic_nb");
  std::cout << "  ✓ test_folder_create_path_basic passed" << std::endl;
  return 0;
}

int test_folder_create_path_single() {
  std::cout << "  Running test_folder_create_path_single..." << std::endl;
  cleanup_test_dir("test_folder_path_single_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_path_single_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "single_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists("test_folder_path_single_nb/single_folder"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_path_single_nb");
  std::cout << "  ✓ test_folder_create_path_single passed" << std::endl;
  return 0;
}

int test_folder_create_path_partial_exists() {
  std::cout << "  Running test_folder_create_path_partial_exists..." << std::endl;
  cleanup_test_dir("test_folder_path_partial_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_path_partial_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "existing", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create_path(ctx, notebook_id, "existing/new1/new2", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists("test_folder_path_partial_nb/existing"));
  ASSERT(path_exists("test_folder_path_partial_nb/existing/new1"));
  ASSERT(path_exists("test_folder_path_partial_nb/existing/new1/new2"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_path_partial_nb");
  std::cout << "  ✓ test_folder_create_path_partial_exists passed" << std::endl;
  return 0;
}

int test_folder_create_path_empty() {
  std::cout << "  Running test_folder_create_path_empty..." << std::endl;
  cleanup_test_dir("test_folder_path_empty_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_path_empty_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "", &folder_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_path_empty_nb");
  std::cout << "  ✓ test_folder_create_path_empty passed" << std::endl;
  return 0;
}

int test_folder_create_path_trailing_slash() {
  std::cout << "  Running test_folder_create_path_trailing_slash..." << std::endl;
  cleanup_test_dir("test_folder_path_slash_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_path_slash_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "path1/path2/", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  ASSERT(path_exists("test_folder_path_slash_nb/path1"));
  ASSERT(path_exists("test_folder_path_slash_nb/path1/path2"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_path_slash_nb");
  std::cout << "  ✓ test_folder_create_path_trailing_slash passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running folder tests..." << std::endl;

  vxcore_set_test_mode(1);

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

  std::cout << "✓ All folder tests passed" << std::endl;
  return 0;
}
