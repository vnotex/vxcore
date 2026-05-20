#include "test_internals/mock_sync_backend.h"

#include <sstream>

namespace vxcore {

MockSyncBackend::MockSyncBackend() = default;

MockSyncBackend::~MockSyncBackend() {
  Shutdown();
}

VxCoreError MockSyncBackend::Initialize(const std::string &root_folder,
                                        const SyncConfig &config) {
  RecordCall("Initialize");
  root_folder_ = root_folder;
  config_ = config;
  initialized_ = true;
  return GetReturnCode("Initialize");
}

VxCoreError MockSyncBackend::SetCredentials(const SyncCredentials &creds) {
  RecordCall("SetCredentials");
  (void)creds;
  return GetReturnCode("SetCredentials");
}

VxCoreError MockSyncBackend::Shutdown() {
  if (!initialized_) return VXCORE_OK;
  RecordCall("Shutdown");
  initialized_ = false;
  return GetReturnCode("Shutdown");
}

VxCoreError MockSyncBackend::Sync(SyncProgressCallback callback,
                                  void *userdata) {
  RecordCall("Sync");
  (void)callback;
  (void)userdata;

  if (fake_conflicts_enabled_) {
    return VXCORE_ERR_SYNC_CONFLICT;
  }

  return GetReturnCode("Sync");
}

VxCoreError MockSyncBackend::Push(SyncProgressCallback callback,
                                  void *userdata) {
  RecordCall("Push");
  (void)callback;
  (void)userdata;
  return GetReturnCode("Push");
}

VxCoreError MockSyncBackend::Pull(SyncProgressCallback callback,
                                  void *userdata) {
  RecordCall("Pull");
  (void)callback;
  (void)userdata;
  return GetReturnCode("Pull");
}

VxCoreError MockSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  RecordCall("GetStatus");
  out_files.clear();
  return GetReturnCode("GetStatus");
}

VxCoreError MockSyncBackend::GetConflicts(
    std::vector<SyncConflictInfo> &out_conflicts) {
  RecordCall("GetConflicts");
  out_conflicts.clear();
  return GetReturnCode("GetConflicts");
}

VxCoreError MockSyncBackend::ResolveConflict(
    const std::string &path, SyncConflictResolution resolution) {
  RecordCall("ResolveConflict");
  (void)path;
  (void)resolution;
  return GetReturnCode("ResolveConflict");
}

void MockSyncBackend::SetReturnCode(const std::string &method_name,
                                    VxCoreError code) {
  return_codes_[method_name] = code;
}

void MockSyncBackend::EnableFakeConflicts(bool enable) {
  fake_conflicts_enabled_ = enable;
}

size_t MockSyncBackend::GetCallCount() const {
  return call_records_.size();
}

const std::vector<std::string> &MockSyncBackend::GetCallRecords() const {
  return call_records_;
}

void MockSyncBackend::ClearCallRecords() {
  call_records_.clear();
}

VxCoreError MockSyncBackend::GetReturnCode(const std::string &method_name) {
  auto it = return_codes_.find(method_name);
  if (it != return_codes_.end()) {
    return it->second;
  }
  return VXCORE_OK;
}

void MockSyncBackend::RecordCall(const std::string &method_name) {
  call_records_.push_back(method_name);
}

}  // namespace vxcore
