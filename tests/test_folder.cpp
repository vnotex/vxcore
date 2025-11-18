#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace fs = std::filesystem;

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

int test_folder_list() {
  std::cout << "  Running test_folder_list..." << std::endl;
  cleanup_test_dir("test_folder_list_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_folder_list_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::ofstream test_file("test_folder_list_nb/test.md");
  test_file << "# Test Note\n";
  test_file.close();

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "folder1", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *contents_json = nullptr;
  err = vxcore_folder_list(ctx, notebook_id, ".", &contents_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(contents_json);

  nlohmann::json contents = nlohmann::json::parse(contents_json);
  ASSERT(contents.contains("all_files"));
  ASSERT(contents.contains("all_folders"));
  ASSERT(contents.contains("tracked_files"));
  ASSERT(contents.contains("tracked_folders"));

  bool found_file = false;
  for (const auto &file : contents["all_files"]) {
    if (file["name"] == "test.md") {
      found_file = true;
    }
  }
  ASSERT(found_file);

  bool found_folder = false;
  for (const auto &folder : contents["all_folders"]) {
    if (folder == "folder1") {
      found_folder = true;
    }
  }
  ASSERT(found_folder);

  vxcore_string_free(contents_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_folder_list_nb");
  std::cout << "  ✓ test_folder_list passed" << std::endl;
  return 0;
}

int test_file_track() {
  std::cout << "  Running test_file_track..." << std::endl;
  cleanup_test_dir("test_file_track_nb");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_file_track_nb", "{\"name\":\"Test Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::ofstream test_file("test_file_track_nb/note.md");
  test_file << "# My Note\n";
  test_file.close();

  char *file_id = nullptr;
  err = vxcore_file_track(ctx, notebook_id, ".", "note.md", &file_id);
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
  cleanup_test_dir("test_file_track_nb");
  std::cout << "  ✓ test_file_track passed" << std::endl;
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

  std::ofstream test_file("test_file_meta_nb/note.md");
  test_file << "# My Note\n";
  test_file.close();

  char *file_id = nullptr;
  err = vxcore_file_track(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  err = vxcore_file_update_metadata(ctx, notebook_id, ".", "note.md",
                                    R"({"author": "John Doe", "priority": "high"})");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_update_tags(ctx, notebook_id, ".", "note.md", R"(["work", "urgent"])");
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

int main() {
  std::cout << "\n=== Running Folder Tests ===" << std::endl;

  RUN_TEST(test_folder_create);
  RUN_TEST(test_folder_list);
  RUN_TEST(test_file_track);
  RUN_TEST(test_file_metadata_and_tags);
  RUN_TEST(test_folder_delete);

  std::cout << "\n=== All Folder Tests Passed ===" << std::endl;
  return 0;
}
