#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_node_get_config_file() {
  std::cout << "  Running test_node_get_config_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_get_config_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_get_config_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Test node API on file
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config.contains("type"));
  ASSERT(config["type"] == "file");
  ASSERT(config.contains("name"));
  ASSERT(config["name"] == "note.md");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_get_config_file_nb"));
  std::cout << "  ✓ test_node_get_config_file passed" << std::endl;
  return 0;
}

int test_node_get_config_folder() {
  std::cout << "  Running test_node_get_config_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_get_config_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_get_config_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Test node API on folder
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "subfolder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config.contains("type"));
  ASSERT(config["type"] == "folder");
  ASSERT(config.contains("name"));
  ASSERT(config["name"] == "subfolder");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_get_config_folder_nb"));
  std::cout << "  ✓ test_node_get_config_folder passed" << std::endl;
  return 0;
}

int test_node_get_config_not_found() {
  std::cout << "  Running test_node_get_config_not_found..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_not_found_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_not_found_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Test node API on non-existent path
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "nonexistent.md", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_not_found_nb"));
  std::cout << "  ✓ test_node_get_config_not_found passed" << std::endl;
  return 0;
}

int test_node_delete_file() {
  std::cout << "  Running test_node_delete_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_delete_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_delete_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "delete_me.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Verify file exists
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "delete_me.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Delete using node API
  err = vxcore_node_delete(ctx, notebook_id, "delete_me.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file is gone
  err = vxcore_node_get_config(ctx, notebook_id, "delete_me.md", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_delete_file_nb"));
  std::cout << "  ✓ test_node_delete_file passed" << std::endl;
  return 0;
}

int test_node_delete_folder() {
  std::cout << "  Running test_node_delete_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_delete_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_delete_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "delete_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Verify folder exists
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "delete_folder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Delete using node API
  err = vxcore_node_delete(ctx, notebook_id, "delete_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify folder is gone
  err = vxcore_node_get_config(ctx, notebook_id, "delete_folder", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_delete_folder_nb"));
  std::cout << "  ✓ test_node_delete_folder passed" << std::endl;
  return 0;
}

int test_node_rename_file() {
  std::cout << "  Running test_node_rename_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_rename_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_rename_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "old_name.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Rename using node API
  err = vxcore_node_rename(ctx, notebook_id, "old_name.md", "new_name.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify old name is gone
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "old_name.md", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Verify new name exists
  err = vxcore_node_get_config(ctx, notebook_id, "new_name.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["type"] == "file");
  ASSERT(config["name"] == "new_name.md");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_rename_file_nb"));
  std::cout << "  ✓ test_node_rename_file passed" << std::endl;
  return 0;
}

int test_node_rename_folder() {
  std::cout << "  Running test_node_rename_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_rename_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_rename_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "old_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Rename using node API
  err = vxcore_node_rename(ctx, notebook_id, "old_folder", "new_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify old name is gone
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "old_folder", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Verify new name exists
  err = vxcore_node_get_config(ctx, notebook_id, "new_folder", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["type"] == "folder");
  ASSERT(config["name"] == "new_folder");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_rename_folder_nb"));
  std::cout << "  ✓ test_node_rename_folder passed" << std::endl;
  return 0;
}

int test_node_move_file() {
  std::cout << "  Running test_node_move_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_move_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_move_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "target_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "move_me.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Move using node API
  err = vxcore_node_move(ctx, notebook_id, "move_me.md", "target_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify old location is gone
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "move_me.md", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Verify new location exists
  err = vxcore_node_get_config(ctx, notebook_id, "target_folder/move_me.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["type"] == "file");
  ASSERT(config["name"] == "move_me.md");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_move_file_nb"));
  std::cout << "  ✓ test_node_move_file passed" << std::endl;
  return 0;
}

int test_node_move_folder() {
  std::cout << "  Running test_node_move_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_move_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_move_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "parent", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "child", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Move using node API
  err = vxcore_node_move(ctx, notebook_id, "child", "parent");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify old location is gone
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "child", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Verify new location exists
  err = vxcore_node_get_config(ctx, notebook_id, "parent/child", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["type"] == "folder");
  ASSERT(config["name"] == "child");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_move_folder_nb"));
  std::cout << "  ✓ test_node_move_folder passed" << std::endl;
  return 0;
}

int test_node_copy_file() {
  std::cout << "  Running test_node_copy_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_copy_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_copy_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "target", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "original.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Copy using node API
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original.md", "target", "copy.md", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);
  vxcore_string_free(copy_id);

  // Verify original still exists
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "original.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Verify copy exists
  err = vxcore_node_get_config(ctx, notebook_id, "target/copy.md", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["type"] == "file");
  ASSERT(config["name"] == "copy.md");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_copy_file_nb"));
  std::cout << "  ✓ test_node_copy_file passed" << std::endl;
  return 0;
}

int test_node_copy_folder() {
  std::cout << "  Running test_node_copy_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_copy_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_copy_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "original", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "target", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Copy using node API
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "original", "target", "copied", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);
  vxcore_string_free(copy_id);

  // Verify original still exists
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "original", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(config_json);

  // Verify copy exists
  err = vxcore_node_get_config(ctx, notebook_id, "target/copied", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT(config["type"] == "folder");
  ASSERT(config["name"] == "copied");

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_copy_folder_nb"));
  std::cout << "  ✓ test_node_copy_folder passed" << std::endl;
  return 0;
}

int test_node_metadata_file() {
  std::cout << "  Running test_node_metadata_file..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_metadata_file_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_metadata_file_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Update metadata using node API
  err = vxcore_node_update_metadata(ctx, notebook_id, "file.md",
                                    R"({"author": "Alice", "priority": "high"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Get metadata using node API
  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "file.md", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(metadata_json);

  nlohmann::json metadata = nlohmann::json::parse(metadata_json);
  ASSERT(metadata.contains("author"));
  ASSERT(metadata["author"] == "Alice");
  ASSERT(metadata.contains("priority"));
  ASSERT(metadata["priority"] == "high");

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_metadata_file_nb"));
  std::cout << "  ✓ test_node_metadata_file passed" << std::endl;
  return 0;
}

int test_node_metadata_folder() {
  std::cout << "  Running test_node_metadata_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_metadata_folder_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_metadata_folder_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Update metadata using node API
  err = vxcore_node_update_metadata(ctx, notebook_id, "folder",
                                    R"({"category": "documentation", "status": "active"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Get metadata using node API
  char *metadata_json = nullptr;
  err = vxcore_node_get_metadata(ctx, notebook_id, "folder", &metadata_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(metadata_json);

  nlohmann::json metadata = nlohmann::json::parse(metadata_json);
  ASSERT(metadata.contains("category"));
  ASSERT(metadata["category"] == "documentation");
  ASSERT(metadata.contains("status"));
  ASSERT(metadata["status"] == "active");

  vxcore_string_free(metadata_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_metadata_folder_nb"));
  std::cout << "  ✓ test_node_metadata_folder passed" << std::endl;
  return 0;
}

int test_node_invalid_params() {
  std::cout << "  Running test_node_invalid_params..." << std::endl;
  cleanup_test_dir(get_test_path("test_node_invalid_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_node_invalid_nb").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Test NULL context
  char *config_json = nullptr;
  err = vxcore_node_get_config(nullptr, notebook_id, "path", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Test NULL notebook_id
  err = vxcore_node_get_config(ctx, nullptr, "path", &config_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Test NULL node_path
  err = vxcore_node_get_config(ctx, notebook_id, nullptr, &config_json);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Test NULL output pointer
  err = vxcore_node_get_config(ctx, notebook_id, "path", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_node_invalid_nb"));
  std::cout << "  ✓ test_node_invalid_params passed" << std::endl;
  return 0;
}

int test_node_get_file_size() {
  std::cout << "  Running test_node_get_file_size (bundled and raw)..." << std::endl;
  for (const auto type : {VXCORE_NOTEBOOK_BUNDLED, VXCORE_NOTEBOOK_RAW}) {
    const auto root = get_test_path(type == VXCORE_NOTEBOOK_BUNDLED
                                       ? "test_node_file_size_bundled"
                                       : "test_node_file_size_raw");
    cleanup_test_dir(root);
    VxCoreContextHandle ctx = nullptr;
    ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
    char *notebook_id = nullptr;
    ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Size\"}", type,
                                    &notebook_id), VXCORE_OK);
    char *folder_id = nullptr;
    ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "nested", &folder_id), VXCORE_OK);
    vxcore_string_free(folder_id);
    const std::string name = u8"\u7b14\u8bb0.md";
    const std::string relative = "nested/" + name;
    const std::string disk_path = root + "/" + relative;
    char *file_id = nullptr;
    ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "nested", name.c_str(), &file_id), VXCORE_OK);
    vxcore_string_free(file_id);

    uint64_t size = 99;
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, relative.c_str(), &size), VXCORE_OK);
    ASSERT_EQ(size, 0U);
    // Byte count, not character count; embedded NUL and CRLF must count too.
    write_file(disk_path, std::string("A\0B\r\n", 5) + u8"\u03bb");
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, relative.c_str(), &size), VXCORE_OK);
    ASSERT_EQ(size, 7U);
    // An external rewrite must be visible without refreshing notebook metadata.
    write_file(disk_path, "new");
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, relative.c_str(), &size), VXCORE_OK);
    ASSERT_EQ(size, 3U);

    ASSERT_TRUE(std::filesystem::remove(utf8_to_fs_path(disk_path)));
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, relative.c_str(), &size),
              VXCORE_ERR_NOT_FOUND);
    ASSERT_EQ(size, 0U);
    size = 99;
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, "nested", &size), VXCORE_ERR_IO);
    ASSERT_EQ(size, 0U);
    size = 99;
    ASSERT_EQ(vxcore_node_get_file_size(ctx, "unknown-notebook", relative.c_str(), &size),
              VXCORE_ERR_NOT_FOUND);
    ASSERT_EQ(size, 0U);

    for (const char *path : {"", "..", "../outside.md", "nested/../../outside.md", "/note.md"
#ifdef _WIN32
                            , "C:note.md", "C:/note.md", "\\\\server\\share\\note.md"
#endif
         }) {
      size = 99;
      ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, path, &size), VXCORE_ERR_INVALID_PARAM);
      ASSERT_EQ(size, 0U);
    }
    size = 99;
    ASSERT_EQ(vxcore_node_get_file_size(nullptr, notebook_id, "note.md", &size),
              VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(size, 0U);
    size = 99;
    ASSERT_EQ(vxcore_node_get_file_size(ctx, nullptr, "note.md", &size), VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(size, 0U);
    size = 99;
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, nullptr, &size), VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(size, 0U);
    ASSERT_EQ(vxcore_node_get_file_size(ctx, notebook_id, "note.md", nullptr),
              VXCORE_ERR_INVALID_PARAM);

    vxcore_string_free(notebook_id);
    vxcore_context_destroy(ctx);
    cleanup_test_dir(root);
  }
  return 0;
}

