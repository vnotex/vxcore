#ifndef VXCORE_TEST_INTERNALS_MOCK_SYNC_BACKEND_H
#define VXCORE_TEST_INTERNALS_MOCK_SYNC_BACKEND_H

#include <string>
#include <vector>
#include <map>

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
  ~MockSyncBackend() override;

  // ISyncBackend implementation
  VxCoreError Initialize(const std::string &root_folder,
                         const SyncConfig &config) override;
  VxCoreError SetCredentials(const SyncCredentials &creds) override;
  VxCoreError Shutdown() override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Push(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Pull(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

  // Configuration methods
  void SetReturnCode(const std::string &method_name, VxCoreError code);
  void EnableFakeConflicts(bool enable);

  // Call recording accessors
  size_t GetCallCount() const;
  const std::vector<std::string> &GetCallRecords() const;
  void ClearCallRecords();

 private:
  std::map<std::string, VxCoreError> return_codes_;
  std::vector<std::string> call_records_;
  bool fake_conflicts_enabled_ = false;
  std::string root_folder_;
  SyncConfig config_;
  bool initialized_ = false;

  VxCoreError GetReturnCode(const std::string &method_name);
  void RecordCall(const std::string &method_name);
};

}  // namespace vxcore

#endif  // VXCORE_TEST_INTERNALS_MOCK_SYNC_BACKEND_H
