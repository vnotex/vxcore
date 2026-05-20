#ifndef VXCORE_TESTS_MOCK_SYNC_BACKEND_H
#define VXCORE_TESTS_MOCK_SYNC_BACKEND_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sync/sync_backend.h"
#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ICredentialProvider;

// Test-only in-memory backend used by test_sync_manager_dispatch.
// All methods record their invocations and return the configured canned result.
class MockSyncBackend : public ISyncBackend {
 public:
  // Configurable canned return values (default VXCORE_OK).
  VxCoreError next_initialize_result = VXCORE_OK;
  VxCoreError next_sync_result = VXCORE_OK;
  VxCoreError next_resolve_conflict_result = VXCORE_OK;
  std::vector<SyncFileInfo> canned_status;
  std::vector<SyncConflictInfo> canned_conflicts;

  // Call counters.
  int initialize_call_count = 0;
  int sync_call_count = 0;
  int get_status_call_count = 0;
  int get_conflicts_call_count = 0;
  int resolve_conflict_call_count = 0;
  // Wave 6.3 F4.4: replaces set_credentials_call_count. Tracks how many
  // times SyncManager::UpdateCredentials rotated the provider.
  int replace_creds_provider_call_count = 0;

  // Captured parameters.
  std::string last_resolve_path;
  SyncConflictResolution last_resolve_resolution = SyncConflictResolution::kKeepBoth;

  // Wave 6.3 F4.4: snapshot of replace_creds_provider_call_count taken inside
  // Initialize(). Proves rotation ordering relative to Initialize when needed.
  int replace_creds_provider_call_count_at_initialize = 0;

  VxCoreError Initialize(const std::string &root_folder, const SyncConfig &config) override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

  // Wave 6.3 F4.4: provider-rotation recorder.
  void ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) override;
  std::shared_ptr<ICredentialProvider> GetCredsProviderSnapshot() const override;

  // Task 4.1 (sync-backend-phase4 F1.2): backend identity. Hard-coded for
  // this legacy in-tests-dir mock; the test_internals/mock_sync_backend.h
  // copy is the configurable one used by the metadata test.
  std::string GetName() const override { return "mock"; }
  SyncCapabilities GetCapabilities() const override {
    return static_cast<SyncCapabilities>(SyncCapability::None);
  }
  bool IsInitialized() const override { return initialize_call_count > 0; }

 private:
  mutable std::mutex creds_provider_mu_;
  std::shared_ptr<ICredentialProvider> creds_provider_;
};

}  // namespace vxcore

#endif  // VXCORE_TESTS_MOCK_SYNC_BACKEND_H
