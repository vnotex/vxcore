// test_missing_nodes: T4 of the missing-files-on-disk plan.
//
// Verifies the reactive missing-content gate on the bundled-notebook
// read/access C-APIs. When an indexed node's CONTENT disappears from disk,
// vxcore_node_get_config / vxcore_node_get_metadata / vxcore_buffer_open must
// return the new VXCORE_ERR_NODE_NOT_EXISTS code, while the removal path
// (vxcore_node_unindex) keeps working on the phantom and raw notebooks are
// unaffected.
//
// All assertions go through the PUBLIC C-API. The bundled storage layout used
// by the on-disk deletions:
//   - content:  <notebook_root>/<relative_path>
//   - metadata: <notebook_root>/vx_notebook/contents/<relative_path>/vx.json
// Deleting ONLY the content (not the metadata) leaves the node indexed, which
// is exactly the "indexed but missing on disk" condition under test.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

// Delete a single on-disk content node (file or directory) by UTF-8 path.
void delete_disk_path(const std::string &utf8_path) {
  std::error_code ec;
  std::filesystem::remove_all(utf8_to_fs_path(utf8_path), ec);
}

std::string content_path(const std::string &notebook_root, const std::string &relative) {
  return notebook_root + "/" + relative;
}

std::string root_config_path(const std::string &notebook_root) {
  return notebook_root + "/vx_notebook/contents/vx.json";
}

// On-disk vx.json for a non-root folder (bundled metadata layout).
std::string folder_config_path(const std::string &notebook_root, const std::string &relative) {
  return notebook_root + "/vx_notebook/contents/" + relative + "/vx.json";
}

}  // namespace

// Scenario 1: bundled indexed FILE deleted on disk.
int test_bundled_file_missing() {
  std::cout << "  Running test_bundled_file_missing..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_file_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Before deletion: all three read/access APIs succeed.
  char *config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json), VXCORE_OK);
  vxcore_string_free(config_json);
  config_json = nullptr;

  char *metadata_json = nullptr;
  ASSERT_EQ(vxcore_node_get_metadata(ctx, notebook_id, "note.md", &metadata_json), VXCORE_OK);
  vxcore_string_free(metadata_json);
  metadata_json = nullptr;

  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id), VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);
  ASSERT_EQ(vxcore_buffer_close(ctx, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  buffer_id = nullptr;

  // Delete the on-disk content (keep the metadata -> node stays indexed).
  delete_disk_path(content_path(nb_path, "note.md"));

  // After deletion: all three return the new code.
  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(config_json);

  ASSERT_EQ(vxcore_node_get_metadata(ctx, notebook_id, "note.md", &metadata_json),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(metadata_json);

  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id),
            VXCORE_ERR_NODE_NOT_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_bundled_file_missing passed" << std::endl;
  return 0;
}

// Scenario 2: bundled indexed FOLDER deleted on disk.
int test_bundled_folder_missing() {
  std::cout << "  Running test_bundled_folder_missing..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_folder_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  // Before deletion: get_config succeeds.
  char *config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "subfolder", &config_json), VXCORE_OK);
  vxcore_string_free(config_json);
  config_json = nullptr;

  // Delete only the on-disk content dir; folder metadata (vx.json) survives.
  delete_disk_path(content_path(nb_path, "subfolder"));

  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "subfolder", &config_json),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_bundled_folder_missing passed" << std::endl;
  return 0;
}

// Scenario 3: unindex still works on a phantom (gate did NOT leak into removal).
int test_unindex_phantom() {
  std::cout << "  Running test_unindex_phantom..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_unindex_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "phantom.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Make it a phantom: delete the on-disk content, keep the index entry.
  delete_disk_path(content_path(nb_path, "phantom.md"));

  // Sanity: the gate fires on read while the node is still indexed.
  char *config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "phantom.md", &config_json),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(config_json);

  // Unindex must STILL succeed on the phantom.
  ASSERT_EQ(vxcore_node_unindex(ctx, notebook_id, "phantom.md"), VXCORE_OK);

  // vx.json no longer lists the phantom.
  {
    std::ifstream f(utf8_to_fs_path(root_config_path(nb_path)));
    ASSERT_TRUE(f.is_open());
    nlohmann::json j;
    f >> j;
    bool listed = false;
    if (j.contains("files") && j["files"].is_array()) {
      for (const auto &entry : j["files"]) {
        if (entry.contains("name") && entry["name"] == "phantom.md") {
          listed = true;
          break;
        }
      }
    }
    ASSERT_FALSE(listed);
  }

  // And it is now genuinely absent from the index (NOT the missing-content code).
  config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "phantom.md", &config_json),
            VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_unindex_phantom passed" << std::endl;
  return 0;
}