namespace {

std::string ReadTrashTestFile(const std::string &path) {
  std::ifstream input(utf8_to_fs_path(path), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

struct StrictTrashFixture {
  explicit StrictTrashFixture(const char *name, VxCoreNotebookType type = VXCORE_NOTEBOOK_BUNDLED)
      : root(get_test_path(name)) {
    cleanup_test_dir(root);
    error = vxcore_context_create(nullptr, &context);
    if (error == VXCORE_OK)
      error = vxcore_notebook_create(context, root.c_str(), "{\"name\":\"Strict trash\"}", type,
                                     &notebook);
  }
  ~StrictTrashFixture() {
    vxcore_string_free(notebook);
    if (context) vxcore_context_destroy(context);
    cleanup_test_dir(root);
  }
  VxCoreError Create(const char *name, const std::string &body) {
    char *id = nullptr;
    const auto result = vxcore_file_create(context, notebook, ".", name, &id);
    if (result == VXCORE_OK) {
      file_id = id;
      write_file(root + "/" + name, body);
    }
    vxcore_string_free(id);
    return result;
  }
  std::string ConfigPath() const { return root + "/vx_notebook/contents/vx.json"; }
  std::string RecyclePath() const { return root + "/vx_notebook/recycle_bin"; }
  std::string root;
  std::string file_id;
  VxCoreContextHandle context = nullptr;
  char *notebook = nullptr;
  VxCoreError error = VXCORE_OK;
};

void CountTrashEvent(const char *, const char *, void *userdata) {
  ++*static_cast<int *>(userdata);
}

int AssertTrashIdentity(StrictTrashFixture &fixture, const char *name) {
  char *json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(fixture.context, fixture.notebook, name, &json), VXCORE_OK);
  const auto record = nlohmann::json::parse(json);
  vxcore_string_free(json);
  ASSERT_EQ(record["id"], fixture.file_id);
  char *path = nullptr;
  ASSERT_EQ(
      vxcore_node_get_path_by_id(fixture.context, fixture.notebook, fixture.file_id.c_str(), &path),
      VXCORE_OK);
  const std::string actual = path;
  vxcore_string_free(path);
  ASSERT_EQ(actual, std::string(name));
  return 0;
}

}  // namespace

int test_node_trash_recycles_body_and_identity() {
  StrictTrashFixture fixture("node_strict_trash_success");
  ASSERT_EQ(fixture.error, VXCORE_OK);
  // Existing entries are never replaced, including the strict manifest's name.
  create_directory(fixture.RecyclePath());
  write_file(fixture.RecyclePath() + "/vx_trash.json", "previous recycle entry");
  const std::string body("ordinary\0body\r\n", 15);
  ASSERT_EQ(fixture.Create("vx_trash.json", body), VXCORE_OK);
  const auto before = nlohmann::json::parse(ReadTrashTestFile(fixture.ConfigPath()));
  int deleted = 0;
  int configs = 0;
  ASSERT_EQ(vxcore_on_event(fixture.context, "file.deleted", CountTrashEvent, &deleted), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(fixture.context, "folder.config_changed", CountTrashEvent, &configs),
            VXCORE_OK);
  bool trashed = false;
  bool recovery = true;
  ASSERT_EQ(
      vxcore_node_trash(fixture.context, fixture.notebook, "vx_trash.json", &trashed, &recovery),
      VXCORE_OK);
  ASSERT_TRUE(trashed);
  ASSERT_FALSE(recovery);
  ASSERT_FALSE(path_exists(fixture.root + "/vx_trash.json"));
  ASSERT_EQ(deleted, 1);
  ASSERT_EQ(configs, 1);
  ASSERT_EQ(ReadTrashTestFile(fixture.RecyclePath() + "/vx_trash.json"), "previous recycle entry");
  int entries = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(utf8_to_fs_path(fixture.RecyclePath()))) {
    if (!entry.is_directory()) continue;
    ++entries;
    const auto directory = fs_path_to_utf8(entry.path());
    ASSERT_EQ(ReadTrashTestFile(directory + "/contents/vx_trash.json"), body);
    const auto manifest = nlohmann::json::parse(ReadTrashTestFile(directory + "/vx_trash.json"));
    ASSERT_EQ(manifest["notebookId"], std::string(fixture.notebook));
    ASSERT_EQ(manifest["parentId"], before["id"]);
    ASSERT_EQ(manifest["file"], before["files"][0]);
    ASSERT_EQ(manifest["file"]["id"], fixture.file_id);
  }
  ASSERT_EQ(entries, 1);
  const auto after = nlohmann::json::parse(ReadTrashTestFile(fixture.ConfigPath()));
  ASSERT_TRUE(after["files"].empty());
  char *path = nullptr;
  ASSERT_EQ(
      vxcore_node_get_path_by_id(fixture.context, fixture.notebook, fixture.file_id.c_str(), &path),
      VXCORE_ERR_NOT_FOUND);
  vxcore_string_free(path);
  trashed = true;
  recovery = true;
  ASSERT_NE(
      vxcore_node_trash(fixture.context, fixture.notebook, "vx_trash.json", &trashed, &recovery),
      VXCORE_OK);
  ASSERT_FALSE(trashed);
  ASSERT_FALSE(recovery);
  ASSERT_EQ(deleted, 1);
  return 0;
}

