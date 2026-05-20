#ifndef VXCORE_SYNC_BACKEND_REGISTRY_H
#define VXCORE_SYNC_BACKEND_REGISTRY_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "credential_provider.h"
#include "sync_backend.h"
#include "sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// Factory signature for constructing an ISyncBackend instance from a
// SyncConfig + ICredentialProvider. Returning nullptr indicates "this
// factory declined to build a backend" and is treated identically to
// "unknown name" by Create().
//
// Task 6.2 (F4.4) of sync-backend-phase4: the provider parameter is passed
// to the backend constructor. Backends that declare SyncCapability::AuthRequired
// MUST receive a non-null provider OR a non-null credentials snapshot via
// SyncManager::EnableSync; SyncManager enforces this after factory invocation
// but before registering the backend in states_.
using SyncBackendFactory = std::function<std::unique_ptr<ISyncBackend>(
    const SyncConfig &, std::shared_ptr<ICredentialProvider>)>;

// Process-wide registry mapping backend names (e.g. "git") to factory
// callables. Populated at static-init time via BackendRegistration tokens
// (see git_sync_backend.cpp for the canonical example).
//
// Thread safety: all public methods acquire an internal mutex. Factory
// callables are invoked WITHOUT the mutex held to respect the "no callback
// under lock" rule documented in libs/vxcore/src/sync/AGENTS.md
// (Threading & Callback Contract).
//
// Duplicate-name semantics: first-wins. A second Register() call for an
// already-registered name returns false and leaves the existing factory
// untouched. This makes static-init order across translation units a
// non-issue — whichever TU initializes first wins, and the other TU's
// BackendRegistration token silently no-ops.
class SyncBackendRegistry {
 public:
  // Meyer's singleton — thread-safe per the C++11 standard guarantee for
  // local static initialization. No std::call_once needed.
  VXCORE_API static SyncBackendRegistry &Instance();

  // Returns true on successful insert. Returns false (no-op) if:
  //   * name is empty, OR
  //   * factory is null (operator bool() == false), OR
  //   * name is already registered (first-wins).
  VXCORE_API bool Register(const std::string &name, SyncBackendFactory factory);

  // Returns a fresh backend instance from the registered factory for name,
  // or nullptr if the name is not registered (or the factory returns null).
  // The factory is invoked with mu_ released (callback-under-lock rule).
  // The provider is forwarded to the factory; callers SHOULD pass non-null
  // for backends with SyncCapability::AuthRequired.
  VXCORE_API std::unique_ptr<ISyncBackend> Create(
      const std::string &name, const SyncConfig &cfg,
      std::shared_ptr<ICredentialProvider> provider) const;

  // Returns the registered names sorted lexicographically. Sort order is
  // guaranteed for deterministic test assertions.
  VXCORE_API std::vector<std::string> Names() const;

 private:
  SyncBackendRegistry() = default;
  ~SyncBackendRegistry() = default;
  SyncBackendRegistry(const SyncBackendRegistry &) = delete;
  SyncBackendRegistry &operator=(const SyncBackendRegistry &) = delete;

  mutable std::mutex mu_;
  std::unordered_map<std::string, SyncBackendFactory> factories_;
};

// RAII token for static-init registration. Construct one at namespace scope
// in an anonymous namespace inside a backend's .cpp file; the constructor
// calls SyncBackendRegistry::Instance().Register(name, std::move(factory)).
//
// Never throws — exceptions during construction are swallowed so static-init
// time cannot crash the program. If the underlying Register() returns false
// (duplicate name, empty name, null factory), the BackendRegistration is a
// silent no-op.
struct BackendRegistration {
  VXCORE_API BackendRegistration(const std::string &name,
                                 SyncBackendFactory factory);
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_BACKEND_REGISTRY_H
