#ifndef VXCORE_CONTEXT_H
#define VXCORE_CONTEXT_H

#include "config_manager.h"
#include "notebook_manager.h"
#include "vxcore/vxcore_types.h"

#include <memory>
#include <string>

namespace vxcore {

struct VxCoreContext {
  std::unique_ptr<ConfigManager> config_manager;
  std::unique_ptr<NotebookManager> notebook_manager;
  std::string last_error;
};

} // namespace vxcore

#endif