int test_node_trash_rejects_unsupported_and_unsafe() {
  for (const auto type : {VXCORE_NOTEBOOK_BUNDLED, VXCORE_NOTEBOOK_RAW}) {
    StrictTrashFixture fixture(type == VXCORE_NOTEBOOK_RAW ? "node_trash_raw" : "node_trash_guards",
                               type);
    ASSERT_EQ(fixture.error, VXCORE_OK);
    ASSERT_EQ(fixture.Create("keep.md", "unchanged bytes"), VXCORE_OK);
    bool trashed = true;
    bool recovery = true;
    ASSERT_EQ(vxcore_notebook_set_read_only(fixture.context, fixture.notebook, true), VXCORE_OK);
    ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery),
              VXCORE_ERR_READ_ONLY);
    ASSERT_FALSE(trashed);
    ASSERT_FALSE(recovery);
    ASSERT_EQ(vxcore_notebook_set_read_only(fixture.context, fixture.notebook, false), VXCORE_OK);
    if (type == VXCORE_NOTEBOOK_RAW) {
      ASSERT_EQ(
          vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery),
          VXCORE_ERR_UNSUPPORTED);
    } else {
      const auto before = ReadTrashTestFile(fixture.ConfigPath());
      for (const char *path : {"", "../keep.md", "./keep.md", "/keep.md", "C:keep.md",
                               "nested/../keep.md", "keep.md.", "keep.md "}) {
        ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, path, &trashed, &recovery),
                  VXCORE_ERR_INVALID_PARAM);
        ASSERT_FALSE(trashed);
        ASSERT_FALSE(recovery);
      }
      ASSERT_EQ(ReadTrashTestFile(fixture.ConfigPath()), before);
      char *folder = nullptr;
      ASSERT_EQ(vxcore_folder_create(fixture.context, fixture.notebook, ".", "folder", &folder),
                VXCORE_OK);
      vxcore_string_free(folder);
      ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, "folder", &trashed, &recovery),
                VXCORE_ERR_UNSUPPORTED);
      ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, ".", &trashed, &recovery),
                VXCORE_ERR_UNSUPPORTED);
      // Even unmarked/misnamed ciphertext is refused, without attempting decryption.
      write_file(fixture.root + "/keep.md", std::string("VNOTEE1\0body", 12));
      ASSERT_EQ(
          vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery),
          VXCORE_ERR_UNSUPPORTED);
      write_file(fixture.root + "/keep.md", "unchanged bytes");
      const auto ordinary_config = ReadTrashTestFile(fixture.ConfigPath());
      auto protected_config = nlohmann::json::parse(ordinary_config);
      protected_config["files"][0]["metadata"]["encrypted"] = true;
      write_file(fixture.ConfigPath(), protected_config.dump());
      const auto protection_result =
          vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery);
      const auto rejected_config = ReadTrashTestFile(fixture.ConfigPath());
      write_file(fixture.ConfigPath(), ordinary_config);
      ASSERT_EQ(protection_result, VXCORE_ERR_UNSUPPORTED);
      ASSERT_EQ(rejected_config, protected_config.dump());
      ASSERT_FALSE(trashed);
      ASSERT_FALSE(recovery);
      const auto source = utf8_to_fs_path(fixture.root + "/keep.md");
      const auto permissions = std::filesystem::status(source).permissions();
      std::filesystem::permissions(source, std::filesystem::perms::owner_read);
      const auto readonly_result =
          vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery);
      std::filesystem::permissions(source, permissions);
      ASSERT_EQ(readonly_result, VXCORE_ERR_PERMISSION_DENIED);
      ASSERT_FALSE(trashed);
      ASSERT_FALSE(recovery);
      ASSERT_EQ(vxcore_node_trash(fixture.context, nullptr, "keep.md", &trashed, &recovery),
                VXCORE_ERR_INVALID_PARAM);
      ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, nullptr, &trashed, &recovery),
                VXCORE_ERR_INVALID_PARAM);
      ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, nullptr),
                VXCORE_ERR_INVALID_PARAM);
      trashed = true;
      ASSERT_EQ(vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &trashed),
                VXCORE_ERR_INVALID_PARAM);
      ASSERT_FALSE(trashed);
      ASSERT_EQ(AssertTrashIdentity(fixture, "keep.md"), 0);
    }
    ASSERT_FALSE(trashed);
    ASSERT_FALSE(recovery);
    ASSERT_EQ(ReadTrashTestFile(fixture.root + "/keep.md"), "unchanged bytes");
  }
  bool trashed = true;
  bool recovery = true;
  ASSERT_EQ(vxcore_node_trash(nullptr, "notebook", "note.md", &trashed, &recovery),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_FALSE(trashed);
  ASSERT_FALSE(recovery);
  recovery = true;
  ASSERT_EQ(vxcore_node_trash(nullptr, "notebook", "note.md", nullptr, &recovery),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_FALSE(recovery);
  return 0;
}

int test_node_trash_recycle_failure_preserves_source() {
  StrictTrashFixture fixture("node_trash_recycle_failure");
  ASSERT_EQ(fixture.error, VXCORE_OK);
  ASSERT_EQ(fixture.Create("keep.md", "never permanently deleted"), VXCORE_OK);
  const auto before = ReadTrashTestFile(fixture.ConfigPath());
  // A real file in place of the recycle directory is a deterministic IO failure.
  cleanup_test_dir(fixture.RecyclePath());
  write_file(fixture.RecyclePath(), "occupied by unrelated data");
  int deleted = 0;
  int configs = 0;
  ASSERT_EQ(vxcore_on_event(fixture.context, "file.deleted", CountTrashEvent, &deleted), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(fixture.context, "folder.config_changed", CountTrashEvent, &configs),
            VXCORE_OK);
  bool trashed = true;
  bool recovery = true;
  ASSERT_NE(vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery),
            VXCORE_OK);
  ASSERT_FALSE(trashed);
  ASSERT_FALSE(recovery);
  ASSERT_EQ(ReadTrashTestFile(fixture.root + "/keep.md"), "never permanently deleted");
  ASSERT_EQ(ReadTrashTestFile(fixture.ConfigPath()), before);
  ASSERT_EQ(ReadTrashTestFile(fixture.RecyclePath()), "occupied by unrelated data");
  ASSERT_EQ(deleted, 0);
  ASSERT_EQ(configs, 0);
  ASSERT_EQ(AssertTrashIdentity(fixture, "keep.md"), 0);
  return 0;
}

