#ifndef VXCORE_SYNC_PROVIDER_RETIREMENT_H
#define VXCORE_SYNC_PROVIDER_RETIREMENT_H

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

#include "vxcore/vxcore_types.h"

namespace vxcore {

struct SyncProviderRuntimeFacts {
  bool runtime_registered = false;
  bool apply_in_progress = false;
  bool persisted_enabled = false;
  // True only when enabled is false AND both backend and URL have been cleared.
  bool persisted_routing_cleared = false;
};

// Read-only, network-free and context-free. A cleanCloneBaseline fact is diagnostic,
// never permission to import a cursor. Unsupported provider storage is not migrated.
// probe reports unresolved recovery, not a dormant empty legacy probe binding.
// Any probe namespace still disqualifies a clean clone baseline. Legacy unsealed
// retirement records are readable facts only, never restore/commit authority.
// hasState and binding/hash fields describe the live state, not retained archives
// or unpublished candidates. An empty candidate parent has no active recovery,
// may archive intact, and still disqualifies a clean clone baseline.
VxCoreError InspectSyncStorage(const std::string &notebook_root,
                             const std::string &expected_notebook_id,
                             nlohmann::json &out_facts);

// The manager supplies the selected bundled root and runtime/persisted facts under
// its owner lease. Commands contain no paths. Archives are NEVER deleted.
VxCoreError SyncProviderTransaction(const std::string &notebook_root,
                                   const std::string &expected_notebook_id,
                                   const nlohmann::json &command,
                                   const SyncProviderRuntimeFacts &runtime,
                                   nlohmann::json &out_result);

// Shared schema inspection for the legacy encryption/reconfiguration guards.
// This helper has no mutation or cleanup authority.
struct WebDavStorageStatus {
  bool encryption_conflict = false;
  bool encryption_pending = false;
  bool replaces_encryption = false;
  bool any_conflict = false;
  bool any_pending = false;
};
VxCoreError InspectWebDavStorage(const std::filesystem::path &directory,
                               WebDavStorageStatus &out_status);

}  // namespace vxcore

#endif  // VXCORE_SYNC_PROVIDER_RETIREMENT_H
