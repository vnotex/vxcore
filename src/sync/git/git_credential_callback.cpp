#include "sync/git/git_credential_callback.h"

#include <cstddef>

#include "sync/credential_provider.h"
#include "sync/sync_cancellation.h"
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
                                          const std::string &remote_url,
                                          SyncCancellation *cancellation) {
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
  // W12.1: optional cancellation token rides in the same payload as the
  // credential snapshot (libgit2 dispatches one payload pointer to all
  // callbacks in a git_remote_callbacks struct).
  bundle.payload.cancellation = cancellation;
  bundle.callbacks = GIT_REMOTE_CALLBACKS_INIT;
  bundle.callbacks.payload = &bundle.payload;
  bundle.callbacks.credentials = &GitSyncBackendCredentialCb;
  // Cancellation shims — install ONLY when a token is actually wired.
  // libgit2's `git_remote_fetch` can change its internal codepath when
  // `transfer_progress` is non-null even for trivial local file:// remotes
  // (observed: the checkout step skipped after a successful fetch when an
  // always-return-0 shim was installed unconditionally). Keeping the field
  // null in the no-token case preserves pre-W12 behavior bit-for-bit.
  if (cancellation != nullptr) {
    bundle.callbacks.transfer_progress =
        [](const git_indexer_progress *stats, void *payload) -> int {
          auto *pl = static_cast<GitCredentialPayload *>(payload);
          if (pl == nullptr) return 0;
          return SyncCancellation::Libgit2ProgressCheck(stats, pl->cancellation);
        };
    bundle.callbacks.push_transfer_progress =
        [](unsigned int current, unsigned int total, std::size_t bytes,
           void *payload) -> int {
          auto *pl = static_cast<GitCredentialPayload *>(payload);
          if (pl == nullptr) return 0;
          return SyncCancellation::Libgit2PushProgressCheck(current, total, bytes,
                                                            pl->cancellation);
        };
  }
  return bundle;
}

}  // namespace vxcore
