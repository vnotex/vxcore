#include <iostream>
#include <string>

#include "sync/git/libgit2_init.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

// Successful construction makes libgit2 available to callers.
int test_ok_true_after_init_success() {
  std::cout << "  Running test_ok_true_after_init_success..." << std::endl;

  // Construct a LibGit2Init (should call git_libgit2_init)
  vxcore::LibGit2Init guard;

  // After construction, ok() should return true (init succeeded)
  bool ok_after = vxcore::LibGit2Init::ok();
  ASSERT_TRUE(ok_after);

  std::cout << "  ✓ test_ok_true_after_init_success passed" << std::endl;
  return 0;
}

// Reject unregistered backends before attempting to use their configuration.
int test_unknown_backend_rejected() {
  std::cout << "  Running test_unknown_backend_rejected..." << std::endl;

  vxcore_set_test_mode(1);
  cleanup_test_dir(get_test_path("test_libgit2_init_prop"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("test_libgit2_init_prop").c_str(),
                             "{\"name\":\"Test Notebook\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_enable(
      ctx, notebook_id, "{\"backend\":\"unsupported-test-backend\",\"remoteUrl\":\"test://repo\"}",
      nullptr);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_libgit2_init_prop"));

  std::cout << "  ✓ test_unknown_backend_rejected passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running test_libgit2_init_propagation tests..." << std::endl;

  RUN_TEST(test_ok_true_after_init_success);
  RUN_TEST(test_unknown_backend_rejected);

  std::cout << "All tests passed!" << std::endl;
  return 0;
}
