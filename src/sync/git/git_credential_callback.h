#ifndef VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H
#define VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H

#include <git2.h>

#include <string>

namespace vxcore {

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

// Helper that returns a git_remote_callbacks struct initialized to defaults
// and wired with our credential callback + the given payload. Use at every
// libgit2 fetch/push/ls-remote site to keep the wiring consistent.
git_remote_callbacks MakeRemoteCallbacks(GitCredentialPayload *payload);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_CREDENTIAL_CALLBACK_H
