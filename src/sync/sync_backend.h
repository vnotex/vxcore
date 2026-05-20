#ifndef VXCORE_SYNC_BACKEND_H
#define VXCORE_SYNC_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

#include "sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// Capability bits advertised by an ISyncBackend implementation. The set of
// bits returned by GetCapabilities() lets callers query optional features
// without subclass-specific knowledge (e.g. progress UIs, cancellation glue,
// auth prompts). Capabilities are exposed at the C++ level only — there is
// no `vxcore_sync_get_capabilities` C ABI in v1.
//
// Task 4.1 of sync-backend-phase4 introduces these initial bits. New bits
// MUST be appended (do NOT renumber existing bits — third-party backends
// may already encode them in persisted configuration in the future).
enum class SyncCapability : uint32_t {
  None = 0,
  ConflictDetection  = 1u << 0,
  ConflictResolution = 1u << 1,
  IncrementalSync    = 1u << 2,
  AuthRequired       = 1u << 3,
  Cancellation       = 1u << 4,
  ProgressReporting  = 1u << 5,
};

// Bitwise-OR of SyncCapability values. Kept as a plain uint32_t alias rather
// than a strong type so callers can use straightforward bit math without
// casting through enum class operators.
using SyncCapabilities = uint32_t;

class ISyncBackend {
 public:
  virtual ~ISyncBackend() = default;

  // Identification — returns the registered backend name (e.g. "git",
  // "webdav"). MUST match the value used in SyncConfig::backend so the
  // upcoming SyncBackendRegistry (Task 4.2) can match instances to factory
  // keys. Implementations MUST return a stable, lower-case ASCII string.
  virtual std::string GetName() const = 0;

  // Feature flags — OR'd bitmask of SyncCapability values. Returned value
  // MAY change between calls if a backend re-evaluates capabilities after
  // Initialize (e.g. detecting that a remote supports server-side
  // cancellation), but implementations SHOULD prefer a static value.
  virtual SyncCapabilities GetCapabilities() const = 0;

  // True iff the backend has completed a successful Initialize() and is
  // ready to handle Sync. Becomes false on destruction or after a failed
  // Initialize(). Used by callers (and by SyncManager-level diagnostics)
  // to distinguish a registered-but-unconfigured backend from one that's
  // actually ready to talk to a remote.
  virtual bool IsInitialized() const = 0;

  virtual VxCoreError Initialize(const std::string &root_folder,
                                 const SyncConfig &config) = 0;

  // Called by SyncManager BEFORE Initialize/Sync when credentials are
  // available. Backend caches them. Idempotent. Default no-op for backends that
  // don't need credentials (e.g., MockSyncBackend).
  virtual VxCoreError SetCredentials(const SyncCredentials &creds) {
    (void)creds;
    return VXCORE_OK;
  }

  // Sync is the composite remote-roundtrip operation (stage/commit/fetch/
  // rebase/push). Wave 4.5 (F1.3 of sync-backend-phase4) removed the legacy
  // standalone Push/Pull/Shutdown pure virtuals — implementations now expose
  // only this composite entry point. Teardown is handled by the destructor.
  virtual VxCoreError Sync(SyncProgressCallback callback, void *userdata) = 0;

  virtual VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) = 0;

  virtual VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) = 0;

  virtual VxCoreError ResolveConflict(const std::string &path,
                                      SyncConflictResolution resolution) = 0;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_BACKEND_H
