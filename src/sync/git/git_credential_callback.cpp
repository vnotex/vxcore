#include "sync/git/git_credential_callback.h"

namespace vxcore {

int GitSyncBackendCredentialCb(git_credential **out, const char *url,
                                const char *username_from_url,
                                unsigned int allowed_types, void *payload) {
  (void)url;
  (void)username_from_url;
  auto *pl = static_cast<GitCredentialPayload *>(payload);
  if (pl == nullptr) {
    return GIT_PASSTHROUGH;
  }
  if (!pl->personal_access_token.empty() &&
      (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0) {
    return git_credential_userpass_plaintext_new(
        out, "x-access-token", pl->personal_access_token.c_str());
  }
  return GIT_PASSTHROUGH;
}

git_remote_callbacks MakeRemoteCallbacks(GitCredentialPayload *payload) {
  git_remote_callbacks cb = GIT_REMOTE_CALLBACKS_INIT;
  cb.payload = payload;
  cb.credentials = &GitSyncBackendCredentialCb;
  return cb;
}

}  // namespace vxcore
