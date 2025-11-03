#include "vxcore_session_config.h"

namespace vxcore {

VxCoreSessionConfig VxCoreSessionConfig::fromJson(const nlohmann::json &json) {
  VxCoreSessionConfig config;
  if (json.contains("notebooks") && json["notebooks"].is_array()) {
    for (const auto &item : json["notebooks"]) {
      config.notebooks.push_back(NotebookRecord::fromJson(item));
    }
  }
  return config;
}

nlohmann::json VxCoreSessionConfig::toJson() const {
  nlohmann::json json = nlohmann::json::object();
  nlohmann::json notebooksArray = nlohmann::json::array();
  for (const auto &record : notebooks) {
    notebooksArray.push_back(record.toJson());
  }
  json["notebooks"] = notebooksArray;
  return json;
}

} // namespace vxcore
