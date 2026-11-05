#ifndef VXCORE_CONTEXT_H
#define VXCORE_CONTEXT_H

#include <memory>
#include <mutex>
#include <string>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;
class NotebookManager;
class WorkspaceManager;
class BufferManager;
class TemplateManager;
class SnippetManager;
class SyncManager;
class WorkQueueManager;
class EventManager;
class ActivityManager;

// Shared diagnostics are latest-context state, never operation attribution.
// A getter copies under the same lock; readers never borrow this storage.
class ContextError {
 public:
  ContextError &operator=(const std::string &message) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex_); value_ = message; } catch (...) {}
    return *this;
  }
  ContextError &operator=(std::string &&message) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex_); value_ = std::move(message); } catch (...) {}
    return *this;
  }
  ContextError &operator=(const char *message) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex_); value_ = message ? message : ""; } catch (...) {}
    return *this;
  }
  void clear() noexcept {
    try { std::lock_guard<std::mutex> lock(mutex_); value_.clear(); } catch (...) {}
  }
  void CopyTo(std::string &snapshot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = value_.empty() ? "No error" : value_;
  }
 private:
  mutable std::mutex mutex_;
  std::string value_;
};

struct VxCoreContext {
  // IMPORTANT: Member order determines destruction order (reverse of declaration).
  // event_manager must outlive sync_manager and notebook_manager (they subscribe to events).
  // work_queue_manager must outlive event_manager (events may be dispatched via queues).
  // workspace_manager depends on buffer_manager and config_manager.
  // buffer_manager depends on notebook_manager and config_manager.
  std::unique_ptr<ConfigManager> config_manager;
  std::unique_ptr<WorkQueueManager> work_queue_manager;
  std::unique_ptr<EventManager> event_manager;
  std::unique_ptr<NotebookManager> notebook_manager;
  std::unique_ptr<BufferManager> buffer_manager;
  std::unique_ptr<WorkspaceManager> workspace_manager;
  std::unique_ptr<TemplateManager> template_manager;
  std::unique_ptr<SnippetManager> snippet_manager;
  std::unique_ptr<SyncManager> sync_manager;
  // Declared AFTER event_manager so it is destroyed FIRST (reverse-order
  // destruction): its dtor unsubscribes from event_manager, which must still
  // be alive at that point.
  std::unique_ptr<ActivityManager> activity_manager;
  ContextError last_error;
  // App-wide locale used for locale-aware, UTF-8 output (see
  // vxcore_context_set_locale). Runtime-only: never persisted to vxcore.json.
  std::string locale = "en";
  bool shutdown_called = false;
};

VxCoreError InitializeContext(std::unique_ptr<ConfigManager> config_manager,
                              VxCoreContextHandle *out_context);

}  // namespace vxcore

#endif
