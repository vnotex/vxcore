#ifndef VXCORE_SYNC_GIT_GIT_ERROR_TRANSLATOR_H
#define VXCORE_SYNC_GIT_GIT_ERROR_TRANSLATOR_H

#include <string>

#include "vxcore/vxcore_types.h"

namespace vxcore {

// T28: translate libgit2 return codes / error classes to VxCoreError. Always
// reads git_error_last() (and logs it) so the original libgit2 message is
// preserved in the log even when we collapse many klass values into a single
// VxCoreError. libgit2 v1.7.x has no GIT_ERROR_TCP / GIT_ERROR_NOTFOUND klass
// constants; we rely on the return-code constants (GIT_ENOTFOUND,
// GIT_EINVALIDSPEC) for those cases instead.
VxCoreError TranslateGitError(int git_rc);

// W11.2 / Task 11.2 (B3): structured classification of a libgit2 operation
// outcome. Distinct from the C-ABI VxCoreError mapping above -- this enum is
// vxcore-C++-internal and is the ONLY structured way callers should reason
// about libgit2 failures. Replaces the ad-hoc English-substring matching that
// used to live in GitSyncBackend::Sync (non-fast-forward detection) and inside
// TranslateGitError (HTTP-auth message sniffing).
enum class GitOpError {
  None,                // libgit2 rc == 0 (success)
  NetworkUnavailable,  // transient network / transport (timeout, DNS, TCP)
  AuthFailure,         // GIT_EAUTH or HTTP 401/403/credentials
  MergeConflict,       // GIT_EMERGECONFLICT / GIT_ECONFLICT / GIT_ERROR_MERGE / GIT_ERROR_CHECKOUT
  NonFastForward,      // push rejected because the ref would not advance fast-forward
  RepoCorrupt,         // GIT_EUNCOMMITTED / GIT_ENOTAREPO / broken index or HEAD
  RemoteDiverged,      // fetch reveals divergence before a push attempt (distinct from NonFastForward)
  Cancelled,           // GIT_EUSER (-7): user-cancelled via SyncCancellation token (W12.1)
  Unknown              // anything not matched above -- the safe default
};

// Translate a libgit2 return code + the captured `git_error_last()->message`
// (UTF-8) to a structured GitOpError. The message string is passed in by the
// caller (rather than re-read globally) so the caller controls capture
// lifetime: libgit2 wipes thread-local error state on the next call.
//
// Pass an empty string when no message is available; classification still
// works for rc-driven cases (e.g. GIT_EAUTH -> AuthFailure).
//
// IMPORTANT: This function is the ONLY place in the codebase allowed to
// inspect English libgit2 error-message text. All callers MUST switch on
// GitOpError values; they MUST NOT do their own substring matching against
// libgit2 messages.
GitOpError ClassifyGitOp(int libgit2_rc, const std::string &last_error_message);

// Human-readable English description, for logs / tooltips ONLY. NEVER use the
// return value for control-flow matching.
const char *GitOpErrorMessage(GitOpError err);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_ERROR_TRANSLATOR_H
