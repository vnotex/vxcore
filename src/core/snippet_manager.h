#ifndef VXCORE_SNIPPET_MANAGER_H
#define VXCORE_SNIPPET_MANAGER_H

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;
class DateTimeNames;

enum class SnippetType {
  kText,
  kDynamic,
};

struct SnippetData {
  std::string name;
  std::string description;
  SnippetType type = SnippetType::kText;
  std::string content;
  std::string cursor_mark = "@@";
  std::string selection_mark = "$$";
  bool indent_as_first_line = false;
  bool is_builtin = false;
};

struct ApplyResult {
  std::string text;
  int cursor_offset = -1;
};

using DynamicCallback = std::function<std::string()>;
using OverrideMap = std::unordered_map<std::string, std::string>;

// NOT thread-safe. Set the locale (SetLocale) before use, from the same thread
// that applies snippets; the built-in dynamic callbacks read the locale state at
// call time.
class SnippetManager {
 public:
  explicit SnippetManager(ConfigManager *config_manager);

  // Copy and move are deleted: the built-in dynamic callbacks capture `this`, so
  // a copied/moved manager would carry callbacks still pointing at the original
  // object. Do NOT reintroduce them.
  SnippetManager(const SnippetManager &) = delete;
  SnippetManager &operator=(const SnippetManager &) = delete;
  SnippetManager(SnippetManager &&) = delete;
  SnippetManager &operator=(SnippetManager &&) = delete;

  // Set the locale used by the locale-aware built-in snippets (%ddd%, %dddd%,
  // %MMM%, %MMMM%). Accepts Qt/POSIX-style names ("en_US", "zh_CN", "ja");
  // unknown or empty resolves to English. Stores the canonical name.
  void SetLocale(const std::string &locale);
  const std::string &GetLocale() const;

  VxCoreError GetSnippetFolderPath(std::string &out_path) const;
  VxCoreError ListSnippets(std::vector<SnippetData> &out_snippets) const;
  VxCoreError GetSnippet(const std::string &name, SnippetData &out_snippet) const;
  VxCoreError CreateSnippet(const std::string &name, const std::string &json_content);
  VxCoreError DeleteSnippet(const std::string &name);
  VxCoreError RenameSnippet(const std::string &old_name, const std::string &new_name);
  VxCoreError UpdateSnippet(const std::string &name, const std::string &json_content);

  ApplyResult ApplySnippet(const std::string &name, const std::string &selected_text,
                           const std::string &indentation, const OverrideMap &overrides);
  // Expand arbitrary inline content (e.g. a template body): processes a top-level
  // "@@" cursor mark, "$$" selection mark, and nested %name% symbols. Unlike
  // ApplySnippet, when no "@@" cursor mark is present in |content| the returned
  // cursor_offset is -1 (NOT end-of-text).
  ApplyResult ExpandContent(const std::string &content, const std::string &selected_text,
                            const std::string &indentation, const OverrideMap &overrides);
  std::string ExpandSymbols(const std::string &content, const std::string &selected_text,
                            int &cursor_offset, const OverrideMap &overrides);

 private:
  // Shared tail used by ApplySnippet and ExpandContent: applies raw marks then
  // chains ExpandSymbols. When |detect_cursor_mark| is true and |content| does
  // not contain |cursor_mark|, the returned cursor_offset is -1.
  ApplyResult ApplyMarksAndExpand(const std::string &content, const std::string &selected_text,
                                  const std::string &indentation, const std::string &cursor_mark,
                                  const std::string &selection_mark, bool indent_first,
                                  const OverrideMap &overrides, bool detect_cursor_mark);

  VxCoreError EnsureSnippetFolderExists() const;
  bool IsValidSnippetName(const std::string &name) const;
  void LoadBuiltInSnippets();
  const SnippetData *FindSnippet(const std::string &name) const;
  std::string GetSnippetFilePath(const std::string &name) const;

  ConfigManager *config_manager_ = nullptr;
  std::string snippet_folder_path_;
  std::string locale_;
  const DateTimeNames *names_ = nullptr;
  std::vector<SnippetData> builtin_snippets_;
  std::unordered_map<std::string, DynamicCallback> dynamic_callbacks_;
};

}  // namespace vxcore

#endif
