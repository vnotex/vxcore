#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

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

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running unified node API tests..." << std::endl;

  RUN_TEST(test_node_get_config_file);
  RUN_TEST(test_node_get_config_folder);
  RUN_TEST(test_node_get_config_not_found);
  RUN_TEST(test_node_delete_file);
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