int test_node_trash_metadata_failure_preserves_identity() {
  StrictTrashFixture fixture("node_trash_metadata_failure");
  ASSERT_EQ(fixture.error, VXCORE_OK);
  ASSERT_EQ(fixture.Create("keep.md", "recoverable body"), VXCORE_OK);
  const auto before = ReadTrashTestFile(fixture.ConfigPath());
  int deleted = 0;
  int configs = 0;
  ASSERT_EQ(vxcore_on_event(fixture.context, "file.deleted", CountTrashEvent, &deleted), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(fixture.context, "folder.config_changed", CountTrashEvent, &configs),
            VXCORE_OK);
  // The cache has a valid record, but broken authoritative metadata must not be
  // silently repaired or used to remove its source. This runs even as POSIX root.
  write_file(fixture.ConfigPath(), "{broken metadata");
  bool trashed = true;
  bool recovery = true;
  const auto malformed =
      vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery);
  write_file(fixture.ConfigPath(), before);
  ASSERT_NE(malformed, VXCORE_OK);
  ASSERT_FALSE(trashed);
  ASSERT_FALSE(recovery);
  ASSERT_EQ(ReadTrashTestFile(fixture.root + "/keep.md"), "recoverable body");
  ASSERT_EQ(AssertTrashIdentity(fixture, "keep.md"), 0);

  // Prevent only config installation, after the real recycle relocation. The
  // source must be rolled back with the same UUID, not recreated or reindexed.
