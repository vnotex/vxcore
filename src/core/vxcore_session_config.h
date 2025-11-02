#ifndef VXCORE_VXCORE_SESSION_CONFIG_H
#define VXCORE_VXCORE_SESSION_CONFIG_H

#include <nlohmann/json.hpp>

namespace vxcore {

struct VxCoreSessionConfig {
  VxCoreSessionConfig() {}

  static VxCoreSessionConfig fromJson(const nlohmann::json &json);
  nlohmann::json toJson() const;
};

} // namespace vxcore

#endif
