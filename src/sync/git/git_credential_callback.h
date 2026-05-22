#ifndef VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H
#define VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H

#include <git2.h>

#include <string>

#include "sync/sync_cancellation.h"

namespace vxcore {

// Forward declarations
struct SyncCredentials;
class ICredentialProvider;

// Payload carried into libgit2 credential callbacks. Holds only the data
// the callback actually reads (PAT). Per-call snapshot — lifetime must
// span the entire libgit2 operation (fetch/push/ls-remote).
//
// Wave 6.3 (F4.4): the payload is built ONCE per libgit2 operation by
// MakeRemoteCallbacks, which calls ICredentialProvider::GetCredentials at
// callback-build time. The libgit2 thread reads the snapshot without ever
// touching the provider or any vxcore mutex.
//
// W12.1: the same payload now also carries a non-owning pointer to an
// optional SyncCancellation token (null when the pipeline has no
// cancellation wired). The transfer_progress and push_transfer_progress
// callbacks read this field on every progress tick to honour user-driven
// cancel requests. libgit2 routes a single `payload` pointer to ALL
// callbacks in a `git_remote_callbacks` struct, so credentials +
// cancellation share the same payload instance.
//
// Lifetime contract: callers MUST declare a GitCredentialPayload (typically
// via the RemoteCallbacksBundle returned by MakeRemoteCallbacks) in the
// enclosing scope before the libgit2 call and ensure it lives until that
// call returns. Passing a pointer to a temporary is UB.
struct GitCredentialPayload {
  std::string personal_access_token;
  // Non-owning. Set to nullptr (default) when no cancellation token is
  // wired -- the progress callbacks then treat it as "never cancelled".
  SyncCancellation *cancellation = nullptr;
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

// Helper that constructs a per-call GitCredentialPayload snapshot from a
// credential provider (Wave 6.3 F4.4 of sync-backend-phase4) and returns a
// bundle of {git_remote_callbacks, payload}. Caller must keep the returned
// bundle alive for the duration of the libgit2 operation (fetch/push/
// ls-remote).
//
// When |provider| is null OR provider->GetCredentials returns false, the
// payload's PAT is left empty — the callback will then return GIT_PASSTHROUGH
// and libgit2 falls through to anonymous transport (correct for public
// HTTPS remotes).
//
// Snapshot semantics: the provider is queried EXACTLY ONCE per libgit2
// operation, even if libgit2 re-invokes the credential callback multiple
// times (e.g. after a 401 retry). This matches F2.6's stack-local snapshot
// contract — each call gets a fresh copy, not a shared reference. Calling
// the provider on the libgit2 thread is explicitly avoided.
struct RemoteCallbacksBundle {
  git_remote_callbacks callbacks;
  GitCredentialPayload payload;
};

RemoteCallbacksBundle MakeRemoteCallbacks(ICredentialProvider *provider,
                                          const std::string &remote_url,
                                          SyncCancellation *cancellation = nullptr);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H
