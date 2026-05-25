// Explicit registration of built-in sync backends. Replaces the previous
// static-initializer self-registration scheme (BackendRegistration token in
// git_sync_backend.cpp + EnsureGitBackendLinked anchor in libgit2_init.cpp)
// that was unreliable under MSVC /OPT:REF when shipping inside vnote.exe.
//
// vxcore_context_create() calls RegisterBuiltinBackends(...) once at startup;
// subsequent calls are silent no-ops (SyncBackendRegistry::Register uses
// first-wins semantics).

#include <memory>
#include <utility>

#include "sync/git/git_sync_backend.h"
#include "sync/sync_backend.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_types.h"

namespace vxcore {

namespace {

std::unique_ptr<ISyncBackend> MakeGitSyncBackend(
    const SyncConfig &cfg, std::shared_ptr<ICredentialProvider> provider) {
  return std::make_unique<GitSyncBackend>(cfg, std::move(provider));
}

}  // namespace

void RegisterBuiltinBackends(SyncBackendRegistry &registry) {
  registry.Register("git", &MakeGitSyncBackend);
}

}  // namespace vxcore
