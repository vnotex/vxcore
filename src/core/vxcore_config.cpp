#include "vxcore_config.h"

namespace vxcore {

VxCoreConfig VxCoreConfig::FromJson(const nlohmann::json &json) {
  VxCoreConfig config;
  if (json.contains("version") && json["version"].is_string()) {
    config.version = json["version"].get<std::string>();
  }
  return config;
}

nlohmann::json VxCoreConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["version"] = version;
  return json;
}

}  // namespace vxcore
