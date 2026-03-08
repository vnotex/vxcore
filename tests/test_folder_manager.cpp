// Tests for FolderManager attachment interfaces.
// This test links directly against folder manager sources instead of vxcore library.

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/bundled_folder_manager.h"
#include "core/bundled_notebook.h"
#include "core/folder_manager.h"
#include "core/notebook.h"
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

int main() {
  std::cout << "Running folder manager attachment tests..." << std::endl;

  RUN_TEST(test_folder_manager_add_attachment);
  RUN_TEST(test_folder_manager_add_attachment_duplicate);
  RUN_TEST(test_folder_manager_delete_attachment);
  RUN_TEST(test_folder_manager_delete_attachment_not_found);
  RUN_TEST(test_folder_manager_attachment_file_not_found);
  RUN_TEST(test_folder_manager_attachment_nested_file);
  RUN_TEST(test_folder_manager_update_attachments);
  RUN_TEST(test_folder_manager_update_attachments_invalid_json);

  std::cout << "All folder manager attachment tests passed!" << std::endl;
  return 0;
}
