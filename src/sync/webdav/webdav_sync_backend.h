#ifndef VXCORE_SYNC_WEBDAV_SYNC_BACKEND_H
#define VXCORE_SYNC_WEBDAV_SYNC_BACKEND_H

#include <memory>
#include <string>
#include <vector>

#include "sync/sync_backend.h"

namespace vxcore {

// Callers serialize phase methods and working-tree mutations with their notebook IO gate.
// Status, conflicts, provider replacement and cancellation remain independently callable.
class WebDavSyncBackend final : public ISyncBackend {
 public:
  WebDavSyncBackend(const SyncConfig &config, std::shared_ptr<ICredentialProvider> provider);
  ~WebDavSyncBackend() override;
  std::string GetName() const override;
  SyncCapabilities GetCapabilities() const override;
  bool IsInitialized() const override;
  VxCoreError Initialize(const std::string &root_folder, const SyncConfig &config) override;
  VxCoreError Clone(const std::string &target_dir, const SyncConfig &config) override;
  void ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) override;
  std::shared_ptr<ICredentialProvider> GetCredsProviderSnapshot() const override;
  void SetCancellation(SyncCancellationPtr token) override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError StageAndCommit(bool *out_did_commit) override;
  VxCoreError FetchRebasePush() override;
  VxCoreError ApplySync(const std::vector<std::string> &protected_paths,
                        std::vector<std::string> &out_changed_paths) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path, SyncConflictResolution resolution) override;
  std::string GetLastError() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_WEBDAV_SYNC_BACKEND_H
