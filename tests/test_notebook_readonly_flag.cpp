#include <iostream>
#include <string>

#include "core/context.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

// Test 1: Newly opened bundled notebook defaults to IsReadOnly() == false
int test_readonly_flag_defaults_false() {
  std::cout << "  Running test_readonly_flag_defaults_false..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_readonly_default").c_str(),
                               "{\"name\":\"ReadOnly Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Verify that notebook is not read-only by default
  bool is_readonly = false;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(is_readonly);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_readonly_default"));
  std::cout << "    \xE2\x9C\x93 test_readonly_flag_defaults_false passed" << std::endl;
  return 0;
}

// Test 2: After SetReadOnly(true), IsReadOnly() returns true
int test_readonly_flag_set_true() {
  std::cout << "  Running test_readonly_flag_set_true..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_readonly_set_true").c_str(),
                               "{\"name\":\"ReadOnly Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Set read-only to true
  err = vxcore_notebook_set_read_only(ctx, notebook_id, true);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify it is now read-only
  bool is_readonly = false;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(is_readonly);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_readonly_set_true"));
  std::cout << "    \xE2\x9C\x93 test_readonly_flag_set_true passed" << std::endl;
  return 0;
}

// Test 3: After SetReadOnly(false), IsReadOnly() returns false
int test_readonly_flag_set_false() {
  std::cout << "  Running test_readonly_flag_set_false..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_readonly_set_false").c_str(),
                               "{\"name\":\"ReadOnly Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Set read-only to true, then to false
  err = vxcore_notebook_set_read_only(ctx, notebook_id, true);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_set_read_only(ctx, notebook_id, false);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify it is now NOT read-only
  bool is_readonly = true;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(is_readonly);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_readonly_set_false"));
  std::cout << "    \xE2\x9C\x93 test_readonly_flag_set_false passed" << std::endl;
  return 0;
}

// Test 4: The flag is independent of modified_ and other existing booleans
// (no accidental shared state)
int test_readonly_flag_independent_from_modified() {
  std::cout << "  Running test_readonly_flag_independent_from_modified..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_readonly_independent").c_str(),
                               "{\"name\":\"ReadOnly Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Set read-only to true
  err = vxcore_notebook_set_read_only(ctx, notebook_id, true);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify read-only is true
  bool is_readonly = false;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(is_readonly);

  // Modify the config (which may set some other internal flag)
  err = vxcore_notebook_update_config(ctx, notebook_id, "{\"name\":\"Updated Name\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify read-only flag is still true (independent)
  is_readonly = false;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(is_readonly);

  // Set read-only to false again
  err = vxcore_notebook_set_read_only(ctx, notebook_id, false);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify it's false
  is_readonly = true;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(is_readonly);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_readonly_independent"));
  std::cout << "    \xE2\x9C\x93 test_readonly_flag_independent_from_modified passed"
            << std::endl;
  return 0;
}

}  // namespace

int main() {
  std::cout << "Running notebook readonly flag tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_readonly_flag_defaults_false);
  RUN_TEST(test_readonly_flag_set_true);
  RUN_TEST(test_readonly_flag_set_false);
  RUN_TEST(test_readonly_flag_independent_from_modified);

  std::cout << "\xE2\x9C\x93 All notebook readonly flag tests passed" << std::endl;
  return 0;
}
