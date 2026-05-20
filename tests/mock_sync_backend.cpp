#include "mock_sync_backend.h"

namespace vxcore {

VxCoreError MockSyncBackend::Initialize(const std::string &root_folder,
                                        const SyncConfig &config) {
  (void)root_folder;
  (void)config;
  ++initialize_call_count;
  set_credentials_call_count_at_initialize = set_credentials_call_count;
  return next_initialize_result;
}

VxCoreError MockSyncBackend::SetCredentials(const SyncCredentials &creds) {
  ++set_credentials_call_count;
  last_credentials = creds;
  return next_set_credentials_result;
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
