#ifndef VXCORE_VXCORE_SESSION_CONFIG_H
#define VXCORE_VXCORE_SESSION_CONFIG_H

#include <nlohmann/json.hpp>
#include <vector>

#include "notebook.h"

namespace vxcore {

struct VxCoreSessionConfig {
  std::vector<NotebookRecord> notebooks;

  VxCoreSessionConfig() {}

  static VxCoreSessionConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

}  // namespace vxcore

#endif
