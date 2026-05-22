// W11.2 / Task 11.2 of sync-backend-phase4 (B3): unit tests for
// ClassifyGitOp + GitOpErrorMessage in git_error_translator.cpp.
//
// Direct-compile pattern: ClassifyGitOp is not VXCORE_API-exported and is
// internal to vxcore.dll. We compile git_error_translator.cpp directly into
// the test (mirrors test_dirty_tracker / test_retry_policy). Links libgit2
// for git_error_last() interaction in the message-driven branches; tests
// that exercise message-driven classification do NOT need a real libgit2
// error to be live since ClassifyGitOp reads its msg argument first and
// only consults git_error_last() for klass.

#include <git2.h>

#include <iostream>
#include <string>

#include "sync/git/git_error_translator.h"
#include "test_utils.h"

// vxcore_set_test_mode shim -- same pattern as test_dirty_tracker.cpp /
// test_retry_policy.cpp. We do not link vxcore.dll, so the public symbol
// is unavailable; a no-op local stub satisfies the linker.
namespace {
void vxcore_set_test_mode(int) {}
}  // namespace

using vxcore::ClassifyGitOp;
using vxcore::GitOpError;
using vxcore::GitOpErrorMessage;

// libgit2 error code constants re-derived locally so the test does not need
// to share <git2.h>'s integer values across translation units. Values match
// libs/vxcore/third_party/libgit2/include/git2/errors.h.
static constexpr int kGitOk = 0;
static constexpr int kGitError = -1;
static constexpr int kGitENotFound = -3;
static constexpr int kGitEUser = -7;
static constexpr int kGitEUnmerged = -10;
static constexpr int kGitENonFastForward = -11;
static constexpr int kGitEAuth = -16;
static constexpr int kGitECertificate = -17;
static constexpr int kGitEUncommitted = -22;

static int test_none_on_zero_rc() {
  ASSERT_TRUE(ClassifyGitOp(kGitOk, "") == GitOpError::None);
  ASSERT_TRUE(ClassifyGitOp(kGitOk, "anything") == GitOpError::None);
  std::cout << "  PASS test_none_on_zero_rc" << std::endl;
  return 0;
}

static int test_auth_failure_rc() {
  // GIT_EAUTH is the canonical auth signal regardless of message.
  ASSERT_TRUE(ClassifyGitOp(kGitEAuth, "") == GitOpError::AuthFailure);
  ASSERT_TRUE(ClassifyGitOp(kGitEAuth, "anything") == GitOpError::AuthFailure);
  std::cout << "  PASS test_auth_failure_rc" << std::endl;
  return 0;
}

static int test_network_unavailable_cert() {
  // GIT_ECERTIFICATE is rc-driven so works without klass.
  ASSERT_TRUE(ClassifyGitOp(kGitECertificate, "") == GitOpError::NetworkUnavailable);
  std::cout << "  PASS test_network_unavailable_cert" << std::endl;
  return 0;
}

static int test_non_fast_forward_rc() {
  // GIT_ENONFASTFORWARD is the structured rc; no message needed.
  ASSERT_TRUE(ClassifyGitOp(kGitENonFastForward, "") == GitOpError::NonFastForward);
  std::cout << "  PASS test_non_fast_forward_rc" << std::endl;
  return 0;
}

static int test_merge_conflict_rc() {
  ASSERT_TRUE(ClassifyGitOp(kGitEUnmerged, "") == GitOpError::MergeConflict);
  std::cout << "  PASS test_merge_conflict_rc" << std::endl;
  return 0;
}

static int test_repo_corrupt_rc() {
  ASSERT_TRUE(ClassifyGitOp(kGitEUncommitted, "") == GitOpError::RepoCorrupt);
  std::cout << "  PASS test_repo_corrupt_rc" << std::endl;
  return 0;
}

static int test_non_fast_forward_message() {
  // The canonical W11.1-era trigger: rc == -1 (GIT_ERROR), no special klass,
  // signal lives in the message text. Verifies all four substring variants.
  ASSERT_TRUE(ClassifyGitOp(kGitError, "cannot push non-fast-forward update") ==
              GitOpError::NonFastForward);
  ASSERT_TRUE(ClassifyGitOp(kGitError, "non fast forward") ==
              GitOpError::NonFastForward);
  ASSERT_TRUE(ClassifyGitOp(kGitError, "fast-forward update would be lost") ==
              GitOpError::NonFastForward);
  ASSERT_TRUE(ClassifyGitOp(kGitError, "remote rejected push") ==
              GitOpError::NonFastForward);
  std::cout << "  PASS test_non_fast_forward_message" << std::endl;
  return 0;
}

static int test_cancelled_on_giteuser() {
  // W12.1: GIT_EUSER (-7) is libgit2's signal for "a user callback returned
  // nonzero" -- in vxcore that means a SyncCancellation token was tripped
  // from the progress callback. Must classify to GitOpError::Cancelled
  // independently of message text.
  ASSERT_TRUE(ClassifyGitOp(kGitEUser, "") == GitOpError::Cancelled);
  ASSERT_TRUE(ClassifyGitOp(kGitEUser, "user cancelled the operation") ==
              GitOpError::Cancelled);
  std::cout << "  PASS test_cancelled_on_giteuser" << std::endl;
  return 0;
}

static int test_unknown_default() {
  // No matching rc, no matching klass, no matching message keyword.
  // GIT_ENOTFOUND (-3) deliberately maps to Unknown in this classifier --
  // VxCoreError mapping handles NOT_FOUND separately.
  ASSERT_TRUE(ClassifyGitOp(kGitError, "totally unrelated error blob") ==
              GitOpError::Unknown);
  ASSERT_TRUE(ClassifyGitOp(kGitENotFound, "") == GitOpError::Unknown);
  std::cout << "  PASS test_unknown_default" << std::endl;
  return 0;
}

static int test_error_message_strings() {
  // GitOpErrorMessage must return a non-null, non-empty string for every
  // enum value. The strings are tooltip-only -- contents are not asserted.
  for (auto e : {GitOpError::None, GitOpError::NetworkUnavailable,
                 GitOpError::AuthFailure, GitOpError::MergeConflict,
                 GitOpError::NonFastForward, GitOpError::RepoCorrupt,
                 GitOpError::RemoteDiverged, GitOpError::Cancelled,
                 GitOpError::Unknown}) {
    const char *msg = GitOpErrorMessage(e);
    ASSERT_NOT_NULL(msg);
    ASSERT_TRUE(std::string(msg).size() > 0);
  }
  std::cout << "  PASS test_error_message_strings" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  // libgit2 must be initialised before any git_error_last() call inside
  // ClassifyGitOp; init is reference-counted so a single init+shutdown pair
  // is sufficient for the whole test process.
  git_libgit2_init();

  std::cout << "Running GitOpError classifier tests..." << std::endl;

  RUN_TEST(test_none_on_zero_rc);
  RUN_TEST(test_auth_failure_rc);
  RUN_TEST(test_network_unavailable_cert);
  RUN_TEST(test_non_fast_forward_rc);
  RUN_TEST(test_merge_conflict_rc);
  RUN_TEST(test_repo_corrupt_rc);
  RUN_TEST(test_non_fast_forward_message);
  RUN_TEST(test_cancelled_on_giteuser);
  RUN_TEST(test_unknown_default);
  RUN_TEST(test_error_message_strings);

  git_libgit2_shutdown();

  std::cout << "All GitOpError classifier tests passed." << std::endl;
  return 0;
}