#ifdef _WIN32
  const auto config_path = utf8_to_fs_path(fixture.ConfigPath());
  HANDLE lock = CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(lock, INVALID_HANDLE_VALUE);
  const auto failed =
      vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery);
  CloseHandle(lock);
#else
  if (::geteuid() == 0) {
    std::cout << "  Skipping permission-based config installation failure as root" << std::endl;
    ASSERT_EQ(deleted, 0);
    ASSERT_EQ(configs, 0);
    return 0;
  }
  const auto parent = utf8_to_fs_path(fixture.ConfigPath()).parent_path();
  const auto permissions = std::filesystem::status(parent).permissions();
  std::filesystem::permissions(
      parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  const auto failed =
      vxcore_node_trash(fixture.context, fixture.notebook, "keep.md", &trashed, &recovery);
  std::filesystem::permissions(parent, permissions);
#endif
  ASSERT_NE(failed, VXCORE_OK);
  ASSERT_FALSE(trashed);
  ASSERT_FALSE(recovery);
  ASSERT_EQ(ReadTrashTestFile(fixture.root + "/keep.md"), "recoverable body");
  ASSERT_EQ(ReadTrashTestFile(fixture.ConfigPath()), before);
  ASSERT_EQ(deleted, 0);
  ASSERT_EQ(configs, 0);
  ASSERT_EQ(AssertTrashIdentity(fixture, "keep.md"), 0);
  if (path_exists(fixture.RecyclePath()))
    ASSERT_TRUE(std::filesystem::is_empty(utf8_to_fs_path(fixture.RecyclePath())));
  return 0;
}

int main() {
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  std::cout << "Running unified node API tests..." << std::endl;

  RUN_TEST(test_node_get_file_size);
  RUN_TEST(test_node_get_config_file);
  RUN_TEST(test_node_get_config_folder);
  RUN_TEST(test_node_get_config_not_found);
  RUN_TEST(test_node_delete_file);
  RUN_TEST(test_node_trash_recycles_body_and_identity);
  RUN_TEST(test_node_trash_rejects_unsupported_and_unsafe);
  RUN_TEST(test_node_trash_recycle_failure_preserves_source);
  RUN_TEST(test_node_trash_metadata_failure_preserves_identity);
  RUN_TEST(test_node_delete_folder);
  RUN_TEST(test_node_rename_file);
  RUN_TEST(test_node_rename_folder);
  RUN_TEST(test_node_move_file);
  RUN_TEST(test_node_move_folder);
  RUN_TEST(test_node_copy_file);
  RUN_TEST(test_node_copy_folder);
  RUN_TEST(test_node_metadata_file);
  RUN_TEST(test_node_metadata_folder);
  RUN_TEST(test_node_invalid_params);

  std::cout << "✓ All unified node API tests passed!" << std::endl;
  return 0;
}
