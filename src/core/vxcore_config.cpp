#include "vxcore_config.h"

namespace vxcore {

SearchConfig SearchConfig::FromJson(const nlohmann::json &json) {
  SearchConfig config;
  if (json.contains("backends") && json["backends"].is_array()) {
    config.backends.clear();
    for (const auto &backend : json["backends"]) {
      if (backend.is_string()) {
        config.backends.push_back(backend.get<std::string>());
      }
    }
  }
  return config;
}

nlohmann::json SearchConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["backends"] = backends;
  return json;
}

VxCoreConfig VxCoreConfig::FromJson(const nlohmann::json &json) {
  VxCoreConfig config;
  if (json.contains("version") && json["version"].is_string()) {
    config.version = json["version"].get<std::string>();
  }
  if (json.contains("search") && json["search"].is_object()) {
    config.search = SearchConfig::FromJson(json["search"]);
  }
  return config;
}

nlohmann::json VxCoreConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["version"] = version;
  json["search"] = search.ToJson();
  return json;
}

}  // namespace vxcore
