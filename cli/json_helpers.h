#ifndef VXCORE_CLI_JSON_HELPERS_H
#define VXCORE_CLI_JSON_HELPERS_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vxcore_cli {

class JsonHelpers {
public:
  static nlohmann::json buildProperties(const std::string &propsOption,
                                        const std::string &propsJsonOption,
                                        const std::vector<std::string> &propsList);

  static nlohmann::json loadFromFile(const std::string &path);

  static nlohmann::json loadFromStdin();

  static void applyKeyValue(nlohmann::json &json, const std::string &keyValue);

private:
  static void setNestedValue(nlohmann::json &json, const std::vector<std::string> &path,
                             const std::string &value);

  static std::vector<std::string> splitPath(const std::string &path);
};

} // namespace vxcore_cli

#endif
