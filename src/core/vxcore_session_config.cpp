#include "vxcore_session_config.h"

namespace vxcore {

VxCoreSessionConfig VxCoreSessionConfig::fromJson(const nlohmann::json &json) {
  (void)json;
  VxCoreSessionConfig config;
  return config;
}

nlohmann::json VxCoreSessionConfig::toJson() const {
  nlohmann::json json = nlohmann::json::object();
  return json;
}

} // namespace vxcore
