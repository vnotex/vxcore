#ifndef VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H
#define VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H

#include <git2.h>

#include <string>

namespace vxcore {

// Forward declaration
struct SyncCredentials;

// Payload carried into libgit2 credential callbacks. Holds only the data
// the callback actually reads (PAT). Per-call snapshot — lifetime must
// span the entire libgit2 operation (fetch/push/ls-remote).
//
// Lifetime contract: callers MUST declare a GitCredentialPayload in the
// enclosing scope before the libgit2 call and ensure it lives until that
// call returns. Passing a pointer to a temporary is UB.
struct GitCredentialPayload {
  std::string personal_access_token;
};

// libgit2 credential callback (function-pointer signature). Reads PAT from
// |payload| (cast to GitCredentialPayload*). Returns GIT_PASSTHROUGH when:
//   - payload is null
//   - PAT is empty
//   - GIT_CREDENTIAL_USERPASS_PLAINTEXT is not in allowed_types
// Otherwise creates a userpass credential with username "x-access-token"
// (GitHub/GitLab/Gitea convention) and the PAT as the password.
int GitSyncBackendCredentialCb(git_credential **out, const char *url,
                                const char *username_from_url,
                                unsigned int allowed_types, void *payload);

// Helper that constructs a per-call GitCredentialPayload snapshot from
// credentials and returns a bundle of {git_remote_callbacks, payload}.
// Caller must keep the returned bundle alive for the duration of the libgit2
// operation (fetch/push/ls-remote). This preserves F2.6 stack-local snapshot
// semantics: each call gets a fresh copy, not a shared reference.
struct RemoteCallbacksBundle {
  git_remote_callbacks callbacks;
  GitCredentialPayload payload;
};

RemoteCallbacksBundle MakeRemoteCallbacks(const SyncCredentials &credentials);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H
