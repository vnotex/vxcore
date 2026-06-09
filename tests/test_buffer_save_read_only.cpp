#include <fstream>
#include <iostream>
#include <string>

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

  // Attempt to save should fail with VXCORE_ERR_READ_ONLY
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // Verify disk content remains unchanged (still the initial content)
  disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, initial_content);

  // Clean up
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_save_ro_bundled"));
  std::cout << "  ✓ test_buffer_save_read_only_bundled passed" << std::endl;
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

  // Attempt to save should fail with VXCORE_ERR_READ_ONLY
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // Verify disk content remains unchanged
  disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, initial_content);

  // Clean up
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_save_ro_raw"));
  std::cout << "  ✓ test_buffer_save_read_only_raw passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running buffer save read-only tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_buffer_save_read_only_bundled);
  RUN_TEST(test_buffer_save_read_only_raw);

  std::cout << "All buffer save read-only tests passed!" << std::endl;
  return 0;
}
