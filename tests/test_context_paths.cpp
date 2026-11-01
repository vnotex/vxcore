#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <vector>

#include "test_utils.h"
#include "vxcore/mobile/vxcore_mobile.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;

struct TempLeaf {
  fs::path path;
  explicit TempLeaf(const char *name) {
    static unsigned int sequence = 0;
    const auto parent = utf8_to_fs_path(get_test_path("context_paths"));
    fs::create_directories(parent);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    do {
      path = parent / (std::string(name) + "-" + std::to_string(stamp) + "-" +
                       std::to_string(++sequence));
    } while (!fs::create_directory(path));
  }
  ~TempLeaf() { std::error_code ec; fs::remove_all(path, ec); }
};

struct Context {
  VxCoreContextHandle value = nullptr;
  ~Context() { vxcore_context_destroy(value); }
  void Reset() { vxcore_context_destroy(value); value = nullptr; }
};

struct CoreString {
  char *value = nullptr;
  ~CoreString() { vxcore_string_free(value); }
};

VxCoreError Open(Context &context, const fs::path &app, const fs::path &local) {
  return vxcore_context_create_with_paths(nullptr, fs_path_to_utf8(app).c_str(),
                                          fs_path_to_utf8(local).c_str(), &context.value);
}

int ExpectFailure(const char *app, const char *local, VxCoreError expected) {
  auto output = reinterpret_cast<VxCoreContextHandle>(static_cast<uintptr_t>(1));
  const auto error = vxcore_context_create_with_paths(nullptr, app, local, &output);
  ASSERT_EQ(error, expected);
  ASSERT_NULL(output);
  return 0;
}

