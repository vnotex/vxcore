#ifndef VXCORE_VXCORE_CONFIG_H
#define VXCORE_VXCORE_CONFIG_H

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vxcore {

struct SearchConfig {
  std::vector<std::string> backends;

  SearchConfig() : backends({"rg", "simple"}) {}

  static SearchConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct FileTypeEntry {
  int type_id;
  std::string name;
  std::vector<std::string> suffixes;
  bool is_newable;
  std::string display_name;
  std::string metadata;

  FileTypeEntry()
      : type_id(0), name(""), suffixes(), is_newable(true), display_name(""), metadata("") {}

  FileTypeEntry(int p_id, std::string p_name, std::vector<std::string> p_suffixes,
                bool p_newable = true, std::string p_display_name = "")
      : type_id(p_id),
        name(p_name),
        suffixes(p_suffixes),
        is_newable(p_newable),
        display_name(p_display_name.empty() ? p_name : p_display_name),
        metadata("") {}

  std::string GetDisplayName(const std::string &locale) const;

  static FileTypeEntry FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct FileTypesConfig {
  std::vector<FileTypeEntry> types;

  FileTypesConfig();

  const FileTypeEntry *GetBySuffix(const std::string &suffix) const;
  const FileTypeEntry *GetByName(const std::string &name) const;
  const FileTypeEntry *GetById(int type_id) const;

  static FileTypesConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct VxCoreConfig {
  std::string version;
  SearchConfig search;
  FileTypesConfig file_types;

  VxCoreConfig() : version("0.1.0"), search(), file_types() {}

  static VxCoreConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

}  // namespace vxcore

#endif
