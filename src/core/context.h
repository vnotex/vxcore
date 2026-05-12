#ifndef VXCORE_CONTEXT_H
#define VXCORE_CONTEXT_H

#include <BS_thread_pool/BS_thread_pool.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>

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

struct VxCoreContext {
  // IMPORTANT: Member order determines destruction order (reverse of declaration).
  // thread_pool must outlive all managers (first declared = last destroyed).
  // event_manager must outlive sync_manager and notebook_manager (they subscribe to events).
  // work_queue_manager must outlive event_manager (events may be dispatched via queues).
  // workspace_manager depends on buffer_manager and config_manager.
  // buffer_manager depends on notebook_manager and config_manager.
  std::unique_ptr<BS::thread_pool<>> thread_pool_;

  std::unique_ptr<ConfigManager> config_manager;
  std::unique_ptr<WorkQueueManager> work_queue_manager;
  std::unique_ptr<EventManager> event_manager;
  std::unique_ptr<NotebookManager> notebook_manager;
  std::unique_ptr<BufferManager> buffer_manager;
  std::unique_ptr<WorkspaceManager> workspace_manager;
  std::unique_ptr<TemplateManager> template_manager;
  std::unique_ptr<SnippetManager> snippet_manager;
  std::unique_ptr<SyncManager> sync_manager;
  std::string last_error;
  bool shutdown_called = false;

  BS::thread_pool<> &GetThreadPool() {
    if (!thread_pool_) {
      unsigned int num_threads = std::min(std::thread::hardware_concurrency(), 8u);
      if (num_threads == 0) num_threads = 2;
      thread_pool_ = std::make_unique<BS::thread_pool<>>(num_threads);
    }
    return *thread_pool_;
  }
};

}  // namespace vxcore

#endif
