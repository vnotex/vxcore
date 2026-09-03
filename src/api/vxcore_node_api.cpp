#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/api_utils.h"
#include "core/buffer_manager.h"
#include "core/context.h"
#include "core/event_manager.h"
#include "core/folder_manager.h"
#include "core/node_transfer.h"
#include "core/notebook_manager.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

// Helper function to detect whether a path refers to a file or folder
static VxCoreError DetectNodeType(vxcore::Notebook *notebook, const std::string &path,
                                  vxcore::NodeType &out_type) {
  vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
  if (!folder_manager) {
    return VXCORE_ERR_INVALID_STATE;
  }

  // Try file first (files are more common)
  std::string file_info_json;
  VxCoreError err = folder_manager->GetFileInfo(path, file_info_json);
  if (err == VXCORE_OK) {
    out_type = vxcore::NodeType::File;
    return VXCORE_OK;
  }

  // Try folder
  std::string folder_config_json;
  err = folder_manager->GetFolderConfig(path, folder_config_json);
  if (err == VXCORE_OK) {
    out_type = vxcore::NodeType::Folder;
    return VXCORE_OK;
  }

  return VXCORE_ERR_NOT_FOUND;
}

struct VxCoreNodeTransfer_ {
  vxcore::VxCoreContext *context = nullptr;
  std::unique_ptr<vxcore::PreparedNodeTransfer> transfer;
};

namespace {

struct TransferHandleRegistry {
  std::mutex mutex;
  std::unordered_map<vxcore::VxCoreContext *, std::unordered_set<VxCoreNodeTransferHandle>> handles;
  std::unordered_map<vxcore::VxCoreContext *, std::unordered_map<std::string, nlohmann::json>>
      event_batches;
  std::unordered_set<vxcore::VxCoreContext *> cleanup_registered;
};

TransferHandleRegistry &GetTransferHandleRegistry() {
  static auto *registry = new TransferHandleRegistry();
  return *registry;
}

void DiscardRegisteredHandles(vxcore::VxCoreContext *context) {
  std::vector<VxCoreNodeTransferHandle> handles;
  auto &registry = GetTransferHandleRegistry();
  {
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.handles.find(context);
    if (it != registry.handles.end()) {
      handles.assign(it->second.begin(), it->second.end());
      registry.handles.erase(it);
    }
    registry.event_batches.erase(context);
    registry.cleanup_registered.erase(context);
  }
  for (auto *handle : handles) {
    vxcore::NodeTransfer::Discard(std::move(handle->transfer));
    delete handle;
  }
}

struct ContextTransferCleanup {
  explicit ContextTransferCleanup(vxcore::VxCoreContext *context) : context(context) {}
  ~ContextTransferCleanup() { DiscardRegisteredHandles(context); }

  vxcore::VxCoreContext *context;
};

void EnsureContextCleanup(vxcore::VxCoreContext *context) {
  bool first = false;
  auto &registry = GetTransferHandleRegistry();
  {
    std::lock_guard<std::mutex> lock(registry.mutex);
    first = registry.cleanup_registered.insert(context).second;
  }
  if (!first) {
    return;
  }
  try {
    auto cleanup = std::make_shared<ContextTransferCleanup>(context);
    context->event_manager->Subscribe("__vxcore.transfer.context_lifetime",
                                      [cleanup](const std::string &, const nlohmann::json &) {});
  } catch (...) {
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.cleanup_registered.erase(context);
    throw;
  }
}

void RegisterTransferHandle(vxcore::VxCoreContext *context, VxCoreNodeTransferHandle handle) {
  EnsureContextCleanup(context);
  auto &registry = GetTransferHandleRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.handles[context].insert(handle);
}

void UnregisterTransferHandle(vxcore::VxCoreContext *context, VxCoreNodeTransferHandle handle) {
  auto &registry = GetTransferHandleRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto it = registry.handles.find(context);
  if (it == registry.handles.end()) {
    return;
  }
  it->second.erase(handle);
  if (it->second.empty()) {
    registry.handles.erase(it);
  }
}

struct ReservedEventBatch {
  std::string id;
  nlohmann::json *events = nullptr;
};

ReservedEventBatch ReserveEventBatch(vxcore::VxCoreContext *context) {
  auto &registry = GetTransferHandleRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto &batches = registry.event_batches[context];
  for (;;) {
    const std::string id = vxcore::GenerateUUID();
    auto inserted = batches.emplace(id, nlohmann::json::array());
    if (inserted.second) {
      return {id, &inserted.first->second};
    }
  }
}

void RemoveEventBatch(vxcore::VxCoreContext *context, const std::string &id) {
  if (id.empty()) {
    return;
  }
  auto &registry = GetTransferHandleRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto context_it = registry.event_batches.find(context);
  if (context_it == registry.event_batches.end()) {
    return;
  }
  context_it->second.erase(id);
  if (context_it->second.empty()) {
    registry.event_batches.erase(context_it);
  }
}

bool TakeEventBatch(vxcore::VxCoreContext *context, const std::string &id,
                    nlohmann::json &out_events) {
  auto &registry = GetTransferHandleRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto context_it = registry.event_batches.find(context);
  if (context_it == registry.event_batches.end()) {
    return false;
  }
  auto batch_it = context_it->second.find(id);
  if (batch_it == context_it->second.end()) {
    return false;
  }
  out_events = std::move(batch_it->second);
  context_it->second.erase(batch_it);
  if (context_it->second.empty()) {
    registry.event_batches.erase(context_it);
  }
  return true;
}

}  // namespace

