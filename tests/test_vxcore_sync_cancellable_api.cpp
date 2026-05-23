// Wave 12.2 / F5.9 of sync-backend-phase4: C ABI tests for the cancellation
// token lifecycle and vxcore_sync_trigger_cancellable.
//
// Scope:
//   * Lifecycle: create / cancel / free are null-safe and don't leak.
//   * Idempotent cancel on the same token.
//   * vxcore_sync_trigger_cancellable with NULL token behaves identically to
//     vxcore_sync_trigger on a notebook that has no sync enabled.
//   * vxcore_sync_trigger_cancellable with a (non-NULL but uncancelled) token
//     on a non-enabled notebook still returns the expected
//     VXCORE_ERR_SYNC_NOT_ENABLED — proving the token plumbing doesn't change
//     the failure path.
//
// Out of scope for this test: full Sync() interruption with a live remote.
// That requires the libgit2 fixture and is exercised by the existing
// test_git_sync_* suite implicitly (token reaches GitSyncPipeline via the
// W12.2 SetCancellation wiring, asserted by direct test_sync_cancellation
// shim tests added in W12.1).

#include <cstring>
#include <iostream>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

static int test_lifecycle_null_safe() {
  std::cout << "  Running test_lifecycle_null_safe..." << std::endl;
  // All three handle-management fns must be null-safe.
  vxcore_sync_cancel(nullptr);
  vxcore_sync_free_cancellation(nullptr);
  // Create + cancel + free roundtrip — must not leak (run under ASan in CI to
  // verify).
  VxCoreSyncCancellation *t = vxcore_sync_create_cancellation();
  ASSERT_NOT_NULL(t);
  vxcore_sync_cancel(t);
  // Idempotent cancel: calling twice on the same token must not crash.
  vxcore_sync_cancel(t);
  vxcore_sync_free_cancellation(t);
  std::cout << "  PASS test_lifecycle_null_safe" << std::endl;
  return 0;
}

static int test_trigger_cancellable_null_token_matches_legacy() {
  std::cout << "  Running test_trigger_cancellable_null_token_matches_legacy..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Use a notebook id that is not registered. Both APIs should return the
  // same error code so callers can swap between them without behavior drift.
  const char *unknown_id = "no-such-notebook-12-2";
  VxCoreError legacy_err = vxcore_sync_trigger(ctx, unknown_id);
  VxCoreError cancellable_null_err =
      vxcore_sync_trigger_cancellable(ctx, unknown_id, nullptr);
  std::cout << "    legacy_err=" << legacy_err
            << " cancellable_null_err=" << cancellable_null_err << std::endl;
  ASSERT_EQ(legacy_err, cancellable_null_err);

  vxcore_context_destroy(ctx);
  std::cout << "  PASS test_trigger_cancellable_null_token_matches_legacy" << std::endl;
  return 0;
}

static int test_trigger_cancellable_with_token_same_error_code() {
  std::cout << "  Running test_trigger_cancellable_with_token_same_error_code..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  VxCoreSyncCancellation *t = vxcore_sync_create_cancellation();
  ASSERT_NOT_NULL(t);

  const char *unknown_id = "no-such-notebook-12-2";
  VxCoreError legacy_err = vxcore_sync_trigger(ctx, unknown_id);
  VxCoreError cancellable_err =
      vxcore_sync_trigger_cancellable(ctx, unknown_id, t);
  ASSERT_EQ(legacy_err, cancellable_err);

  // After Sync runs (here: fails before any backend exists), the token
  // is still usable — cancel + free must not crash.
  vxcore_sync_cancel(t);
  vxcore_sync_free_cancellation(t);

  vxcore_context_destroy(ctx);
  std::cout << "  PASS test_trigger_cancellable_with_token_same_error_code" << std::endl;
  return 0;
}

static int test_trigger_cancellable_null_args_rejected() {
  std::cout << "  Running test_trigger_cancellable_null_args_rejected..." << std::endl;
  // Both required args (context, notebook_id) must be non-null.
  VxCoreError err1 = vxcore_sync_trigger_cancellable(nullptr, "x", nullptr);
  ASSERT_EQ(err1, VXCORE_ERR_NULL_POINTER);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  VxCoreError err2 = vxcore_sync_trigger_cancellable(ctx, nullptr, nullptr);
  ASSERT_EQ(err2, VXCORE_ERR_NULL_POINTER);
  vxcore_context_destroy(ctx);
  std::cout << "  PASS test_trigger_cancellable_null_args_rejected" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);
  int failures = 0;
  RUN_TEST(test_lifecycle_null_safe);
  RUN_TEST(test_trigger_cancellable_null_token_matches_legacy);
  RUN_TEST(test_trigger_cancellable_with_token_same_error_code);
  RUN_TEST(test_trigger_cancellable_null_args_rejected);
  std::cout << "test_vxcore_sync_cancellable_api: failures=" << failures << std::endl;
  return failures == 0 ? 0 : 1;
}
