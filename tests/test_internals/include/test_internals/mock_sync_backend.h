#ifndef VXCORE_TEST_INTERNALS_MOCK_SYNC_BACKEND_H
#define VXCORE_TEST_INTERNALS_MOCK_SYNC_BACKEND_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sync/credential_provider.h"
#include "sync/sync_backend.h"
#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// MockSyncBackend — configurable mock implementation of ISyncBackend for testing.
//
// Features:
// - Per-method configurable return codes (default VXCORE_OK)
// - Call recording: tracks method invocations with argument summaries
// - Fake conflicts: when enabled, Sync() returns VXCORE_ERR_SYNC_CONFLICT
//
// Usage:
//   MockSyncBackend mock;
//   mock.SetReturnCode("Sync", VXCORE_ERR_NETWORK);
//   mock.EnableFakeConflicts(true);
//   VxCoreError err = mock.Sync(nullptr, nullptr);  // returns VXCORE_ERR_NETWORK
//   ASSERT_EQ(mock.GetCallCount(), 1);
class MockSyncBackend : public ISyncBackend {
 public:
  MockSyncBackend();
  // Task 6.2 (F4.4) ctor matching the SyncBackendFactory shape. The provider
  // is stored and returned via GetCredsProvider() for assertions.
  explicit MockSyncBackend(std::shared_ptr<ICredentialProvider> creds_provider);
  ~MockSyncBackend() override;

  // ISyncBackend implementation
  std::string GetName() const override;
  SyncCapabilities GetCapabilities() const override;
  bool IsInitialized() const override;
  VxCoreError Initialize(const std::string &root_folder,
                         const SyncConfig &config) override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

  // Wave 6.3 F4.4: ReplaceCredsProvider override records the rotation so
  // tests can assert that SyncManager::UpdateCredentials forwarded the new
  // provider. The base-class default is a no-op.
  void ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) override;
  std::shared_ptr<ICredentialProvider> GetCredsProviderSnapshot() const override;

  // Configuration methods
  void SetReturnCode(const std::string &method_name, VxCoreError code);
  void EnableFakeConflicts(bool enable);

  // T8 (sync-queue-convergence): populate the conflict set that GetConflicts
  // returns AND make Sync() return VXCORE_ERR_SYNC_CONFLICT. Allows tests to
  // assert that TriggerSync emits sync.conflict with the expected files list.
  void SetConflicts(std::vector<SyncConflictInfo> conflicts);

  // Task 4.1 (sync-backend-phase4 F1.2): allow tests to override identity
  // methods. Defaults: name="mock", capabilities=None,
  // IsInitialized()=true so legacy tests don't have to call Initialize()
  // first to exercise other methods.
  void SetName(std::string name);
  void SetCapabilities(SyncCapabilities caps);
  void SetInitialized(bool initialized);

  // Call recording accessors
  size_t GetCallCount() const;
  const std::vector<std::string> &GetCallRecords() const;
  void ClearCallRecords();

  // Task 6.2 (F4.4): expose the provider supplied via the factory ctor so
  // tests can assert that the registry forwarded it correctly. Returns
  // nullptr for instances constructed via the default ctor.
  //
  // Wave 6.3 F4.4: also reflects the latest ReplaceCredsProvider() call —
  // when SyncManager::UpdateCredentials rotates the provider, this getter
  // returns the new shared_ptr.
  std::shared_ptr<ICredentialProvider> GetCredsProvider() const;

  // Wave 6.3 F4.4 rotation recorder.
  int replace_creds_provider_call_count = 0;

  // Task 6.2 (F4.4) capability override for registry-built mocks. The
  // registration lambda has no per-instance hook, so AuthRequired tests set
  // this static atomic before triggering construction and reset it after.
  // RAII helper ScopedCapabilityOverride below makes this safe.
  static void SetNextCapabilitiesOverride(SyncCapabilities caps);
  static void ClearNextCapabilitiesOverride();
  static bool HasNextCapabilitiesOverride();
  static SyncCapabilities ConsumeNextCapabilitiesOverride();

  // RAII helper: sets the override on ctor, clears on dtor. Use it to scope
  // an AuthRequired capability injection to a single test case.
  class ScopedCapabilityOverride {
   public:
    explicit ScopedCapabilityOverride(SyncCapabilities caps);
    ~ScopedCapabilityOverride();
    ScopedCapabilityOverride(const ScopedCapabilityOverride &) = delete;
    ScopedCapabilityOverride &operator=(const ScopedCapabilityOverride &) = delete;
  };

 private:
  std::map<std::string, VxCoreError> return_codes_;
  std::vector<std::string> call_records_;
  bool fake_conflicts_enabled_ = false;
  std::vector<SyncConflictInfo> fake_conflicts_;
  std::string root_folder_;
  SyncConfig config_;
  bool initialized_ = false;
  std::string name_ = "mock";
  SyncCapabilities capabilities_ = static_cast<SyncCapabilities>(SyncCapability::None);
  bool initialized_override_ = true;
  std::shared_ptr<ICredentialProvider> creds_provider_;
  mutable std::mutex creds_provider_mu_;

  VxCoreError GetReturnCode(const std::string &method_name);
  void RecordCall(const std::string &method_name);
};

}  // namespace vxcore

#endif  // VXCORE_TEST_INTERNALS_MOCK_SYNC_BACKEND_H
