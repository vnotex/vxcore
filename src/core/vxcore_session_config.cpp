#include "vxcore_session_config.h"

namespace vxcore {

VxCoreSessionConfig VxCoreSessionConfig::FromJson(const nlohmann::json &json) {
  VxCoreSessionConfig config;
  if (json.contains("notebooks") && json["notebooks"].is_array()) {
    for (const auto &item : json["notebooks"]) {
      config.notebooks.push_back(NotebookRecord::FromJson(item));
    }
  }
  return config;
}

nlohmann::json VxCoreSessionConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  nlohmann::json notebooksArray = nlohmann::json::array();
  for (const auto &record : notebooks) {
    notebooksArray.push_back(record.ToJson());
  }
  json["notebooks"] = notebooksArray;
  return json;
}

}  // namespace vxcore