VXCORE_API VxCoreError vxcore_node_transfer_prepare(
    VxCoreContextHandle context, const char *source_notebook_id, const char *source_relative_path,
    const char *destination_notebook_id, const char *destination_folder_path,
    const char *options_json, VxCoreNodeTransferProgressCallback progress_callback, void *userdata,
    VxCoreNodeTransferHandle *out_transfer) {
  if (out_transfer) {
    *out_transfer = nullptr;
  }
  if (!context || !source_notebook_id || !source_relative_path || !destination_notebook_id ||
      !destination_folder_path || !options_json || !out_transfer) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    const nlohmann::json options = nlohmann::json::parse(options_json);
    std::unique_ptr<vxcore::PreparedNodeTransfer> prepared;
    std::string error_message;
    const vxcore::NodeTransferProgress progress =
        progress_callback
            ? [progress_callback, userdata](const std::string &phase, uint64_t completed,
                                            uint64_t total) {
                return progress_callback(phase.c_str(), completed, total, userdata) == 0;
              }
            : vxcore::NodeTransferProgress();
    VxCoreError error = vxcore::NodeTransfer::Prepare(
        ctx->notebook_manager.get(), source_notebook_id, source_relative_path,
        destination_notebook_id, destination_folder_path, options, progress, prepared,
        error_message);
    if (error != VXCORE_OK) {
      ctx->last_error = error_message;
      return error;
    }
    auto handle = std::make_unique<VxCoreNodeTransfer_>();
    handle->context = ctx;
    handle->transfer = std::move(prepared);
    try {
      RegisterTransferHandle(ctx, handle.get());
    } catch (...) {
      vxcore::NodeTransfer::Discard(std::move(handle->transfer));
      throw;
    }
    *out_transfer = handle.release();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_transfer_commit(VxCoreContextHandle context,
                                                   VxCoreNodeTransferHandle transfer,
                                                   char **out_result_json) {
  if (out_result_json) {
    *out_result_json = nullptr;
  }
  if (!context || !transfer || !out_result_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (transfer->context != ctx) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  UnregisterTransferHandle(ctx, transfer);
  std::unique_ptr<VxCoreNodeTransfer_> handle(transfer);
  ReservedEventBatch event_batch;
  vxcore::NodeTransferCommitResult commit_result;
  try {
    event_batch = ReserveEventBatch(ctx);
    std::string error_message;
    const VxCoreError error = vxcore::NodeTransfer::Commit(
        ctx->notebook_manager.get(), std::move(handle->transfer), event_batch.id, commit_result,
        *event_batch.events, error_message);
    ctx->last_error = error_message;
    if (error != VXCORE_OK) {
      RemoveEventBatch(ctx, event_batch.id);
      return error;
    }
    *out_result_json = commit_result.ReleaseSelected();
    if (!*out_result_json) {
      RemoveEventBatch(ctx, event_batch.id);
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    if (commit_result.HasCommittedDestination()) {
      *out_result_json = commit_result.ReleaseRecovery();
    }
    if (*out_result_json) {
      return VXCORE_OK;
    }
    RemoveEventBatch(ctx, event_batch.id);
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::exception &e) {
    if (commit_result.HasCommittedDestination()) {
      *out_result_json = commit_result.ReleaseRecovery();
    }
    if (*out_result_json) {
      return VXCORE_OK;
    }
    RemoveEventBatch(ctx, event_batch.id);
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_transfer_dispatch_events(VxCoreContextHandle context,
                                                            const char *event_batch_id) {
  if (!context || !event_batch_id || !*event_batch_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  nlohmann::json events;
  if (!TakeEventBatch(ctx, event_batch_id, events)) {
    return VXCORE_ERR_NOT_FOUND;
  }
  try {
    for (const auto &event : events) {
      if (!event.is_object() || !event.contains("name") || !event["name"].is_string() ||
          !event.contains("data") || !event["data"].is_object()) {
        return VXCORE_ERR_INVALID_STATE;
      }
      ctx->event_manager->Emit(event["name"].get<std::string>(), event["data"]);
    }
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API void vxcore_node_transfer_free(VxCoreContextHandle context,
                                          VxCoreNodeTransferHandle transfer) {
  if (!transfer) {
    return;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx || transfer->context != ctx) {
    return;
  }
  UnregisterTransferHandle(ctx, transfer);
  std::unique_ptr<VxCoreNodeTransfer_> handle(transfer);
  vxcore::NodeTransfer::Discard(std::move(handle->transfer));
}

VXCORE_API VxCoreError vxcore_node_finalize_transfer_move(VxCoreContextHandle context,
                                                          const char *resume_token_json,
                                                          char **out_result_json) {
  if (out_result_json) {
    *out_result_json = nullptr;
  }
  if (!context || !resume_token_json || !out_result_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  ReservedEventBatch event_batch;
  try {
    EnsureContextCleanup(ctx);
    event_batch = ReserveEventBatch(ctx);
    const nlohmann::json token = nlohmann::json::parse(resume_token_json);
    std::string error_message;
    const VxCoreError error =
        vxcore::NodeTransfer::FinalizeMove(ctx->notebook_manager.get(), token, event_batch.id,
                                           out_result_json, *event_batch.events, error_message);
    ctx->last_error = error_message;
    if (error != VXCORE_OK) {
      RemoveEventBatch(ctx, event_batch.id);
      return error;
    }
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    RemoveEventBatch(ctx, event_batch.id);
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::bad_alloc &) {
    if (*out_result_json) {
      return VXCORE_OK;
    }
    RemoveEventBatch(ctx, event_batch.id);
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::exception &e) {
    if (*out_result_json) {
      return VXCORE_OK;
    }
    RemoveEventBatch(ctx, event_batch.id);
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_get_config(VxCoreContextHandle context, const char *notebook_id,
                                              const char *node_path, char **out_config_json) {
  if (!context || !notebook_id || !node_path || !out_config_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, node_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    // Reactive missing-content gate (bundled notebooks only): the node is
    // indexed in metadata but its content is gone from disk. The base
    // FolderManager default returns true, so raw notebooks are unaffected.
    if (!folder_manager->NodeContentExistsOnDisk(node_path,
                                                 node_type == vxcore::NodeType::Folder)) {
      ctx->last_error = "Node no longer exists on disk";
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }

    // Get config with type information
    std::string config_json;
    if (node_type == vxcore::NodeType::File) {
      err = folder_manager->GetFileInfo(node_path, config_json);
      if (err != VXCORE_OK) {
        return err;
      }
      // Parse and add type field
      nlohmann::json j = nlohmann::json::parse(config_json);
      j["type"] = "file";
      config_json = j.dump();
    } else {
      err = folder_manager->GetFolderConfig(node_path, config_json);
      if (err != VXCORE_OK) {
        return err;
      }
      // Parse and add type field
      nlohmann::json j = nlohmann::json::parse(config_json);
      j["type"] = "folder";
      config_json = j.dump();
    }

    *out_config_json = vxcore_strdup(config_json.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_delete(VxCoreContextHandle context, const char *notebook_id,
                                          const char *node_path) {
  if (!context || !notebook_id || !node_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, node_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    // Delete based on type
    if (node_type == vxcore::NodeType::File) {
      return folder_manager->DeleteFile(node_path);
    } else {
      return folder_manager->DeleteFolder(node_path);
    }
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_rename(VxCoreContextHandle context, const char *notebook_id,
                                          const char *node_path, const char *new_name) {
  if (!context || !notebook_id || !node_path || !new_name) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, node_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    // Rename based on type
    if (node_type == vxcore::NodeType::File) {
      err = folder_manager->RenameFile(node_path, new_name);
    } else {
      err = folder_manager->RenameFolder(node_path, new_name);
    }

    if (err != VXCORE_OK) {
      return err;
    }

    // Refresh open buffer paths from metadata store.
    if (ctx->buffer_manager) {
      ctx->buffer_manager->UpdatePaths(notebook_id);
    }

    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_move(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_path, const char *dest_parent_path) {
  if (!context || !notebook_id || !src_path || !dest_parent_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Convert empty dest_parent_path to "." for root folder
  std::string dest_path = dest_parent_path;
  if (dest_path.empty()) {
    dest_path = ".";
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, src_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    // Move based on type
    VxCoreError move_err;
    if (node_type == vxcore::NodeType::File) {
      move_err = folder_manager->MoveFile(src_path, dest_path);
    } else {
      move_err = folder_manager->MoveFolder(src_path, dest_path);
    }

    if (move_err != VXCORE_OK) {
      return move_err;
    }

    // Refresh open buffer paths from metadata store.
    if (ctx->buffer_manager) {
      ctx->buffer_manager->UpdatePaths(notebook_id);
    }

    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_copy(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_path, const char *dest_parent_path,
                                        const char *new_name, char **out_node_id) {
  if (!context || !notebook_id || !src_path || !dest_parent_path || !out_node_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Convert empty dest_parent_path to "." for root folder
  std::string dest_path = dest_parent_path;
  if (dest_path.empty()) {
    dest_path = ".";
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, src_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    std::string target_name = new_name ? new_name : "";
    std::string node_id;

    // Copy based on type
    if (node_type == vxcore::NodeType::File) {
      err = folder_manager->CopyFile(src_path, dest_path, target_name, node_id);
    } else {
      err = folder_manager->CopyFolder(src_path, dest_path, target_name, node_id);
    }

    if (err != VXCORE_OK) {
      return err;
    }

    *out_node_id = vxcore_strdup(node_id.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_get_metadata(VxCoreContextHandle context,
                                                const char *notebook_id, const char *node_path,
                                                char **out_metadata_json) {
  if (!context || !notebook_id || !node_path || !out_metadata_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, node_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    // Reactive missing-content gate (bundled notebooks only): the node is
    // indexed in metadata but its content is gone from disk. The base
    // FolderManager default returns true, so raw notebooks are unaffected.
    if (!folder_manager->NodeContentExistsOnDisk(node_path,
                                                 node_type == vxcore::NodeType::Folder)) {
      ctx->last_error = "Node no longer exists on disk";
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }

    std::string metadata_json;

    // Get metadata based on type
    if (node_type == vxcore::NodeType::File) {
      err = folder_manager->GetFileMetadata(node_path, metadata_json);
    } else {
      err = folder_manager->GetFolderMetadata(node_path, metadata_json);
    }

    if (err != VXCORE_OK) {
      return err;
    }

    *out_metadata_json = vxcore_strdup(metadata_json.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_update_metadata(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *node_path,
                                                   const char *metadata_json) {
  if (!context || !notebook_id || !node_path || !metadata_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    // Detect node type
    vxcore::NodeType node_type;
    VxCoreError err = DetectNodeType(notebook, node_path, node_type);
    if (err != VXCORE_OK) {
      return err;
    }

    // Update metadata based on type
    if (node_type == vxcore::NodeType::File) {
      return folder_manager->UpdateFileMetadata(node_path, metadata_json);
    } else {
      return folder_manager->UpdateFolderMetadata(node_path, metadata_json);
    }
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_index(VxCoreContextHandle context, const char *notebook_id,
                                         const char *node_path) {
  if (!context || !notebook_id || !node_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    return folder_manager->IndexNode(node_path);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_unindex(VxCoreContextHandle context, const char *notebook_id,
                                           const char *node_path) {
  if (!context || !notebook_id || !node_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    return folder_manager->UnindexNode(node_path);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    VXCORE_LOG_ERROR("Node API exception: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}
