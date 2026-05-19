#include "sync/git/git_error_translator.h"

#include <cstring>

#include <git2.h>

#include "utils/logger.h"

namespace vxcore {

VxCoreError TranslateGitError(int git_rc) {
  if (git_rc >= 0) {
    return VXCORE_OK;
  }

  const git_error *err = git_error_last();
  const char *message = (err && err->message) ? err->message : "(no message)";
  int klass = err ? err->klass : 0;
  VXCORE_LOG_ERROR("libgit2 error rc=%d klass=%d: %s", git_rc, klass, message);

  if (git_rc == GIT_EAUTH) {
    return VXCORE_ERR_SYNC_AUTH_FAILED;
  }

  if (klass == GIT_ERROR_HTTP) {
    if (message != nullptr &&
        (std::strstr(message, "401") != nullptr ||
         std::strstr(message, "authenticat") != nullptr ||
         std::strstr(message, "credentials") != nullptr)) {
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

}  // namespace vxcore
