#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

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

  err = vxcore_folder_delete(ctx, notebook_id, "to_delete");
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

  err = vxcore_folder_delete(ctx, notebook_id, "nonexistent");
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

  err = vxcore_file_delete(ctx, notebook_id, "nonexistent.md");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  err = vxcore_file_update_metadata(ctx, notebook_id, "nonexistent.md", "{}");
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

  err = vxcore_file_delete(ctx, notebook_id, "to_delete.md");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT(!path_exists(get_test_path("test_file_delete_nb") + "/to_delete.md"));

  char *config_json = nullptr;
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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

  err = vxcore_folder_rename(ctx, notebook_id, "old_name", "new_name");
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

  err = vxcore_folder_move(ctx, notebook_id, "source", "dest");
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
  err = vxcore_folder_copy(ctx, notebook_id, "original", ".", "copy", &copied_folder_id);
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

  err = vxcore_file_rename(ctx, notebook_id, "old.md", "new.md");
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

  err = vxcore_file_move(ctx, notebook_id, "file.md", "dest");
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
  err = vxcore_file_copy(ctx, notebook_id, "original.md", ".", "copy.md", &copied_file_id);
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

  err = vxcore_folder_copy(ctx, notebook_id, "original", ".", "copy", nullptr);
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
  err = vxcore_folder_copy(ctx, notebook_id, "nonexistent", ".", "copy", &copied_folder_id);
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
  err = vxcore_folder_copy(ctx, notebook_id, "original", ".", "existing", &copied_folder_id);
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

  err = vxcore_file_copy(ctx, notebook_id, "original.md", ".", "copy.md", nullptr);
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
  err = vxcore_file_copy(ctx, notebook_id, "nonexistent.md", ".", "copy.md", &copied_file_id);
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
  err = vxcore_file_copy(ctx, notebook_id, "original.md", ".", "existing.md", &copied_file_id);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json root_config = nlohmann::json::parse(config_json);
  ASSERT(root_config["folders"].size() == 1);  // docs
  ASSERT(root_config["files"].size() == 1);    // readme.md
  vxcore_string_free(config_json);

  // Check nested folder config
  err = vxcore_folder_get_config(ctx, notebook_id, "docs", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json docs_config = nlohmann::json::parse(config_json);
  ASSERT(docs_config["folders"].size() == 1);  // guides
  ASSERT(docs_config["files"].size() == 1);    // intro.md
  vxcore_string_free(config_json);

  // Check deeply nested folder config
  err = vxcore_folder_get_config(ctx, notebook_id, "docs/guides", &config_json);
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
  err = vxcore_file_update_metadata(ctx, notebook_id, "notes/test.md",
                                    R"({"author": "test", "version": 1})");
  ASSERT_EQ(err, VXCORE_OK);

  // Create and apply tag
  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "notes/test.md", "work");
  ASSERT_EQ(err, VXCORE_OK);

  // Rename file
  err = vxcore_file_rename(ctx, notebook_id, "notes/test.md", "renamed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Move file to root
  err = vxcore_file_move(ctx, notebook_id, "notes/renamed.md", ".");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file in root with all its attributes
  char *info_json = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "renamed.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["name"] == "renamed.md");
  ASSERT(info["metadata"]["author"] == "test");
  ASSERT(info["tags"].size() == 1);
  ASSERT(info["tags"][0] == "work");
  vxcore_string_free(info_json);

  // Rename folder
  err = vxcore_folder_rename(ctx, notebook_id, "notes", "documents");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify folder exists with new name
  ASSERT(!path_exists(get_test_path("test_ms_write_through_nb") + "/notes"));
  ASSERT(path_exists(get_test_path("test_ms_write_through_nb") + "/documents"));

  // Delete file
  err = vxcore_file_delete(ctx, notebook_id, "renamed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Delete folder
  err = vxcore_folder_delete(ctx, notebook_id, "documents");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify root is empty (except for vx_notebook metadata folder)
  char *config_json = nullptr;
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_file_update_metadata(ctx, notebook_id, "source/original.md",
                                    R"({"priority": "high"})");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "source/original.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  // Copy file
  char *copied_file_id = nullptr;
  err = vxcore_file_copy(ctx, notebook_id, "source/original.md", ".", "copy.md", &copied_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_file_id);
  vxcore_string_free(copied_file_id);

  // Verify copied file has same metadata and tags
  char *info_json = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "copy.md", &info_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json info = nlohmann::json::parse(info_json);
  ASSERT(info["name"] == "copy.md");
  ASSERT(info["metadata"]["priority"] == "high");
  ASSERT(info["tags"].size() == 1);
  ASSERT(info["tags"][0] == "important");
  vxcore_string_free(info_json);

  // Copy folder
  char *copied_folder_id = nullptr;
  err = vxcore_folder_copy(ctx, notebook_id, "source", ".", "source_copy", &copied_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_folder_id);
  vxcore_string_free(copied_folder_id);

  // Verify copied folder's file has metadata and tags
  err = vxcore_file_get_info(ctx, notebook_id, "source_copy/original.md", &info_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Verify level1 is accessible and has correct content
  err = vxcore_folder_get_config(ctx, notebook_id, "level1", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json level1_config = nlohmann::json::parse(config_json);
  ASSERT(level1_config["folders"].size() == 1);  // level2
  ASSERT(level1_config["files"].size() == 1);    // file1.md
  vxcore_string_free(config_json);

  // Verify level2 is accessible
  err = vxcore_folder_get_config(ctx, notebook_id, "level1/level2", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json level2_config = nlohmann::json::parse(config_json);
  ASSERT(level2_config["folders"].size() == 1);  // level3
  ASSERT(level2_config["files"].size() == 1);    // file2.md
  vxcore_string_free(config_json);

  // Verify level3 is accessible
  err = vxcore_folder_get_config(ctx, notebook_id, "level1/level2/level3", &config_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_file_update_metadata(ctx, notebook_id, "note.md", "{\"priority\":\"low\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify initial metadata
  char *info_json = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "note.md", &info_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Verify metadata was updated
  err = vxcore_file_get_info(ctx, notebook_id, "note.md", &info_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, ".", &config_json);
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
  err = vxcore_folder_update_metadata(ctx, notebook_id, "docs",
                                      "{\"description\":\"Documentation\"}");
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
  err = vxcore_folder_get_config(ctx, notebook_id, "docs", &config_json);
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
    err = vxcore_folder_get_config(ctx, notebook_id, "docs", &config_json);
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
  err = vxcore_folder_get_config(ctx, notebook_id, "docs", &config_json);
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

  std::cout << "✓ All folder tests passed" << std::endl;
  return 0;
}
