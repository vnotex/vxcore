#ifndef VXCORE_CONTEXT_H
#define VXCORE_CONTEXT_H

#include <memory>
#include <string>

namespace vxcore {

class ConfigManager;
class NotebookManager;

struct VxCoreContext {
  std::unique_ptr<ConfigManager> config_manager;
  std::unique_ptr<NotebookManager> notebook_manager;
  std::string last_error;
};

}  // namespace vxcore

#endif
