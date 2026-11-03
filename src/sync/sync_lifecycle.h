#ifndef VXCORE_SYNC_LIFECYCLE_H
#define VXCORE_SYNC_LIFECYCLE_H

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "sync/sync_backend.h"
#include "sync/sync_types.h"
#include "utils/file_utils.h"

namespace vxcore {
struct VxCoreContext;
class Notebook;
class SyncManager;

// The caller owns context/notebook affinity and joins Execute before Complete/Free.
// Execute uses only the immutable snapshot and operation-owned backend, never owner_.
class SyncLifecycle final {
 public:
  static VxCoreError Prepare(VxCoreContext &context, const char *notebook_id,
                            const nlohmann::json &command, SyncCredentials credentials,
                            std::unique_ptr<SyncLifecycle> &out);
  ~SyncLifecycle();
  VxCoreError Execute(SyncCancellationPtr cancellation) noexcept;
  VxCoreError Complete(VxCoreContext &context, nlohmann::json &out) noexcept;

 private:
  enum class Phase { kPrepared, kExecuting, kExecuted, kCompleting, kCompleted };
  SyncLifecycle() = default;
  void ValidateOwner(VxCoreContext &context) const;
  void CompleteImpl(VxCoreContext &context);
  nlohmann::json Summary(VxCoreError code) const;

  VxCoreContext *owner_ = nullptr;
  SyncManager *manager_ = nullptr;
  Notebook *notebook_ = nullptr;  // Owner-only identity, never dereferenced by Execute.
  ISyncBackend *previous_backend_ = nullptr;  // Owner-only registration fence.
  std::atomic<Phase> phase_{Phase::kPrepared};
  bool leased_ = false;
  bool executed_ = false;
  bool committed_ = false;
  bool cleanup_required_ = false;
  VxCoreError execution_code_ = VXCORE_ERR_INVALID_STATE;
  VxCoreError completion_code_ = VXCORE_ERR_INVALID_STATE;
  std::string kind_;
  std::string operation_id_;
  std::string recovery_operation_id_;
  std::string notebook_id_;
  std::string lease_key_;
  std::string root_;
  std::string owner_config_;
  std::string config_sha256_;
  std::string baseline_sha256_;
  FileIdentity root_identity_;
  SyncConfig config_;
  SyncCancellationPtr cancellation_;
  std::shared_ptr<ICredentialProvider> provider_;
  std::unique_ptr<ISyncBackend> backend_;
};

}  // namespace vxcore
#endif
