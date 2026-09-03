#ifndef VXCORE_NODE_TRANSFER_H
#define VXCORE_NODE_TRANSFER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class NotebookManager;
class Notebook;

enum class NodeTransferCommitOutcome {
  NotCommitted,
  DestinationCommitted,
  Success,
  SourceRetained,
  RecoveryRequired,
};

struct NodeTransferCommitResult {
  NodeTransferCommitResult() = default;
  ~NodeTransferCommitResult();

  NodeTransferCommitResult(const NodeTransferCommitResult &) = delete;
  NodeTransferCommitResult &operator=(const NodeTransferCommitResult &) = delete;

  void Reset() noexcept;
  bool HasCommittedDestination() const noexcept;
  char *ReleaseSelected() noexcept;
  char *ReleaseRecovery() noexcept;

  NodeTransferCommitOutcome outcome = NodeTransferCommitOutcome::NotCommitted;
  char *success_json = nullptr;
  char *retained_json = nullptr;
  char *recovery_json = nullptr;
};

struct PreparedNodeTransfer {
  virtual ~PreparedNodeTransfer();
};

using NodeTransferProgress =
    std::function<bool(const std::string &phase, uint64_t completed_bytes, uint64_t total_bytes)>;

class NodeTransfer {
 public:
  static VxCoreError Prepare(NotebookManager *notebook_manager,
                             const std::string &source_notebook_id,
                             const std::string &source_relative_path,
                             const std::string &destination_notebook_id,
                             const std::string &destination_folder_path,
                             const nlohmann::json &options, const NodeTransferProgress &progress,
                             std::unique_ptr<PreparedNodeTransfer> &out_transfer,
                             std::string &out_error);

  static VxCoreError Commit(NotebookManager *notebook_manager,
                            std::unique_ptr<PreparedNodeTransfer> transfer,
                            const std::string &event_batch_id, NodeTransferCommitResult &out_result,
                            nlohmann::json &out_events, std::string &out_error);

  static VxCoreError FinalizeMove(NotebookManager *notebook_manager,
                                  const nlohmann::json &resume_token,
                                  const std::string &event_batch_id, char **out_result_json,
                                  nlohmann::json &out_events, std::string &out_error);

  static VxCoreError Recover(Notebook *notebook, int *out_recovered_count);

  static void Discard(std::unique_ptr<PreparedNodeTransfer> transfer);
};

}  // namespace vxcore

#endif  // VXCORE_NODE_TRANSFER_H
