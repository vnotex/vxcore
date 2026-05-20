#include "sync/git/git_credential_callback.h"

#include "sync/credential_provider.h"
#include "sync/sync_types.h"

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

RemoteCallbacksBundle MakeRemoteCallbacks(ICredentialProvider *provider,
                                          const std::string &remote_url) {
  RemoteCallbacksBundle bundle;
  // Wave 6.3 (F4.4): snapshot creds from the provider at callback-build time.
  // The libgit2 thread never touches the provider — it only reads the
  // stack-local payload built here on the caller's thread. If the provider
  // is null or declines to supply creds, the payload PAT stays empty and
  // the callback returns GIT_PASSTHROUGH for anonymous transport.
  if (provider != nullptr) {
    SyncCredentials snapshot;
    if (provider->GetCredentials(remote_url, /*username_from_url=*/"x-access-token",
                                 &snapshot)) {
      bundle.payload.personal_access_token = snapshot.personal_access_token;
    }
  }
  bundle.callbacks = GIT_REMOTE_CALLBACKS_INIT;
  bundle.callbacks.payload = &bundle.payload;
  bundle.callbacks.credentials = &GitSyncBackendCredentialCb;
  return bundle;
}

}  // namespace vxcore
