#ifndef VXCORE_CONTEXT_H
#define VXCORE_CONTEXT_H

#include <memory>
#include <string>

namespace vxcore {

class ConfigManager;
class NotebookManager;
class WorkspaceManager;
class BufferManager;

struct VxCoreContext {
  std::unique_ptr<ConfigManager> config_manager;
  std::unique_ptr<NotebookManager> notebook_manager;
  std::unique_ptr<WorkspaceManager> workspace_manager;
  std::unique_ptr<BufferManager> buffer_manager;
  std::string last_error;
};

}  // namespace vxcore

#endif