// Scenario 4: raw notebook unchanged (base default true -> no gating).
int test_raw_unchanged() {
  std::cout << "  Running test_raw_unchanged..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_raw_nb");
  cleanup_test_dir(nb_path);
  create_directory(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Raw Notebook\"}",
                                   VXCORE_NOTEBOOK_RAW, &notebook_id),
            VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Delete the on-disk content for the raw notebook's file.
  delete_disk_path(content_path(nb_path, "note.md"));

  // Raw notebooks must NOT return the new code (they self-heal on read; the
  // base FolderManager default of "exists = true" leaves them ungated).
  char *config_json = nullptr;
  VxCoreError err = vxcore_node_get_config(ctx, notebook_id, "note.md", &config_json);
  ASSERT_NE(err, VXCORE_ERR_NODE_NOT_EXISTS);
  if (config_json) {
    vxcore_string_free(config_json);
  }

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_raw_unchanged passed" << std::endl;
  return 0;
}

// Scenario 5: type mismatch (file replaced by a directory on disk).
int test_type_mismatch() {
  std::cout << "  Running test_type_mismatch..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_type_mismatch_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "a.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Replace the file content with a directory of the same name.
  delete_disk_path(content_path(nb_path, "a.md"));
  create_directory(content_path(nb_path, "a.md"));

  // The node is still indexed as a FILE, but its content is a directory ->
  // IsRegularFile is false -> the gate fires.
  char *config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, notebook_id, "a.md", &config_json),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(config_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_type_mismatch passed" << std::endl;
  return 0;
}

// T5: per-child `exists` flag on vxcore_folder_list_children.
//
// The C-API marks (does NOT filter) bundled children whose on-disk content is
// gone, emitting a transient "exists" bool per child; it also returns
// VXCORE_ERR_NODE_NOT_EXISTS when the listed folder's own content dir is gone.
// The flag is OUTPUT-ONLY and must never reach vx.json.

// Scenario 6: a missing child is marked exists==false but STILL listed; a
// healthy sibling is marked exists==true. (bundled)
int test_list_children_marks_missing() {
  std::cout << "  Running test_list_children_marks_missing..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_list_marks_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "f", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "f", "a.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);
  file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "f", "b.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Delete only "f/a.md" content; keep its index entry and keep "f/b.md".
  delete_disk_path(content_path(nb_path, "f/a.md"));

  char *children_json = nullptr;
  ASSERT_EQ(vxcore_folder_list_children(ctx, notebook_id, "f", &children_json), VXCORE_OK);
  ASSERT_NOT_NULL(children_json);

  nlohmann::json j = nlohmann::json::parse(children_json);
  vxcore_string_free(children_json);

  ASSERT_TRUE(j.contains("files") && j["files"].is_array());
  bool saw_a = false, saw_b = false;
  for (const auto &entry : j["files"]) {
    ASSERT_TRUE(entry.contains("name"));
    // Every bundled child carries the transient "exists" flag.
    ASSERT_TRUE(entry.contains("exists"));
    if (entry["name"] == "a.md") {
      saw_a = true;
      ASSERT_FALSE(entry["exists"].get<bool>());  // missing content -> false
    } else if (entry["name"] == "b.md") {
      saw_b = true;
      ASSERT_TRUE(entry["exists"].get<bool>());  // healthy -> true
    }
  }
  // Phantom child is MARKED, not filtered: both still appear.
  ASSERT_TRUE(saw_a);
  ASSERT_TRUE(saw_b);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_list_children_marks_missing passed" << std::endl;
  return 0;
}

