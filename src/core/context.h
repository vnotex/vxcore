#ifndef VXCORE_CONTEXT_H
#define VXCORE_CONTEXT_H

#include <memory>
#include <string>

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
  std::string last_error;
  bool shutdown_called = false;
};

}  // namespace vxcore

#endif
