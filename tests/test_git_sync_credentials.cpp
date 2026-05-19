// T27 + T28 unit tests: credential callback selection and libgit2 error
// translation. Compiles git_sync_backend.cpp + libgit2_init.cpp directly
// (bypassing vxcore.dll, mirroring the test_libgit2_init pattern) so the
// translator and callback symbols are linked statically into this binary
// without DLL-export contortions and so the libgit2 ref-count statics are
// owned exclusively by this binary.

#include <git2.h>

#include <iostream>
#include <string>

#include "sync/git/git_sync_backend.h"
#include "sync/git/git_credential_callback.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_types.h"
#include "test_utils.h"

namespace {

int test_git_credential_pat_used_when_set() {
  vxcore::LibGit2Init init;
  vxcore::GitSyncBackend backend;
  vxcore::SyncCredentials creds;
  creds.personal_access_token = "secret_pat_xyz";
  ASSERT_EQ(backend.SetCredentials(creds), VXCORE_OK);

  vxcore::GitCredentialPayload payload{creds.personal_access_token};
  git_credential *cred = nullptr;
  int rc = vxcore::GitSyncBackendCredentialCb(
      &cred, "https://github.com/example/repo.git", nullptr,
      GIT_CREDENTIAL_USERPASS_PLAINTEXT, &payload);
  ASSERT_EQ(rc, 0);
  ASSERT_NOT_NULL(cred);
  git_credential_free(cred);

  std::cout << "  ✓ test_git_credential_pat_used_when_set passed" << std::endl;
  return 0;
}

int test_git_credential_anonymous_returns_passthrough() {
  vxcore::LibGit2Init init;
  vxcore::GitSyncBackend backend;
  // No credentials set -> personal_access_token is empty.

  vxcore::GitCredentialPayload empty_payload{};
  git_credential *cred = nullptr;
  int rc = vxcore::GitSyncBackendCredentialCb(
      &cred, "https://github.com/example/repo.git", nullptr,
      GIT_CREDENTIAL_USERPASS_PLAINTEXT, &empty_payload);
  ASSERT_EQ(rc, GIT_PASSTHROUGH);
  ASSERT_NULL(cred);

  // Also: PAT set but allowed_types does NOT include USERPASS_PLAINTEXT ->
  // still passthrough (no fallback to other auth types in v1).
  vxcore::SyncCredentials creds;
  creds.personal_access_token = "tok";
  ASSERT_EQ(backend.SetCredentials(creds), VXCORE_OK);
  vxcore::GitCredentialPayload payload{creds.personal_access_token};
  rc = vxcore::GitSyncBackendCredentialCb(
      &cred, "https://github.com/example/repo.git", nullptr,
      /*allowed_types=*/0u, &payload);
  ASSERT_EQ(rc, GIT_PASSTHROUGH);
  ASSERT_NULL(cred);

  // And: null payload -> passthrough (defensive).
  rc = vxcore::GitSyncBackendCredentialCb(
      &cred, "https://github.com/example/repo.git", nullptr,
      GIT_CREDENTIAL_USERPASS_PLAINTEXT, nullptr);
  ASSERT_EQ(rc, GIT_PASSTHROUGH);
  ASSERT_NULL(cred);

  std::cout << "  ✓ test_git_credential_anonymous_returns_passthrough passed" << std::endl;
  return 0;
}

int test_git_error_translate_notfound() {
  vxcore::LibGit2Init init;

  // Drive libgit2 to produce a real ENOTFOUND from git_repository_open on a
  // path that cannot possibly be a repo.
  git_repository *repo = nullptr;
  int rc = git_repository_open(&repo, "definitely_does_not_exist_path_xyz_12345");
  ASSERT(rc < 0);
  if (repo != nullptr) {
    git_repository_free(repo);
  }

  VxCoreError mapped = vxcore::GitSyncBackend::TranslateGitErrorForTesting(rc);
  // git_repository_open returns GIT_ENOTFOUND for missing paths.
  ASSERT_EQ(mapped, VXCORE_ERR_NOT_FOUND);

  // Also: pure GIT_ENOTFOUND -> NOT_FOUND regardless of err state.
  git_error_clear();
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(GIT_ENOTFOUND),
            VXCORE_ERR_NOT_FOUND);

  std::cout << "  ✓ test_git_error_translate_notfound passed" << std::endl;
  return 0;
}

int test_git_error_translate_network() {
  vxcore::LibGit2Init init;

  // Synthesize a NET-class error and verify the translator reads klass via
  // git_error_last() and maps to SYNC_NETWORK.
  git_error_set_str(GIT_ERROR_NET, "fake network failure for test");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(-1),
            VXCORE_ERR_SYNC_NETWORK);

  // SSL/SSH klasses also map to NETWORK.
  git_error_set_str(GIT_ERROR_SSL, "fake ssl failure");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(-1),
            VXCORE_ERR_SYNC_NETWORK);

  // GIT_EAUTH always maps to AUTH_FAILED regardless of klass message.
  git_error_set_str(GIT_ERROR_HTTP, "anything");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(GIT_EAUTH),
            VXCORE_ERR_SYNC_AUTH_FAILED);

  // HTTP klass with auth-keyword in message -> AUTH_FAILED.
  git_error_set_str(GIT_ERROR_HTTP, "401 Unauthorized");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(-1),
            VXCORE_ERR_SYNC_AUTH_FAILED);

  // HTTP klass without auth-keyword -> NETWORK.
  git_error_set_str(GIT_ERROR_HTTP, "503 Service Unavailable");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(-1),
            VXCORE_ERR_SYNC_NETWORK);

  // MERGE/CHECKOUT klasses -> SYNC_CONFLICT.
  git_error_set_str(GIT_ERROR_MERGE, "merge conflict");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(-1),
            VXCORE_ERR_SYNC_CONFLICT);

  // INVALID klass / GIT_EINVALIDSPEC -> INVALID_PARAM.
  git_error_set_str(GIT_ERROR_INVALID, "bad input");
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(-1),
            VXCORE_ERR_INVALID_PARAM);
  git_error_clear();
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(GIT_EINVALIDSPEC),
            VXCORE_ERR_INVALID_PARAM);

  // git_rc >= 0 -> OK (translator short-circuits).
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(0), VXCORE_OK);
  ASSERT_EQ(vxcore::GitSyncBackend::TranslateGitErrorForTesting(1), VXCORE_OK);

  std::cout << "  ✓ test_git_error_translate_network passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_git_credential_pat_used_when_set);
  RUN_TEST(test_git_credential_anonymous_returns_passthrough);
  RUN_TEST(test_git_error_translate_notfound);
  RUN_TEST(test_git_error_translate_network);
  std::cout << "All git sync credentials/error tests passed" << std::endl;
  return 0;
}
