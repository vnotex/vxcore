#ifndef VXCORE_WORKSPACE_H
#define VXCORE_WORKSPACE_H

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vxcore {

struct WorkspaceConfig {
  std::string id;
  std::string name;
  std::vector<std::string> buffer_ids;
  std::string current_buffer_id;
  nlohmann::json metadata;
  std::map<std::string, nlohmann::json> buffer_metadata;

  WorkspaceConfig();

  static WorkspaceConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct WorkspaceRecord {
  std::string id;
  std::string name;
  std::vector<std::string> buffer_ids;
  std::string current_buffer_id;
  nlohmann::json metadata;
  std::map<std::string, nlohmann::json> buffer_metadata;

  WorkspaceRecord();

  static WorkspaceRecord FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

}  // namespace vxcore

#endif  // VXCORE_WORKSPACE_H
