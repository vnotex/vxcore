#include "sync/git/git_error_translator.h"

#include <cstring>

#include <git2.h>

#include "utils/logger.h"

namespace vxcore {

namespace {

// Local helper: case-sensitive substring search on a std::string view.
// Returns true if `needle` appears anywhere in `haystack`.
bool HasSubstr(const std::string &haystack, const char *needle) {
  if (haystack.empty() || needle == nullptr || *needle == '\0') return false;
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

VxCoreError TranslateGitError(int git_rc) {
  if (git_rc >= 0) {
    return VXCORE_OK;
  }

  const git_error *err = git_error_last();
  const char *message = (err && err->message) ? err->message : "(no message)";
  int klass = err ? err->klass : 0;
  VXCORE_LOG_ERROR("libgit2 error rc=%d klass=%d: %s", git_rc, klass, message);

  // W11.2: route HTTP/auth substring detection through ClassifyGitOp so the
  // English-text matching lives in a single, audited function. Mapping back
  // to the C-ABI VxCoreError happens here so the public ABI is unchanged.
  if (git_rc == GIT_EAUTH) {
    return VXCORE_ERR_SYNC_AUTH_FAILED;
  }

  if (klass == GIT_ERROR_HTTP) {
    const std::string msg = (err && err->message) ? err->message : "";
    if (ClassifyGitOp(git_rc, msg) == GitOpError::AuthFailure) {
      return VXCORE_ERR_SYNC_AUTH_FAILED;
    }
    return VXCORE_ERR_SYNC_NETWORK;
  }

  if (klass == GIT_ERROR_NET || klass == GIT_ERROR_SSL || klass == GIT_ERROR_SSH) {
    return VXCORE_ERR_SYNC_NETWORK;
  }

  if (klass == GIT_ERROR_MERGE || klass == GIT_ERROR_CHECKOUT) {
    return VXCORE_ERR_SYNC_CONFLICT;
  }

  if (git_rc == GIT_ENOTFOUND) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (klass == GIT_ERROR_INVALID || git_rc == GIT_EINVALIDSPEC) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  return VXCORE_ERR_UNKNOWN;
}

// =============================================================================
// W11.2 / B3: ClassifyGitOp -- the ONLY function allowed to inspect English
// libgit2 error-message text. Every other caller in the codebase MUST switch
// on the returned GitOpError; they MUST NOT roll their own substring search
// against libgit2 messages. This isolates the cross-locale fragility of
// message matching to one auditable location.
//
// Inputs:
//   - libgit2_rc: the int returned by a libgit2 call (>= 0 means success).
//   - last_error_message: UTF-8 contents of git_error_last()->message,
//     captured by the caller before any subsequent libgit2 call wipes it.
//     Pass "" when unavailable; rc-only classification still works.
//
// Output: a single GitOpError tag. `Unknown` is the safe default; callers
// should treat it as "non-retryable, non-actionable beyond logging".
// =============================================================================
GitOpError ClassifyGitOp(int libgit2_rc, const std::string &last_error_message) {
  if (libgit2_rc == 0) {
    return GitOpError::None;
  }

  // RC-driven classification first (locale-independent, ABI-stable).
  switch (libgit2_rc) {
    case GIT_EUSER:
      // W12.1: libgit2 surfaces a user-aborted op (nonzero from a progress /
      // credential callback) as GIT_EUSER (-7). This is NEVER a retryable
      // failure -- the user explicitly asked to stop.
      return GitOpError::Cancelled;
    case GIT_EAUTH:
      return GitOpError::AuthFailure;
    case GIT_ECERTIFICATE:
      return GitOpError::NetworkUnavailable;
    case GIT_ENONFASTFORWARD:
      return GitOpError::NonFastForward;
    case GIT_EUNMERGED:
    case GIT_EMERGECONFLICT:
    case GIT_ECONFLICT:
      return GitOpError::MergeConflict;
    case GIT_EUNCOMMITTED:
    case GIT_EBAREREPO:
      return GitOpError::RepoCorrupt;
    default:
      break;
  }

  // Klass-driven classification (libgit2's structured error categories).
  const git_error *err = git_error_last();
  const int klass = err ? err->klass : 0;
  if (klass == GIT_ERROR_NET || klass == GIT_ERROR_SSL || klass == GIT_ERROR_SSH) {
    return GitOpError::NetworkUnavailable;
  }
  if (klass == GIT_ERROR_MERGE || klass == GIT_ERROR_CHECKOUT) {
    return GitOpError::MergeConflict;
  }
  if (klass == GIT_ERROR_INDEX) {
    return GitOpError::RepoCorrupt;
  }

  // Message-driven last resort. libgit2 surfaces non-fast-forward push
  // rejections and HTTP 401/403 as generic GIT_ERROR (-1) with klass
  // GIT_ERROR_REFERENCE / GIT_ERROR_HTTP and the meaningful signal is only
  // in the message text. This block is the single place in the codebase that
  // is allowed to do that inspection.
  if (HasSubstr(last_error_message, "non-fast-forward") ||
      HasSubstr(last_error_message, "non fast forward") ||
      HasSubstr(last_error_message, "fast-forward") ||
      HasSubstr(last_error_message, "rejected")) {
    return GitOpError::NonFastForward;
  }

  if (klass == GIT_ERROR_HTTP) {
    if (HasSubstr(last_error_message, "401") ||
        HasSubstr(last_error_message, "403") ||
        HasSubstr(last_error_message, "authenticat") ||
        HasSubstr(last_error_message, "credentials")) {
      return GitOpError::AuthFailure;
    }
    return GitOpError::NetworkUnavailable;
  }

  return GitOpError::Unknown;
}

const char *GitOpErrorMessage(GitOpError err) {
  switch (err) {
    case GitOpError::None:               return "Success";
    case GitOpError::NetworkUnavailable: return "Network transport error (unreachable / timeout / TLS)";
    case GitOpError::AuthFailure:        return "Authentication failed (bad/missing credentials)";
    case GitOpError::MergeConflict:      return "Merge conflict in working tree or index";
    case GitOpError::NonFastForward:     return "Push rejected: branch is not fast-forward";
    case GitOpError::RepoCorrupt:        return "Local repository is corrupt or unusable";
    case GitOpError::RemoteDiverged:     return "Remote branch has diverged from local";
    case GitOpError::Cancelled:          return "Operation cancelled";
    case GitOpError::Unknown:            return "Unknown libgit2 error";
  }
  return "Unknown libgit2 error";
}

}  // namespace vxcore
