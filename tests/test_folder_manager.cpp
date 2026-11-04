// Tests for FolderManager attachment interfaces.
// This test links directly against folder manager sources instead of vxcore library.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <sqlite3.h>

#include "core/bundled_folder_manager.h"
#include "core/bundled_notebook.h"
#include "core/folder_manager.h"
#include "core/notebook.h"
#include "core/metadata_store.h"
#include "test_utils.h"

using namespace vxcore;

// Helper to create a bundled notebook for testing directly (bypassing NotebookManager)
static std::unique_ptr<Notebook> create_test_notebook(const std::string &root_folder) {
  NotebookConfig config;
  config.name = "Test Notebook";

  std::unique_ptr<Notebook> notebook;
  // Use empty string for local_data_folder since bundled notebooks store metadata in root
  VxCoreError err = BundledNotebook::Create("", root_folder, &config, notebook);
  if (err != VXCORE_OK) {
    return nullptr;
  }
  return notebook;
}

int test_folder_manager_add_attachment() {
  std::cout << "  Running test_folder_manager_add_attachment..." << std::endl;
  std::string test_path = get_test_path("test_fm_add_attach");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create a file first
  std::string file_id;
  VxCoreError err = fm->CreateFile(".", "note.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Add first attachment
  err = fm->AddFileAttachment("note.md", "image1.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify
  std::string attachments_json;
  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 1);
  ASSERT_EQ(attachments[0], "image1.png");

  // Add second attachment
  err = fm->AddFileAttachment("note.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_OK);

  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 2);
  ASSERT_EQ(attachments[0], "image1.png");
  ASSERT_EQ(attachments[1], "doc.pdf");

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_add_attachment passed" << std::endl;
  return 0;
}

int test_folder_manager_add_attachment_duplicate() {
  std::cout << "  Running test_folder_manager_add_attachment_duplicate..." << std::endl;
  std::string test_path = get_test_path("test_fm_add_dup");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create a file
  std::string file_id;
  VxCoreError err = fm->CreateFile(".", "note.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Add attachment
  err = fm->AddFileAttachment("note.md", "image.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Add same attachment again (should be no-op, deduplicated)
  err = fm->AddFileAttachment("note.md", "image.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify only one attachment exists
  std::string attachments_json;
  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 1);
  ASSERT_EQ(attachments[0], "image.png");

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_add_attachment_duplicate passed" << std::endl;
  return 0;
}

int test_folder_manager_delete_attachment() {
  std::cout << "  Running test_folder_manager_delete_attachment..." << std::endl;
  std::string test_path = get_test_path("test_fm_del_attach");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create a file
  std::string file_id;
  VxCoreError err = fm->CreateFile(".", "note.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Add multiple attachments
  err = fm->AddFileAttachment("note.md", "image1.png");
  ASSERT_EQ(err, VXCORE_OK);
  err = fm->AddFileAttachment("note.md", "image2.png");
  ASSERT_EQ(err, VXCORE_OK);
  err = fm->AddFileAttachment("note.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_OK);

  // Delete middle attachment
  err = fm->DeleteFileAttachment("note.md", "image2.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify remaining attachments
  std::string attachments_json;
  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 2);
  ASSERT_EQ(attachments[0], "image1.png");
  ASSERT_EQ(attachments[1], "doc.pdf");

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_delete_attachment passed" << std::endl;
  return 0;
}

int test_folder_manager_delete_attachment_not_found() {
  std::cout << "  Running test_folder_manager_delete_attachment_not_found..." << std::endl;
  std::string test_path = get_test_path("test_fm_del_nf");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create a file
  std::string file_id;
  VxCoreError err = fm->CreateFile(".", "note.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Add attachment
  err = fm->AddFileAttachment("note.md", "image.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Delete non-existent attachment (should be no-op, returns OK)
  err = fm->DeleteFileAttachment("note.md", "nonexistent.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify original attachment still exists
  std::string attachments_json;
  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 1);
  ASSERT_EQ(attachments[0], "image.png");

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_delete_attachment_not_found passed" << std::endl;
  return 0;
}

int test_folder_manager_attachment_file_not_found() {
  std::cout << "  Running test_folder_manager_attachment_file_not_found..." << std::endl;
  std::string test_path = get_test_path("test_fm_attach_fnf");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Try to add attachment to non-existent file
  VxCoreError err = fm->AddFileAttachment("nonexistent.md", "image.png");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Try to delete attachment from non-existent file
  err = fm->DeleteFileAttachment("nonexistent.md", "image.png");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Try to get attachments from non-existent file
  std::string attachments_json;
  err = fm->GetFileAttachments("nonexistent.md", attachments_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_attachment_file_not_found passed" << std::endl;
  return 0;
}

int test_folder_manager_attachment_nested_file() {
  std::cout << "  Running test_folder_manager_attachment_nested_file..." << std::endl;
  std::string test_path = get_test_path("test_fm_attach_nested");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create nested folder structure
  std::string folder_id;
  VxCoreError err = fm->CreateFolder(".", "docs", folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = fm->CreateFolder("docs", "notes", folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file in nested folder
  std::string file_id;
  err = fm->CreateFile("docs/notes", "readme.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Add attachment to nested file
  err = fm->AddFileAttachment("docs/notes/readme.md", "diagram.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify attachment
  std::string attachments_json;
  err = fm->GetFileAttachments("docs/notes/readme.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 1);
  ASSERT_EQ(attachments[0], "diagram.png");

  // Delete attachment
  err = fm->DeleteFileAttachment("docs/notes/readme.md", "diagram.png");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify deleted
  err = fm->GetFileAttachments("docs/notes/readme.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 0);

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_attachment_nested_file passed" << std::endl;
  return 0;
}

int test_folder_manager_update_attachments() {
  std::cout << "  Running test_folder_manager_update_attachments..." << std::endl;
  std::string test_path = get_test_path("test_fm_update_attach");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create a file
  std::string file_id;
  VxCoreError err = fm->CreateFile(".", "note.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Update attachments with a JSON array
  nlohmann::json new_attachments = {"img1.png", "img2.png", "doc.pdf"};
  err = fm->UpdateFileAttachments("note.md", new_attachments.dump());
  ASSERT_EQ(err, VXCORE_OK);

  // Verify attachments
  std::string attachments_json;
  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 3);
  ASSERT_EQ(attachments[0], "img1.png");
  ASSERT_EQ(attachments[1], "img2.png");
  ASSERT_EQ(attachments[2], "doc.pdf");

  // Update with empty array (clear all)
  nlohmann::json empty_attachments = nlohmann::json::array();
  err = fm->UpdateFileAttachments("note.md", empty_attachments.dump());
  ASSERT_EQ(err, VXCORE_OK);

  err = fm->GetFileAttachments("note.md", attachments_json);
  ASSERT_EQ(err, VXCORE_OK);

  attachments = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(attachments.size(), 0);

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_update_attachments passed" << std::endl;
  return 0;
}

int test_folder_manager_update_attachments_invalid_json() {
  std::cout << "  Running test_folder_manager_update_attachments_invalid_json..." << std::endl;
  std::string test_path = get_test_path("test_fm_update_invalid");
  cleanup_test_dir(test_path);

  auto notebook = create_test_notebook(test_path);
  ASSERT_NOT_NULL(notebook.get());

  FolderManager *fm = notebook->GetFolderManager();
  ASSERT_NOT_NULL(fm);

  // Create a file
  std::string file_id;
  VxCoreError err = fm->CreateFile(".", "note.md", file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to update with invalid JSON
  err = fm->UpdateFileAttachments("note.md", "not valid json");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  // Try to update with non-array JSON
  err = fm->UpdateFileAttachments("note.md", "{\"key\": \"value\"}");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  cleanup_test_dir(test_path);
  std::cout << "  test_folder_manager_update_attachments_invalid_json passed" << std::endl;
  return 0;
}

static std::string manager_binary(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int test_attachment_errors_reload_actual_metadata() {
  for (const std::string operation : {"replace", "add", "delete"}) {
    const auto path = get_test_path("test_fm_atomic_attachment_" + operation);
    cleanup_test_dir(path);
    auto notebook = create_test_notebook(path);
    ASSERT_NOT_NULL(notebook.get());
    auto *fm = notebook->GetFolderManager();
    std::string id;
    ASSERT_EQ(fm->CreateFile(".", "note.md", id), VXCORE_OK);
    ASSERT_EQ(fm->AddFileAttachment("note.md", "original.bin"), VXCORE_OK);
    ASSERT_EQ(fm->CreateFile(".", "unrelated.md", id), VXCORE_OK);
    const auto config_path = utf8_to_fs_path(path + "/vx_notebook/contents/vx.json");
    const auto before = manager_binary(config_path);
    std::string cached;
    ASSERT_EQ(fm->GetFileAttachments("note.md", cached), VXCORE_OK);
    auto saved = config_path;
    saved += ".saved";
    std::filesystem::rename(config_path, saved);
    std::filesystem::create_directory(config_path);
    VxCoreError error = VXCORE_OK;
    if (operation == "replace") error = fm->UpdateFileAttachments("note.md", "[\"replacement.bin\"]");
    else if (operation == "add") error = fm->AddFileAttachment("note.md", "new.bin");
    else error = fm->DeleteFileAttachment("note.md", "original.bin");
    std::filesystem::remove(config_path);
    std::filesystem::rename(saved, config_path);
    ASSERT_NE(error, VXCORE_OK);
    ASSERT_EQ(manager_binary(config_path), before);
    std::string actual;
    ASSERT_EQ(fm->GetFileAttachments("note.md", actual), VXCORE_OK);
    ASSERT_EQ(actual, cached);
    fm->ClearCache();
    ASSERT_EQ(fm->GetFileAttachments("note.md", actual), VXCORE_OK);
    ASSERT_EQ(actual, cached);
    notebook.reset();
    cleanup_test_dir(path);
  }
  return 0;
}

int test_attachment_atomic_update_preserves_unrelated_json() {
  const auto path = get_test_path("test_fm_attachment_unknown_fields");
  cleanup_test_dir(path);
  auto notebook = create_test_notebook(path);
  ASSERT_NOT_NULL(notebook.get());
  auto *fm = notebook->GetFolderManager();
  std::string id;
  ASSERT_EQ(fm->CreateFile(".", "note.md", id), VXCORE_OK);
  ASSERT_EQ(fm->CreateFile(".", "other.md", id), VXCORE_OK);
  const auto config_path = utf8_to_fs_path(path + "/vx_notebook/contents/vx.json");
  auto config = nlohmann::json::parse(manager_binary(config_path));
  config["opaqueRoot"] = {1, "keep"};
  config["files"][0]["opaqueFile"] = {"keep", 2};
  config["files"][1]["metadata"] = {{"owner", "untouched"}};
  config["files"][1]["tags"] = {"tag-a", "tag-b"};
  config["files"][1]["attachments"] = {"other.bin"};
  const auto unrelated = config["files"][1];
  write_file(path + "/vx_notebook/contents/vx.json", config.dump(2));
  fm->ClearCache();
  ASSERT_EQ(fm->UpdateFileAttachments("note.md", "[\"first.bin\",\"second.bin\"]"), VXCORE_OK);
  const auto after = nlohmann::json::parse(manager_binary(config_path));
  ASSERT_EQ(after["opaqueRoot"], config["opaqueRoot"]);
  ASSERT_EQ(after["files"][0]["opaqueFile"], config["files"][0]["opaqueFile"]);
  ASSERT_EQ(after["files"][1], unrelated);
  ASSERT_EQ(after["files"][0]["attachments"], nlohmann::json({"first.bin", "second.bin"}));
  notebook.reset();
  cleanup_test_dir(path);
  return 0;
}

int test_index_children_bulk_preserves_binary_sources_and_store() {
  const auto path = get_test_path("test_fm_bulk_index_children");
  cleanup_test_dir(path);
  auto notebook = create_test_notebook(path);
  ASSERT_NOT_NULL(notebook.get());
  auto *fm = dynamic_cast<BundledFolderManager *>(notebook->GetFolderManager());
  ASSERT_NOT_NULL(fm);
  nlohmann::json names = nlohmann::json::array();
  for (int i = 0; i < 10000; ++i) {
    const auto name = std::string("entry-") + std::to_string(i) + ".bin";
    names.push_back(name);
    write_file(path + "/" + name, std::string("\0\xff", 2) + std::to_string(i) + "\r\n");
  }
  ASSERT_EQ(fm->IndexChildren(".", names.dump()), VXCORE_OK);
  std::string json;
  ASSERT_EQ(fm->GetFolderConfig(".", json), VXCORE_OK);
  const auto config = nlohmann::json::parse(json);
  ASSERT_EQ(config["files"].size(), size_t(10000));
  std::unordered_set<std::string> ids;
  std::unordered_set<std::string> indexed;
  auto *store = notebook->GetMetadataStore();
  ASSERT_NOT_NULL(store);
  for (const auto &file : config["files"]) {
    const auto id = file["id"].get<std::string>();
    const auto name = file["name"].get<std::string>();
    ASSERT_TRUE(!id.empty() && ids.insert(id).second);
    ASSERT_TRUE(indexed.insert(name).second);
    const auto stored = store->GetFile(id);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->name, name);
  }
  for (int i = 0; i < 10000; ++i) {
    const auto name = std::string("entry-") + std::to_string(i) + ".bin";
    ASSERT_TRUE(indexed.count(name) != 0);
    ASSERT_EQ(manager_binary(utf8_to_fs_path(path + "/" + name)),
              std::string("\0\xff", 2) + std::to_string(i) + "\r\n");
  }
  ASSERT_EQ(nlohmann::json::parse(manager_binary(utf8_to_fs_path(path + "/vx_notebook/contents/vx.json"))), config);
  notebook.reset();
  cleanup_test_dir(path);
  return 0;
}

int test_index_children_rejects_aliases_and_foreign_configs() {
  const auto path = get_test_path("test_fm_index_children_ownership");
  cleanup_test_dir(path);
  auto notebook = create_test_notebook(path);
  ASSERT_NOT_NULL(notebook.get());
  auto *fm = dynamic_cast<BundledFolderManager *>(notebook->GetFolderManager());
  ASSERT_NOT_NULL(fm);
  create_directory(path + "/Folder");
  write_file(path + "/Folder/opaque.bin", std::string("\0\xfe\r\n", 4));
  const auto config_path = utf8_to_fs_path(path + "/vx_notebook/contents/vx.json");
  const auto before = manager_binary(config_path);
  ASSERT_NE(fm->IndexChildren(".", "[\"Folder\",\"missing.md\"]"), VXCORE_OK);
  ASSERT_FALSE(path_exists(path + "/vx_notebook/contents/Folder"));
  ASSERT_EQ(manager_binary(config_path), before);
  create_directory(path + "/vx_notebook/contents/Folder");
  write_file(path + "/vx_notebook/contents/Folder/vx.json", "foreign orphan");
  ASSERT_NE(fm->IndexChildren(".", "[\"Folder\"]"), VXCORE_OK);
  ASSERT_EQ(manager_binary(utf8_to_fs_path(path + "/vx_notebook/contents/Folder/vx.json")), "foreign orphan");
  ASSERT_EQ(manager_binary(config_path), before);
  std::filesystem::remove_all(utf8_to_fs_path(path + "/vx_notebook/contents/Folder"));
  write_file(path + "/one.bin", "hardlinked bytes");
  std::error_code ec;
  std::filesystem::create_hard_link(utf8_to_fs_path(path + "/one.bin"), utf8_to_fs_path(path + "/two.bin"), ec);
  ASSERT_FALSE(bool(ec));
  ASSERT_NE(fm->IndexChildren(".", "[\"Folder\",\"one.bin\",\"two.bin\"]"), VXCORE_OK);
  ASSERT_FALSE(path_exists(path + "/vx_notebook/contents/Folder"));
  ASSERT_EQ(manager_binary(config_path), before);
  ASSERT_EQ(fm->IndexChildren(".", "[\"Folder\"]"), VXCORE_OK);
  ASSERT_EQ(manager_binary(utf8_to_fs_path(path + "/Folder/opaque.bin")), std::string("\0\xfe\r\n", 4));
  const auto child = nlohmann::json::parse(manager_binary(utf8_to_fs_path(path + "/vx_notebook/contents/Folder/vx.json")));
  ASSERT_TRUE(child["files"].empty());
  ASSERT_TRUE(child["folders"].empty());
  write_file(path + "/vx.json", "opaque source bytes, not metadata");
  ASSERT_EQ(fm->IndexChildren(".", "[\"vx.json\"]"), VXCORE_OK);
  ASSERT_EQ(manager_binary(utf8_to_fs_path(path + "/vx.json")), "opaque source bytes, not metadata");
  notebook.reset();
  cleanup_test_dir(path);
  return 0;
}

int test_index_children_failed_parent_publication_cleans_owned_configs() {
#ifdef _WIN32
  const auto path = get_test_path("test_fm_index_children_retry");
  cleanup_test_dir(path);
  auto notebook = create_test_notebook(path);
  ASSERT_NOT_NULL(notebook.get());
  auto *fm = dynamic_cast<BundledFolderManager *>(notebook->GetFolderManager());
  ASSERT_NOT_NULL(fm);
  create_directory(path + "/Folder");
  write_file(path + "/Folder/opaque.bin", "unchanged");
  const auto config_path = utf8_to_fs_path(path + "/vx_notebook/contents/vx.json");
  const auto before = manager_binary(config_path);
  const DWORD attributes = GetFileAttributesW(config_path.c_str());
  ASSERT_TRUE(attributes != INVALID_FILE_ATTRIBUTES);
  ASSERT_TRUE(SetFileAttributesW(config_path.c_str(), attributes | FILE_ATTRIBUTE_READONLY) != 0);
  const auto error = fm->IndexChildren(".", "[\"Folder\"]");
  ASSERT_TRUE(SetFileAttributesW(config_path.c_str(), attributes) != 0);
  ASSERT_NE(error, VXCORE_OK);
  ASSERT_EQ(manager_binary(config_path), before);
  ASSERT_FALSE(path_exists(path + "/vx_notebook/contents/Folder"));
  ASSERT_EQ(manager_binary(utf8_to_fs_path(path + "/Folder/opaque.bin")), "unchanged");
  ASSERT_EQ(fm->IndexChildren(".", "[\"Folder\"]"), VXCORE_OK);
  ASSERT_TRUE(path_exists(path + "/vx_notebook/contents/Folder/vx.json"));
  notebook.reset();
  cleanup_test_dir(path);
#endif
  return 0;
}

int test_lazy_cache_folder_publication_is_atomic() {
  const auto path = get_test_path("test_fm_lazy_cache_atomic");
  cleanup_test_dir(path);
  const auto book = path + "/book";
  NotebookConfig options;
  options.name = "Cache transaction";
  std::unique_ptr<Notebook> notebook;
  ASSERT_EQ(BundledNotebook::Create(path + "/local", book, &options, notebook), VXCORE_OK);
  auto *fm = notebook->GetFolderManager();
  auto *store = notebook->GetMetadataStore();
  ASSERT_NOT_NULL(fm);
  ASSERT_NOT_NULL(store);
  std::string first, late, tail, child, nested;
  ASSERT_EQ(fm->CreateFile(".", "first.md", first), VXCORE_OK);
  ASSERT_EQ(fm->CreateFile(".", "late.md", late), VXCORE_OK);
  ASSERT_EQ(fm->CreateFile(".", "tail.md", tail), VXCORE_OK);
  ASSERT_EQ(fm->CreateFolder(".", "Child", child), VXCORE_OK);
  ASSERT_EQ(fm->CreateFile("Child", "nested.md", nested), VXCORE_OK);
  write_file(book + "/first.md", "first source\r\n");
  write_file(book + "/late.md", "late source\n");
  write_file(book + "/tail.md", "tail source\n");
  write_file(book + "/Child/nested.md", "nested source\n");
  const auto root_path = utf8_to_fs_path(book + "/vx_notebook/contents/vx.json");
  const auto child_path = utf8_to_fs_path(book + "/vx_notebook/contents/Child/vx.json");
  const auto root_bytes = manager_binary(root_path);
  const auto child_bytes = manager_binary(child_path);
  const auto root_id = nlohmann::json::parse(root_bytes)["id"].get<std::string>();
  ASSERT_TRUE(store->RebuildAll());
  ASSERT_TRUE(store->SetNotebookMetadata("last_sync_utc", "123456789"));
  sqlite3 *raw = nullptr;
  const auto db_path = path + "/local/notebooks/" + notebook->GetConfig().id + "/metadata.db";
  ASSERT_EQ(sqlite3_open_v2(db_path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
  ASSERT_EQ(sqlite3_exec(database.get(),
      "CREATE TRIGGER reject_late_cache_file BEFORE INSERT ON files "
      "WHEN NEW.name='late.md' BEGIN SELECT RAISE(ABORT, 'controlled cache failure'); END;",
      nullptr, nullptr, nullptr), SQLITE_OK);
  fm->ClearCache();
  std::string config;
  ASSERT_EQ(fm->GetFolderConfig(".", config), VXCORE_OK);
  ASSERT_FALSE(store->GetFolder(root_id).has_value());
  ASSERT_FALSE(store->GetFile(first).has_value());
  ASSERT_FALSE(store->GetFile(late).has_value());
  ASSERT_FALSE(store->GetFile(tail).has_value());
  ASSERT_FALSE(store->GetFolder(child).has_value());
  ASSERT_FALSE(store->GetFile(nested).has_value());
  ASSERT_EQ(store->GetNotebookMetadata("last_sync_utc").value_or("missing"), "123456789");
  ASSERT_EQ(sqlite3_exec(database.get(), "DROP TRIGGER reject_late_cache_file;", nullptr, nullptr, nullptr), SQLITE_OK);
  database.reset();
  fm->ClearCache();
  ASSERT_EQ(fm->GetFolderConfig(".", config), VXCORE_OK);
  ASSERT_TRUE(store->GetFolder(root_id).has_value());
  ASSERT_TRUE(store->GetFile(first).has_value());
  ASSERT_TRUE(store->GetFile(late).has_value());
  ASSERT_TRUE(store->GetFile(tail).has_value());
  ASSERT_TRUE(store->GetFolder(child).has_value());
  ASSERT_TRUE(store->GetFile(nested).has_value());
  ASSERT_EQ(store->GetNotebookMetadata("last_sync_utc").value_or("missing"), "123456789");
  ASSERT_EQ(manager_binary(root_path), root_bytes);
  ASSERT_EQ(manager_binary(child_path), child_bytes);
  ASSERT_EQ(manager_binary(utf8_to_fs_path(book + "/first.md")), "first source\r\n");
  ASSERT_EQ(manager_binary(utf8_to_fs_path(book + "/late.md")), "late source\n");
  ASSERT_EQ(manager_binary(utf8_to_fs_path(book + "/tail.md")), "tail source\n");
  ASSERT_EQ(manager_binary(utf8_to_fs_path(book + "/Child/nested.md")), "nested source\n");
  notebook.reset();
  cleanup_test_dir(path);
  return 0;
}

int main() {
  std::cout << "Running folder manager attachment tests..." << std::endl;

  RUN_TEST(test_lazy_cache_folder_publication_is_atomic);
  RUN_TEST(test_folder_manager_add_attachment);
  RUN_TEST(test_folder_manager_add_attachment_duplicate);
  RUN_TEST(test_folder_manager_delete_attachment);
  RUN_TEST(test_folder_manager_delete_attachment_not_found);
  RUN_TEST(test_folder_manager_attachment_file_not_found);
  RUN_TEST(test_folder_manager_attachment_nested_file);
  RUN_TEST(test_folder_manager_update_attachments);
  RUN_TEST(test_folder_manager_update_attachments_invalid_json);
  RUN_TEST(test_attachment_errors_reload_actual_metadata);
  RUN_TEST(test_attachment_atomic_update_preserves_unrelated_json);
  RUN_TEST(test_index_children_bulk_preserves_binary_sources_and_store);
  RUN_TEST(test_index_children_rejects_aliases_and_foreign_configs);
  RUN_TEST(test_index_children_failed_parent_publication_cleans_owned_configs);

  std::cout << "All folder manager attachment tests passed!" << std::endl;
  return 0;
}
