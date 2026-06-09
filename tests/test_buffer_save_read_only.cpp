#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"

namespace {

// Helper to read file content
std::string read_file_content(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "";
  }
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return content;
}

// Helper to write a file on disk (used to fabricate a source for InsertAsset/Attachment).
bool write_file_content(const std::string &path, const std::string &content) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  return file.good();
}

}  // namespace

int test_buffer_save_read_only_bundled() {
  std::cout << "  Running test_buffer_save_read_only_bundled..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_save_ro_bundled"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_save_ro_bundled").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Open buffer for the file
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Write initial content to buffer using raw API
  const char *initial_content = "Initial content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, initial_content, strlen(initial_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Subtest 1: Save works when notebook is writable
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file content on disk
  std::string file_path = get_test_path("test_buffer_save_ro_bundled") + "/test.md";
  std::string disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, initial_content);

  // Subtest 2: Save fails when notebook is read-only, and disk content remains unchanged
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to modify buffer content
  const char *new_content = "New content that should not be saved";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, new_content, strlen(new_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Capture original disk bytes BEFORE the read-only save attempt for byte-equality assertion.
  std::string disk_before = read_file_content(file_path);

  // Attempt to save should fail with VXCORE_ERR_READ_ONLY
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // Verify disk content remains byte-for-byte unchanged.
  std::string disk_after = read_file_content(file_path);
  ASSERT_EQ(disk_after, disk_before);
  ASSERT_EQ(disk_after, initial_content);

  // Clean up
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_save_ro_bundled"));
  std::cout << "  PASS test_buffer_save_read_only_bundled" << std::endl;
  return 0;
}

int test_buffer_save_read_only_raw() {
  std::cout << "  Running test_buffer_save_read_only_raw..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_save_ro_raw"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_save_ro_raw").c_str(),
                               "{\"name\":\"Buffer Test Raw\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Open buffer for the file
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Write initial content to buffer using raw API
  const char *initial_content = "Raw notebook content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, initial_content, strlen(initial_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Save the initial content
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file content on disk
  std::string file_path = get_test_path("test_buffer_save_ro_raw") + "/test.md";
  std::string disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, initial_content);

  // Set notebook to read-only
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to modify buffer content
  const char *new_content = "New content that should not be saved to raw";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, new_content, strlen(new_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Capture original disk bytes BEFORE the read-only save attempt.
  std::string disk_before = read_file_content(file_path);

  // Attempt to save should fail with VXCORE_ERR_READ_ONLY
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // Verify disk content remains byte-for-byte unchanged.
  std::string disk_after = read_file_content(file_path);
  ASSERT_EQ(disk_after, disk_before);
  ASSERT_EQ(disk_after, initial_content);

  // Clean up
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_save_ro_raw"));
  std::cout << "  PASS test_buffer_save_read_only_raw" << std::endl;
  return 0;
}

// Verifies that vxcore_buffer_insert_asset_raw is gated by the same
// read-only guard as vxcore_buffer_save: when the owning notebook flips to
// read-only, the disk-write must be refused with VXCORE_ERR_READ_ONLY without
// creating any file in the assets folder.
int test_buffer_insert_asset_raw_read_only() {
  std::cout << "  Running test_buffer_insert_asset_raw_read_only..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_asset_ro"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_asset_ro").c_str(),
                               "{\"name\":\"Asset RO Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "host.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "host.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Sanity: while writable, the asset insert succeeds and produces a non-empty
  // relative path that the assets folder reports.
  const char *first_payload = "first-payload";
  char *first_rel = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "ok.bin", first_payload,
                                       strlen(first_payload), &first_rel);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(first_rel);

  char *assets_folder = nullptr;
  err = vxcore_buffer_get_assets_folder(ctx, buffer_id, &assets_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(assets_folder);
  std::string assets_dir(assets_folder);
  vxcore_string_free(assets_folder);

  // Flip to read-only and confirm the API now refuses the disk-write.
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);

  const char *blocked_payload = "should-never-reach-disk";
  char *blocked_rel = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "blocked.bin", blocked_payload,
                                       strlen(blocked_payload), &blocked_rel);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  // out_relative_path must be untouched / nullptr-equivalent on rejection.
  ASSERT(blocked_rel == nullptr);

  // The blocked asset must not be on disk.
  std::string blocked_path = assets_dir + "/blocked.bin";
  ASSERT(read_file_content(blocked_path).empty());

  vxcore_string_free(first_rel);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_asset_ro"));
  std::cout << "  PASS test_buffer_insert_asset_raw_read_only" << std::endl;
  return 0;
}

// Verifies that vxcore_buffer_insert_attachment is gated by the same
// read-only guard. Same shape as the asset subtest: writable insert works,
// flipping to read-only causes the next insert to be refused with
// VXCORE_ERR_READ_ONLY and no copy on disk.
int test_buffer_insert_attachment_read_only() {
  std::cout << "  Running test_buffer_insert_attachment_read_only..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_attachment_ro"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_attachment_ro").c_str(),
                               "{\"name\":\"Attachment RO Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "host.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "host.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Fabricate a source file outside the notebook to attach.
  std::string src_path = get_test_path("test_buffer_attachment_ro") + "/source.txt";
  ASSERT(write_file_content(src_path, "source-payload"));

  // Sanity: while writable, the attachment insert succeeds.
  char *first_name = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, src_path.c_str(), &first_name);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(first_name);

  char *attachments_folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);
  std::string attachments_dir(attachments_folder);
  vxcore_string_free(attachments_folder);

  // Flip to read-only and confirm the API refuses to copy a second source.
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);

  std::string blocked_src = get_test_path("test_buffer_attachment_ro") + "/blocked.txt";
  ASSERT(write_file_content(blocked_src, "blocked-payload"));

  char *blocked_name = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, blocked_src.c_str(), &blocked_name);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT(blocked_name == nullptr);

  // The blocked attachment must not be on disk.
  std::string blocked_path = attachments_dir + "/blocked.txt";
  ASSERT(read_file_content(blocked_path).empty());

  vxcore_string_free(first_name);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_attachment_ro"));
  std::cout << "  PASS test_buffer_insert_attachment_read_only" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running buffer save read-only tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_buffer_save_read_only_bundled);
  RUN_TEST(test_buffer_save_read_only_raw);
  RUN_TEST(test_buffer_insert_asset_raw_read_only);
  RUN_TEST(test_buffer_insert_attachment_read_only);

  std::cout << "All buffer save read-only tests passed!" << std::endl;
  return 0;
}
