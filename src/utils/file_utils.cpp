#include "file_utils.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(VXCORE_BUILD_DLL)
#include "logger.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef VXCORE_LOG_DEBUG
#define VXCORE_LOG_DEBUG(...) ((void)0)
#endif

namespace vxcore {

static constexpr char kPathSeparator = '/';

#ifdef _WIN32
static std::string WideToUtf8(const std::wstring &wide_str) {
  if (wide_str.empty()) {
    return {};
  }

  int utf8_size =
      WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0) {
    return {};
  }

  std::string utf8_str(static_cast<size_t>(utf8_size), '\0');
  int converted = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, utf8_str.data(), utf8_size,
                                      nullptr, nullptr);
  if (converted <= 0) {
    return {};
  }

  utf8_str.resize(static_cast<size_t>(utf8_size - 1));
  return utf8_str;
}
#endif

std::filesystem::path PathFromUtf8(const std::string &utf8_str) {
  if (utf8_str.empty()) {
    return {};
  }

#ifdef _WIN32
  int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
  if (wide_size <= 0) {
    return {};
  }

  int wide_len = wide_size - 1;
  std::wstring wide_str(static_cast<size_t>(wide_size), L'\0');
  int converted = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wide_str.data(), wide_size);
  if (converted <= 0) {
    return {};
  }

  wide_str.resize(static_cast<size_t>(wide_len));
  VXCORE_LOG_DEBUG("PathFromUtf8: input_bytes=%zu, wide_chars=%d", utf8_str.size(), wide_len);
  return std::filesystem::path(wide_str);
#else
  return std::filesystem::path(utf8_str);
#endif
}

std::string PathToUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return WideToUtf8(path.wstring());
#else
  return path.string();
#endif
}

std::string PathToGenericUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return WideToUtf8(path.generic_wstring());
#else
  return path.generic_string();
#endif
}

std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name) {
  if (parent_path.empty() || parent_path == ".") {
    return child_name;
  } else {
    return parent_path + kPathSeparator + child_name;
  }
}

std::pair<std::string, std::string> SplitPath(const std::string &path) {
  size_t last_slash = path.find_last_of(kPathSeparator);
  if (last_slash == std::string::npos) {
    return {".", path};
  } else {
    std::string parent_path = path.substr(0, last_slash);
    std::string child_name = path.substr(last_slash + 1);
    return {parent_path, child_name};
  }
}

std::vector<std::string> SplitPathComponents(const std::string &path) {
  std::vector<std::string> components;
  size_t start = 0;
  size_t end = path.find(kPathSeparator);
  while (end != std::string::npos) {
    if (end != start) {
      components.push_back(path.substr(start, end - start));
    }
    start = end + 1;
    end = path.find(kPathSeparator, start);
  }
  if (start < path.size()) {
    components.push_back(path.substr(start));
  }
  return components;
}

bool IsRelativePath(const std::string &path) { return PathFromUtf8(path).is_relative(); }

bool IsSingleName(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  if (!PathFromUtf8(path).is_relative()) {
    return false;
  }
  return path.find(kPathSeparator) == std::string::npos && path.find('\\') == std::string::npos;
}

std::string RelativePath(const std::string &base, const std::string &path) {
  if (base.empty()) {
    return std::string();
  }

  if (path.empty()) {
    return ".";
  }

  if (path.size() < base.size() || path.substr(0, base.size()) != base) {
    return std::string();
  }

  if (path.size() > base.size() && path[base.size()] == kPathSeparator) {
    return path.substr(base.size() + 1);
  } else {
    return path.substr(base.size());
  }
}

VxCoreError ReadFile(const std::filesystem::path &path, std::string &out_content) {
  try {
    std::ifstream file(path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out_content = ss.str();
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError ReadFileHead(const std::filesystem::path &path, size_t max_bytes,
                         std::string &out_content) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    out_content.resize(max_bytes);
    file.read(&out_content[0], max_bytes);
    out_content.resize(static_cast<size_t>(file.gcount()));
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError WriteFile(const std::filesystem::path &path, const std::string &content) {
  try {
    std::ofstream file(path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    file << content;
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError LoadJsonFile(const std::filesystem::path &path, nlohmann::json &out_json) {
  try {
    std::ifstream file(path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    file >> out_json;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

}  // namespace vxcore