int test_config_session_and_owned_roots() {
  TempLeaf leaf("roundtrip");
  const auto app = leaf.path / utf8_to_fs_path("设置") / "app";
  const auto local = leaf.path / utf8_to_fs_path("本机") / "local";
  std::string app_input = fs_path_to_utf8(app);
  std::string local_input = fs_path_to_utf8(local);
  Context context;
  ASSERT_EQ(vxcore_context_create_with_paths("ignored, not JSON", app_input.c_str(),
                                             local_input.c_str(), &context.value), VXCORE_OK);
  app_input.assign("overwritten caller storage");
  local_input.clear();
  CoreString app_result, local_result, config_path, session_path;
  ASSERT_EQ(vxcore_context_get_data_path(context.value, VXCORE_DATA_APP, &app_result.value), VXCORE_OK);
  ASSERT_EQ(vxcore_context_get_data_path(context.value, VXCORE_DATA_LOCAL, &local_result.value), VXCORE_OK);
  ASSERT_TRUE(fs::equivalent(utf8_to_fs_path(app_result.value), app));
  ASSERT_TRUE(fs::equivalent(utf8_to_fs_path(local_result.value), local));
  ASSERT_EQ(vxcore_context_get_config_path(context.value, &config_path.value), VXCORE_OK);
  ASSERT_EQ(vxcore_context_get_session_config_path(context.value, &session_path.value), VXCORE_OK);
  ASSERT_TRUE(fs::equivalent(utf8_to_fs_path(config_path.value).parent_path(), app));
  ASSERT_TRUE(fs::equivalent(utf8_to_fs_path(session_path.value).parent_path(), local));
  ASSERT_EQ(vxcore_context_update_config(context.value,
      R"({"recoverLastSession":false,"autoSyncDebounceSeconds":17})"), VXCORE_OK);
  ASSERT_EQ(vxcore_context_update_config_by_name(context.value, VXCORE_DATA_APP, "same",
                                               "app value"), VXCORE_OK);
  ASSERT_EQ(vxcore_context_update_config_by_name(context.value, VXCORE_DATA_LOCAL, "same",
                                               "local value"), VXCORE_OK);
  CoreString notebook;
  const auto notebook_path = fs_path_to_utf8(leaf.path / "notebook");
  ASSERT_EQ(vxcore_notebook_create(context.value, notebook_path.c_str(), "{\"name\":\"Paths\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook.value), VXCORE_OK);
  ASSERT_EQ(vxcore_prepare_shutdown(context.value), VXCORE_OK);
  ASSERT_TRUE(fs::is_regular_file(app / "vxcore.json"));
  ASSERT_TRUE(fs::is_regular_file(local / "vxsession.json"));
  ASSERT_FALSE(fs::exists(local / "vxcore.json"));
  ASSERT_FALSE(fs::exists(app / "vxsession.json"));
  context.Reset();
  ASSERT_EQ(Open(context, app, local), VXCORE_OK);
  CoreString config, session, app_copy, local_copy;
  ASSERT_EQ(vxcore_context_get_config(context.value, &config.value), VXCORE_OK);
  const auto config_json = nlohmann::json::parse(config.value);
  ASSERT_EQ(config_json["recoverLastSession"], false);
  ASSERT_EQ(config_json["autoSyncDebounceSeconds"], 17);
  ASSERT_EQ(vxcore_context_get_session_config(context.value, &session.value), VXCORE_OK);
  const auto session_json = nlohmann::json::parse(session.value);
  ASSERT_EQ(session_json["notebooks"].size(), 1u);
  ASSERT_EQ(session_json["notebooks"][0]["id"], notebook.value);
  ASSERT_EQ(vxcore_context_get_config_by_name(context.value, VXCORE_DATA_APP, "same",
                                              &app_copy.value), VXCORE_OK);
  ASSERT_EQ(vxcore_context_get_config_by_name(context.value, VXCORE_DATA_LOCAL, "same",
                                              &local_copy.value), VXCORE_OK);
  ASSERT_EQ(std::string(app_copy.value), "app value");
  ASSERT_EQ(std::string(local_copy.value), "local value");
  return 0;
}

int test_two_contexts_are_isolated() {
  TempLeaf leaf("isolation");
  Context first, second;
  ASSERT_EQ(Open(first, leaf.path / "one-app", leaf.path / "one-local"), VXCORE_OK);
  ASSERT_EQ(Open(second, leaf.path / "two-app", leaf.path / "two-local"), VXCORE_OK);
  ASSERT_EQ(vxcore_context_update_config_by_name(first.value, VXCORE_DATA_APP, "owner", "one"), VXCORE_OK);
  ASSERT_EQ(vxcore_context_update_config_by_name(second.value, VXCORE_DATA_APP, "owner", "two"), VXCORE_OK);
  ASSERT_EQ(vxcore_context_update_config_by_name(first.value, VXCORE_DATA_LOCAL, "device", "first"), VXCORE_OK);
  CoreString a, b, missing;
  ASSERT_EQ(vxcore_context_get_config_by_name(first.value, VXCORE_DATA_APP, "owner", &a.value), VXCORE_OK);
  ASSERT_EQ(vxcore_context_get_config_by_name(second.value, VXCORE_DATA_APP, "owner", &b.value), VXCORE_OK);
  ASSERT_EQ(vxcore_context_get_config_by_name(second.value, VXCORE_DATA_LOCAL, "device", &missing.value),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(std::string(a.value), "one");
  ASSERT_EQ(std::string(b.value), "two");
  first.Reset();
  ASSERT_EQ(vxcore_context_update_config_by_name(second.value, VXCORE_DATA_LOCAL, "device", "second"), VXCORE_OK);
  ASSERT_EQ(Open(first, leaf.path / "one-app", leaf.path / "one-local"), VXCORE_OK);
  CoreString reopened;
  ASSERT_EQ(vxcore_context_get_config_by_name(first.value, VXCORE_DATA_LOCAL, "device", &reopened.value), VXCORE_OK);
  ASSERT_EQ(std::string(reopened.value), "first");
  return 0;
}

int test_invalid_paths_have_no_side_effects() {
  TempLeaf leaf("invalid");
  const auto app = fs_path_to_utf8(leaf.path / "not-created-app");
  const auto local = fs_path_to_utf8(leaf.path / "not-created-local");
  ASSERT_EQ(vxcore_context_create_with_paths(nullptr, app.c_str(), local.c_str(), nullptr),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(ExpectFailure(nullptr, local.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  ASSERT_EQ(ExpectFailure(app.c_str(), nullptr, VXCORE_ERR_INVALID_PARAM), 0);
  for (const std::string invalid : {"", "relative/path", "\xc0\xaf", "\x80", "\xe2\x82",
                                   "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80"}) {
    ASSERT_EQ(ExpectFailure(invalid.c_str(), local.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
    ASSERT_EQ(ExpectFailure(app.c_str(), invalid.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  }
  ASSERT_EQ(ExpectFailure(app.c_str(), app.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  const auto child = fs_path_to_utf8(utf8_to_fs_path(app) / "child");
  ASSERT_EQ(ExpectFailure(app.c_str(), child.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  ASSERT_EQ(ExpectFailure(child.c_str(), app.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  const auto normalized = fs_path_to_utf8(leaf.path / "unused" / ".." / "not-created-app");
  ASSERT_EQ(ExpectFailure(app.c_str(), normalized.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  ASSERT_TRUE(fs::is_empty(leaf.path));
  const auto file = leaf.path / "not-a-directory";
  write_file(fs_path_to_utf8(file), "unchanged");
  const auto file_string = fs_path_to_utf8(file);
  const auto file_child = fs_path_to_utf8(file / "child");
  ASSERT_EQ(ExpectFailure(app.c_str(), file_string.c_str(), VXCORE_ERR_IO), 0);
  ASSERT_EQ(ExpectFailure(app.c_str(), file_child.c_str(), VXCORE_ERR_IO), 0);
  ASSERT_FALSE(fs::exists(utf8_to_fs_path(app)));
  ASSERT_FALSE(fs::exists(utf8_to_fs_path(local)));
  return 0;
}

int test_aliases_and_inaccessible_roots() {
  TempLeaf leaf("aliases");
  const auto target = leaf.path / "parent" / "LongExistingDirectory";
  fs::create_directories(target);
  const auto app = fs_path_to_utf8(target / "future");
  const auto alias = leaf.path / "alias";
  std::error_code ec;
  fs::create_directory_symlink(target, alias, ec);
  if (!ec) {
    const auto aliased = fs_path_to_utf8(alias / "future" / "child");
    ASSERT_EQ(ExpectFailure(app.c_str(), aliased.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
    // Resolve the symlink before '..', including a nonexistent final component.
    const auto via_parent = fs_path_to_utf8(alias / ".." / "LongExistingDirectory" / "future");
    ASSERT_EQ(ExpectFailure(app.c_str(), via_parent.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  } else {
#ifdef _WIN32
    ASSERT_TRUE(ec.value() == ERROR_PRIVILEGE_NOT_HELD || ec.value() == ERROR_NOT_SUPPORTED);
    std::cout << "Directory symlink case unavailable: " << ec.message() << '\n';
#else
    ASSERT_FALSE(ec);
#endif
  }
#ifdef _WIN32
  const auto upper = fs_path_to_utf8(target / "FUTURE");
  ASSERT_EQ(ExpectFailure(app.c_str(), upper.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  const auto extended = fs_path_to_utf8(fs::path(L"\\\\?\\" + target.native()) / "future");
  ASSERT_EQ(ExpectFailure(app.c_str(), extended.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  std::wstring short_path(32768, L'\0');
  const DWORD count = GetShortPathNameW(target.c_str(), short_path.data(),
                                       static_cast<DWORD>(short_path.size()));
  ASSERT_TRUE(count > 0 && count < short_path.size());
  short_path.resize(count);
  const auto short_alias = fs_path_to_utf8(fs::path(short_path) / "future");
  ASSERT_EQ(ExpectFailure(app.c_str(), short_alias.c_str(), VXCORE_ERR_INVALID_PARAM), 0);
  const auto locked = leaf.path / "inaccessible";
  fs::create_directory(locked);
  const HANDLE handle = CreateFileW(locked.c_str(), FILE_LIST_DIRECTORY, 0, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  auto output = reinterpret_cast<VxCoreContextHandle>(static_cast<uintptr_t>(1));
  const auto locked_error = vxcore_context_create_with_paths(nullptr, app.c_str(),
      fs_path_to_utf8(locked).c_str(), &output);
  CloseHandle(handle);
  ASSERT_EQ(locked_error, VXCORE_ERR_IO);
  ASSERT_NULL(output);
#else
  const auto locked = leaf.path / "inaccessible";
  fs::create_directory(locked);
  ASSERT_EQ(::chmod(locked.c_str(), 0), 0);
  auto output = reinterpret_cast<VxCoreContextHandle>(static_cast<uintptr_t>(1));
  const auto locked_error = vxcore_context_create_with_paths(nullptr, app.c_str(),
      fs_path_to_utf8(locked).c_str(), &output);
  const int restored = ::chmod(locked.c_str(), 0700);
  ASSERT_EQ(restored, 0);
  ASSERT_EQ(locked_error, VXCORE_ERR_IO);
  ASSERT_NULL(output);
#endif
  ASSERT_FALSE(fs::exists(target / "future"));
  return 0;
}

int test_load_failure_never_publishes_context() {
  TempLeaf leaf("invalid-config");
  const auto app = leaf.path / "app";
  const auto local = leaf.path / "local";
  fs::create_directory(app);
  fs::create_directory(local);
  write_file(fs_path_to_utf8(app / "vxcore.json"), "{ invalid");
  ASSERT_EQ(ExpectFailure(fs_path_to_utf8(app).c_str(), fs_path_to_utf8(local).c_str(),
                          VXCORE_ERR_JSON_PARSE), 0);
  ASSERT_FALSE(fs::exists(local / "vxsession.json"));
  write_file(fs_path_to_utf8(app / "vxcore.json"), "{}");
  write_file(fs_path_to_utf8(local / "vxsession.json"), "{ invalid");
  ASSERT_EQ(ExpectFailure(fs_path_to_utf8(app).c_str(), fs_path_to_utf8(local).c_str(),
                          VXCORE_ERR_JSON_PARSE), 0);
  return 0;
}

#ifndef _WIN32
int test_search_only_ancestor() {
  TempLeaf leaf("search-only");
  const auto ancestor = leaf.path / "ancestor";
  const auto support = ancestor / "support";
  fs::create_directories(support);
  ASSERT_EQ(::chmod(ancestor.c_str(), 0111), 0);
  Context context;
  const auto error = Open(context, support / "app", support / "local");
  const int restored = ::chmod(ancestor.c_str(), 0700);
  ASSERT_EQ(restored, 0);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_TRUE(fs::is_directory(support / "app"));
  ASSERT_TRUE(fs::is_directory(support / "local"));
  return 0;
}
#endif

using Snapshot = std::map<fs::path, std::pair<uintmax_t, fs::file_time_type>>;
Snapshot DefaultProfileSnapshot() {
  Snapshot result;
  const auto root = fs::temp_directory_path() / "vxcore_test_config";
  if (!fs::exists(root)) return result;
  result[root] = {0, fs::last_write_time(root)};
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    result[entry.path()] = {entry.is_regular_file() ? entry.file_size() : 0,
                            entry.last_write_time()};
  }
  return result;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  const auto defaults_before = DefaultProfileSnapshot();
  RUN_TEST(test_config_session_and_owned_roots);
  RUN_TEST(test_two_contexts_are_isolated);
  RUN_TEST(test_invalid_paths_have_no_side_effects);
  RUN_TEST(test_aliases_and_inaccessible_roots);
  RUN_TEST(test_load_failure_never_publishes_context);
#ifndef _WIN32
  RUN_TEST(test_search_only_ancestor);
#endif
  ASSERT_TRUE(DefaultProfileSnapshot() == defaults_before);
  return 0;
}
