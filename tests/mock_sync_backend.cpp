#include "mock_sync_backend.h"

#include "sync/credential_provider.h"
#include "sync/sync_backend_registry.h"

namespace vxcore {

// Task 5.2 (sync-backend-phase4 F4.1): self-registration of the mock backend
// into SyncBackendRegistry. The token's constructor runs at static-init time
// and calls Registry::Register("mock", factory). BackendRegistration swallows
// any exception so static-init can never crash the program.
//
// Task 6.2 (F4.4): factory signature updated to take ICredentialProvider.
// Wave 6.3 F4.4: provider is stashed via ReplaceCredsProvider so the mock
// reflects whatever the registry forwarded.
namespace {
const BackendRegistration kMockRegistration{
    "mock",
    [](const SyncConfig & /*cfg*/,
       std::shared_ptr<ICredentialProvider> provider) -> std::unique_ptr<ISyncBackend> {
      auto mock = std::make_unique<MockSyncBackend>();
      if (provider) {
        mock->ReplaceCredsProvider(std::move(provider));
        mock->replace_creds_provider_call_count = 0;  // ctor-time forwarding, not a rotation
      }
      return mock;
    }};
}  // namespace

VxCoreError MockSyncBackend::Initialize(const std::string &root_folder,
                                        const SyncConfig &config) {
  (void)root_folder;
  (void)config;
  ++initialize_call_count;
  replace_creds_provider_call_count_at_initialize = replace_creds_provider_call_count;
  return next_initialize_result;
}

void MockSyncBackend::ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) {
  std::lock_guard<std::mutex> lk(creds_provider_mu_);
  creds_provider_ = std::move(provider);
  ++replace_creds_provider_call_count;
}

std::shared_ptr<ICredentialProvider> MockSyncBackend::GetCredsProviderSnapshot() const {
  std::lock_guard<std::mutex> lk(creds_provider_mu_);
  return creds_provider_;
}

VxCoreError MockSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  (void)callback;
  (void)userdata;
  ++sync_call_count;
  return next_sync_result;
}

VxCoreError MockSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  ++get_status_call_count;
  out_files = canned_status;
  return VXCORE_OK;
}

VxCoreError MockSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  ++get_conflicts_call_count;
  out_conflicts = canned_conflicts;
  return VXCORE_OK;
}

VxCoreError MockSyncBackend::ResolveConflict(const std::string &path,
                                             SyncConflictResolution resolution) {
  ++resolve_conflict_call_count;
  last_resolve_path = path;
  last_resolve_resolution = resolution;
  return next_resolve_conflict_result;
}

}  // namespace vxcore
