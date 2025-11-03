#ifndef VXCORE_VXCORE_SESSION_CONFIG_H
#define VXCORE_VXCORE_SESSION_CONFIG_H

#include "notebook.h"

#include <nlohmann/json.hpp>
#include <vector>

namespace vxcore {

struct VxCoreSessionConfig {
  std::vector<NotebookRecord> notebooks;

  VxCoreSessionConfig() {}

  static VxCoreSessionConfig fromJson(const nlohmann::json &json);
  nlohmann::json toJson() const;
};

} // namespace vxcore

#endif
