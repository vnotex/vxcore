#include "json_helpers.h"
#include <fstream>
#include <iostream>
#include <sstream>

namespace vxcore_cli {

nlohmann::json JsonHelpers::buildProperties(const std::string &propsOption,
                                            const std::string &propsJsonOption,
                                            const std::vector<std::string> &propsList) {
  nlohmann::json result = nlohmann::json::object();

  if (!propsOption.empty()) {
    if (propsOption == "-") {
      result = loadFromStdin();
    } else if (propsOption.size() > 1 && propsOption[0] == '@') {
      std::string filename = propsOption.substr(1);
      result = loadFromFile(filename);
    }
  }

  if (!propsJsonOption.empty()) {
    nlohmann::json inlineJson = nlohmann::json::parse(propsJsonOption);
    result.merge_patch(inlineJson);
  }

  for (const auto &prop : propsList) {
    applyKeyValue(result, prop);
  }

  return result;
}

nlohmann::json JsonHelpers::loadFromFile(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + path);
  }
  nlohmann::json json;
  file >> json;
  return json;
}

nlohmann::json JsonHelpers::loadFromStdin() {
  std::string line;
  std::stringstream buffer;
  while (std::getline(std::cin, line)) {
    buffer << line << '\n';
  }
  return nlohmann::json::parse(buffer.str());
}

void JsonHelpers::applyKeyValue(nlohmann::json &json, const std::string &keyValue) {
  size_t pos = keyValue.find('=');
  if (pos == std::string::npos) {
    throw std::runtime_error("Invalid key=value format: " + keyValue);
  }

  std::string key = keyValue.substr(0, pos);
  std::string value = keyValue.substr(pos + 1);

  std::vector<std::string> path = splitPath(key);
  setNestedValue(json, path, value);
}

std::vector<std::string> JsonHelpers::splitPath(const std::string &path) {
  std::vector<std::string> result;
  std::string current;

  for (char ch : path) {
    if (ch == '.') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current += ch;
    }
  }

  if (!current.empty()) {
    result.push_back(current);
  }

  return result;
}

void JsonHelpers::setNestedValue(nlohmann::json &json, const std::vector<std::string> &path,
                                 const std::string &value) {
  if (path.empty()) {
    return;
  }

  nlohmann::json *current = &json;

  for (size_t i = 0; i < path.size() - 1; ++i) {
    if (!current->contains(path[i]) || !(*current)[path[i]].is_object()) {
      (*current)[path[i]] = nlohmann::json::object();
    }
    current = &(*current)[path[i]];
  }

  (*current)[path.back()] = value;
}

} // namespace vxcore_cli
