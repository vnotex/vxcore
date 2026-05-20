#include <iostream>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

// Test 1: empty backend does NOT return VXCORE_ERR_UNKNOWN_BACKEND
// (it may fail for other reasons like init, but not for unknown backend)
int test_empty_backend_defaults_to_git() {
  std::cout << "  Running test_empty_backend_defaults_to_git..." << std::endl;
  cleanup_test_dir(get_test_path("test_unknown_backend_1"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_unknown_backend_1").c_str(),
                               "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable with empty backend (should default to git, not fail with unknown backend error)
  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"\",\"remoteUrl\":\"test://repo\"}", nullptr);
  // Should NOT be VXCORE_ERR_UNKNOWN_BACKEND (empty backend is allowed)
  ASSERT_NE(err, VXCORE_ERR_UNKNOWN_BACKEND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_unknown_backend_1"));
  std::cout << "  ✓ test_empty_backend_defaults_to_git passed" << std::endl;
  return 0;
}

// Test 2: unknown backend MUST return VXCORE_ERR_UNKNOWN_BACKEND
// (this is the fail-fast check, should fire before any other error)
int test_unknown_backend_returns_error() {
  std::cout << "  Running test_unknown_backend_returns_error..." << std::endl;
  cleanup_test_dir(get_test_path("test_unknown_backend_2"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_unknown_backend_2").c_str(),
                               "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable with unknown backend (should fail with VXCORE_ERR_UNKNOWN_BACKEND)
  // This is the fail-fast check, so it should fire BEFORE the init check
  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"webdav\",\"remoteUrl\":\"test://repo\"}", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_unknown_backend_2"));
  std::cout << "  ✓ test_unknown_backend_returns_error passed" << std::endl;
  return 0;
}

// Test 3: git backend does NOT return VXCORE_ERR_UNKNOWN_BACKEND
// (it may fail for other reasons, but not for unknown backend)
int test_git_backend_succeeds() {
  std::cout << "  Running test_git_backend_succeeds..." << std::endl;
  cleanup_test_dir(get_test_path("test_unknown_backend_3"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_unknown_backend_3").c_str(),
                               "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable with git backend (should NOT return VXCORE_ERR_UNKNOWN_BACKEND)
  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"git\",\"remoteUrl\":\"test://repo\"}", nullptr);
  // We don't assert VXCORE_OK because Initialize might fail for other reasons
  // (e.g., invalid remote URL), but it should NOT be VXCORE_ERR_UNKNOWN_BACKEND
  ASSERT_NE(err, VXCORE_ERR_UNKNOWN_BACKEND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_unknown_backend_3"));
  std::cout << "  ✓ test_git_backend_succeeds passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running test_sync_manager_unknown_backend tests..." << std::endl;

  RUN_TEST(test_empty_backend_defaults_to_git);
  RUN_TEST(test_unknown_backend_returns_error);
  RUN_TEST(test_git_backend_succeeds);

  std::cout << "All tests passed!" << std::endl;
  return 0;
}
