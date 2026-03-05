#ifndef VXCORE_VXCORE_SESSION_CONFIG_H
#define VXCORE_VXCORE_SESSION_CONFIG_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "buffer.h"
#include "notebook.h"
#include "workspace.h"

namespace vxcore {

struct VxCoreSessionConfig {
  std::vector<NotebookRecord> notebooks;
  std::vector<WorkspaceRecord> workspaces;
  std::vector<BufferRecord> buffers;
  std::string current_workspace_id;

  VxCoreSessionConfig() {}

  static VxCoreSessionConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

}  // namespace vxcore

#endif
