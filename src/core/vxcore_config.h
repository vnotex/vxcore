#ifndef VXCORE_VXCORE_CONFIG_H
#define VXCORE_VXCORE_CONFIG_H

#include <nlohmann/json.hpp>
#include <string>

namespace vxcore {

struct VxCoreConfig {
  std::string version;

  VxCoreConfig() : version("0.1.0") {}

  static VxCoreConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

}  // namespace vxcore

#endif