// Scenario 7: listing a folder whose own content dir is gone returns
// VXCORE_ERR_NODE_NOT_EXISTS (bundled). Metadata survives, so the failure is
// driven purely by the on-disk content check.
int test_list_children_missing_folder() {
  std::cout << "  Running test_list_children_missing_folder..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_list_folder_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "g", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  // Give "g" an indexed descendant so its metadata is non-trivial.
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "g", "c.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Before deletion: listing succeeds.
  char *children_json = nullptr;
  ASSERT_EQ(vxcore_folder_list_children(ctx, notebook_id, "g", &children_json), VXCORE_OK);
  vxcore_string_free(children_json);
  children_json = nullptr;

  // Delete "g" content dir (and its descendants' content); metadata survives.
  delete_disk_path(content_path(nb_path, "g"));

  ASSERT_EQ(vxcore_folder_list_children(ctx, notebook_id, "g", &children_json),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(children_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_list_children_missing_folder passed" << std::endl;
  return 0;
}

// Scenario 8: the transient "exists" flag is NEVER persisted to vx.json.
int test_list_children_no_persist() {
  std::cout << "  Running test_list_children_no_persist..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_list_nopersist_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "f", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "f", "a.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Create a phantom child, then list (which computes "exists" on the fly).
  delete_disk_path(content_path(nb_path, "f/a.md"));

  char *children_json = nullptr;
  ASSERT_EQ(vxcore_folder_list_children(ctx, notebook_id, "f", &children_json), VXCORE_OK);
  ASSERT_NOT_NULL(children_json);
  // Sanity: the listing JSON itself does carry the flag.
  {
    nlohmann::json listed = nlohmann::json::parse(children_json);
    bool any_exists_key = false;
    for (const auto &entry : listed["files"]) {
      if (entry.contains("exists")) {
        any_exists_key = true;
        break;
      }
    }
    ASSERT_TRUE(any_exists_key);
  }
  vxcore_string_free(children_json);

  // The on-disk folder vx.json must NOT contain the transient "exists" key.
  {
    std::ifstream f(utf8_to_fs_path(folder_config_path(nb_path, "f")));
    ASSERT_TRUE(f.is_open());
    std::string disk_contents((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    ASSERT_TRUE(disk_contents.find("\"exists\"") == std::string::npos);
  }

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_list_children_no_persist passed" << std::endl;
  return 0;
}

int test_rename_phantom_file_returns_node_not_exists() {
  std::cout << "  Running test_rename_phantom_file_returns_node_not_exists..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_rename_phantom_file_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "a.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  delete_disk_path(content_path(nb_path, "a.md"));

  ASSERT_EQ(vxcore_node_rename(ctx, notebook_id, "a.md", "b.md"),
            VXCORE_ERR_NODE_NOT_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_rename_phantom_file_returns_node_not_exists passed" << std::endl;
  return 0;
}

int test_move_phantom_file_returns_node_not_exists() {
  std::cout << "  Running test_move_phantom_file_returns_node_not_exists..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_move_phantom_file_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "dst", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "a.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  delete_disk_path(content_path(nb_path, "a.md"));

  ASSERT_EQ(vxcore_node_move(ctx, notebook_id, "a.md", "dst"),
            VXCORE_ERR_NODE_NOT_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_move_phantom_file_returns_node_not_exists passed" << std::endl;
  return 0;
}

int test_copy_phantom_file_returns_node_not_exists() {
  std::cout << "  Running test_copy_phantom_file_returns_node_not_exists..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_copy_phantom_file_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "dst", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "a.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  delete_disk_path(content_path(nb_path, "a.md"));

  char *out_id = nullptr;
  ASSERT_EQ(vxcore_node_copy(ctx, notebook_id, "a.md", "dst", "", &out_id),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(out_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_copy_phantom_file_returns_node_not_exists passed" << std::endl;
  return 0;
}

int test_rename_phantom_folder_returns_node_not_exists() {
  std::cout << "  Running test_rename_phantom_folder_returns_node_not_exists..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_rename_phantom_folder_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "f", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  delete_disk_path(content_path(nb_path, "f"));

  ASSERT_EQ(vxcore_node_rename(ctx, notebook_id, "f", "g"), VXCORE_ERR_NODE_NOT_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_rename_phantom_folder_returns_node_not_exists passed" << std::endl;
  return 0;
}

int test_move_phantom_folder_returns_node_not_exists() {
  std::cout << "  Running test_move_phantom_folder_returns_node_not_exists..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_move_phantom_folder_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "f", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);
  folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "dst", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  delete_disk_path(content_path(nb_path, "f"));

  ASSERT_EQ(vxcore_node_move(ctx, notebook_id, "f", "dst"), VXCORE_ERR_NODE_NOT_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_move_phantom_folder_returns_node_not_exists passed" << std::endl;
  return 0;
}

int test_copy_phantom_folder_returns_node_not_exists() {
  std::cout << "  Running test_copy_phantom_folder_returns_node_not_exists..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_copy_phantom_folder_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "f", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);
  folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "dst", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  delete_disk_path(content_path(nb_path, "f"));

  char *out_id = nullptr;
  ASSERT_EQ(vxcore_node_copy(ctx, notebook_id, "f", "dst", "", &out_id),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(out_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_copy_phantom_folder_returns_node_not_exists passed" << std::endl;
  return 0;
}

int test_rename_unindexed_path_still_not_found() {
  std::cout << "  Running test_rename_unindexed_path_still_not_found..." << std::endl;
  const std::string nb_path = get_test_path("test_missing_rename_unindexed_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Test Notebook\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  ASSERT_EQ(vxcore_node_rename(ctx, notebook_id, "never_indexed", "x"), VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_rename_unindexed_path_still_not_found passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  std::cout << "Running missing-nodes (reactive gate) tests..." << std::endl;

  RUN_TEST(test_bundled_file_missing);
  RUN_TEST(test_bundled_folder_missing);
  RUN_TEST(test_unindex_phantom);
  RUN_TEST(test_raw_unchanged);
  RUN_TEST(test_type_mismatch);
  RUN_TEST(test_list_children_marks_missing);
  RUN_TEST(test_list_children_missing_folder);
  RUN_TEST(test_list_children_no_persist);
  RUN_TEST(test_rename_phantom_file_returns_node_not_exists);
  RUN_TEST(test_move_phantom_file_returns_node_not_exists);
  RUN_TEST(test_copy_phantom_file_returns_node_not_exists);
  RUN_TEST(test_rename_phantom_folder_returns_node_not_exists);
  RUN_TEST(test_move_phantom_folder_returns_node_not_exists);
  RUN_TEST(test_copy_phantom_folder_returns_node_not_exists);
  RUN_TEST(test_rename_unindexed_path_still_not_found);

  std::cout << "✓ All missing-nodes tests passed!" << std::endl;
  return 0;
}
