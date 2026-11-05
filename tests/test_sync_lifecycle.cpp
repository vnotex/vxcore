#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "test_utils.h"
#include "sync/sync_types.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"
#if defined(VXCORE_MOBILE_API) && VXCORE_MOBILE_API
#include "vxcore/mobile/vxcore_mobile.h"
#endif

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr const char *kOperation = "12345678-1234-4234-8234-123456789abc";
constexpr const char *kSecret = R"({"extra":{"username":"candidate","password":"private-test-secret"}})";
struct CoreString {
  char *value = nullptr;
  ~CoreString() { vxcore_string_free(value); }
};
struct Lifecycle {
  VxCoreSyncLifecycle *value = nullptr;
  ~Lifecycle() { vxcore_sync_free_lifecycle(value); }
  void Reset() { vxcore_sync_free_lifecycle(value); value = nullptr; }
};
struct Cancellation {
  VxCoreSyncCancellation *value = vxcore_sync_create_cancellation();
  ~Cancellation() { vxcore_sync_free_cancellation(value); }
};
struct Fixture {
  VxCoreContextHandle context = nullptr;
  std::string root;
  CoreString notebook;
  explicit Fixture(const char *name) : root(get_test_path(name)) { cleanup_test_dir(root); }
  ~Fixture() { vxcore_context_destroy(context); cleanup_test_dir(root); }
  VxCoreError Open() {
    auto code = vxcore_context_create(nullptr, &context);
    if (code != VXCORE_OK) return code;
    return vxcore_notebook_create(context, root.c_str(), R"({"name":"Lifecycle"})",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook.value);
  }
};
Json Command(const char *kind = "enable") {
  return {{"version", 1}, {"operationId", kOperation}, {"kind", kind},
          {"config", {{"backend", "webdav"}, {"remoteUrl", "https://127.0.0.1:1/notebook/"}}}};
}
std::string Bytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

VxCoreError RejectTrust(const char *, const uint8_t *const *, const uint64_t *, uint64_t, void *) {
  return VXCORE_ERR_SYNC_NETWORK;
}
int test_runtime_policy_and_clone_limits_are_not_portable() {
  vxcore::SyncConfig config;
  config.backend = "webdav";
  config.remote_url = "https://example.invalid/notebook/";
  int userdata = 1;
  config.tls_policy = {RejectTrust, &userdata};
  config.clone_limits = {1024, 4096, 100, 8, 512};
  config.private_clone = true;
  const auto json = config.ToJson();
  ASSERT_FALSE(json.contains("tlsPolicy"));
  ASSERT_FALSE(json.contains("cloneLimits"));
  ASSERT_FALSE(json.contains("limits"));
  ASSERT_FALSE(json.contains("privateClone"));
  const auto restored = vxcore::SyncConfig::FromJson(json);
  ASSERT_NULL(restored.tls_policy.verifier);
  ASSERT_NULL(restored.tls_policy.userdata);
  ASSERT_EQ(restored.clone_limits.max_file_bytes, uint64_t{0});
  ASSERT_EQ(restored.clone_limits.max_total_bytes, uint64_t{0});
  ASSERT_FALSE(restored.private_clone);
  return 0;
}

int test_closed_prepare_has_no_storage_effects() {
  Fixture fixture("sync_lifecycle_closed");
  ASSERT_EQ(fixture.Open(), VXCORE_OK);
  const auto config_path = utf8_to_fs_path(fixture.root) / "vx_notebook/config.json";
  const auto original = Bytes(config_path);
  for (const auto &invalid : {
           Json{{"version", 1}, {"operationId", kOperation}, {"kind", "erase"}},
           Json{{"version", 1}, {"operationId", "not-a-uuid"}, {"kind", "credentials"}},
           Json{{"version", 1}, {"operationId", kOperation}, {"kind", "credentials"},
                {"path", "state.json"}}}) {
    Lifecycle lifecycle;
    ASSERT_NE(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value,
                                            invalid.dump().c_str(), kSecret, &lifecycle.value), VXCORE_OK);
    ASSERT_NULL(lifecycle.value);
  }
  auto extra = Command();
  extra["config"]["password"] = "forbidden";
  Lifecycle lifecycle;
  ASSERT_NE(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value,
                                          extra.dump().c_str(), kSecret, &lifecycle.value), VXCORE_OK);
  ASSERT_NULL(lifecycle.value);
  ASSERT_EQ(Bytes(config_path), original);
  ASSERT_FALSE(fs::exists(utf8_to_fs_path(fixture.root) / "vx_notebook/vx_sync"));
  return 0;
}

int test_failed_prepare_preserves_tls_policy_mutability() {
  Fixture fixture("sync_lifecycle_failed_tls");
  ASSERT_EQ(fixture.Open(), VXCORE_OK);
  const auto command = Command().dump();
  // Valid command and JSON, but the transport rejects a Basic-auth username
  // containing ':'. This failure occurs after lifecycle lease admission.
  constexpr const char *invalid_secret =
      R"({"extra":{"username":"invalid:account","password":"secret"}})";
  Lifecycle failed;
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value, command.c_str(),
                                          invalid_secret, &failed.value), VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(failed.value);
  ASSERT_EQ(vxcore_sync_disable(fixture.context, fixture.notebook.value), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_set_tls_verifier(fixture.context, RejectTrust, nullptr), VXCORE_OK);
  // A failed retry must not unlock an already installed policy either.
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value, command.c_str(),
                                          invalid_secret, &failed.value), VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(failed.value);
  ASSERT_EQ(vxcore_sync_set_tls_verifier(fixture.context, nullptr, nullptr), VXCORE_ERR_INVALID_STATE);
  ASSERT_FALSE(fs::exists(utf8_to_fs_path(fixture.root) / "vx_notebook/vx_sync"));
  return 0;
}

int test_successful_handle_keeps_tls_policy_locked_after_free() {
  Fixture fixture("sync_lifecycle_successful_tls");
  ASSERT_EQ(fixture.Open(), VXCORE_OK);
  const auto command = Command().dump();
  Lifecycle lifecycle;
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value, command.c_str(),
                                          kSecret, &lifecycle.value), VXCORE_OK);
  lifecycle.Reset();
  ASSERT_EQ(vxcore_sync_disable(fixture.context, fixture.notebook.value), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_set_tls_verifier(fixture.context, RejectTrust, nullptr), VXCORE_ERR_INVALID_STATE);
  return 0;
}

int test_prepare_worker_cancel_complete_and_lease() {
  Fixture fixture("sync_lifecycle_cancel");
  ASSERT_EQ(fixture.Open(), VXCORE_OK);
  Lifecycle lifecycle;
  const auto command = Command().dump();
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value, command.c_str(),
                                          kSecret, &lifecycle.value), VXCORE_OK);
  ASSERT_NOT_NULL(lifecycle.value);
  Lifecycle competing;
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, fixture.notebook.value, command.c_str(),
                                          kSecret, &competing.value), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_NULL(competing.value);
  ASSERT_EQ(vxcore_sync_disable(fixture.context, fixture.notebook.value), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_sync_unregister_notebook(fixture.context, fixture.notebook.value), VXCORE_ERR_SYNC_IN_PROGRESS);
  const auto config = Command().at("config").dump();
  ASSERT_EQ(vxcore_sync_enable(fixture.context, fixture.notebook.value, config.c_str(), kSecret),
            VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_sync_set_tls_verifier(fixture.context, nullptr, nullptr), VXCORE_ERR_INVALID_STATE);
  CoreString early;
  ASSERT_EQ(vxcore_sync_complete_lifecycle(fixture.context, lifecycle.value, &early.value),
            VXCORE_ERR_INVALID_STATE);
  Cancellation cancellation;
  ASSERT_NOT_NULL(cancellation.value);
  vxcore_sync_cancel(cancellation.value);
  VxCoreError execution = VXCORE_ERR_UNKNOWN;
  std::thread worker([&] { execution = vxcore_sync_execute_lifecycle(lifecycle.value, cancellation.value); });
  worker.join();
  ASSERT_EQ(execution, VXCORE_ERR_CANCELLED);
  ASSERT_EQ(vxcore_sync_execute_lifecycle(lifecycle.value, nullptr), VXCORE_ERR_INVALID_STATE);
  CoreString result;
  ASSERT_EQ(vxcore_sync_complete_lifecycle(fixture.context, lifecycle.value, &result.value),
            VXCORE_ERR_CANCELLED);
  const auto summary = Json::parse(result.value);
  ASSERT_TRUE(summary.at("executed").get<bool>());
  ASSERT_FALSE(summary.at("committed").get<bool>());
  ASSERT_FALSE(summary.at("cleanupRequired").get<bool>());
  ASSERT_EQ(summary.at("code").get<int>(), VXCORE_ERR_CANCELLED);
  ASSERT_EQ(summary.at("notebookId").get<std::string>(), fixture.notebook.value);
  ASSERT_TRUE(std::string(result.value).find("private-test-secret") == std::string::npos);
  CoreString duplicate;
  ASSERT_EQ(vxcore_sync_complete_lifecycle(fixture.context, lifecycle.value, &duplicate.value),
            VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(vxcore_sync_disable(fixture.context, fixture.notebook.value), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_sync_unregister_notebook(fixture.context, fixture.notebook.value), VXCORE_ERR_SYNC_IN_PROGRESS);
  lifecycle.Reset();
  ASSERT_EQ(vxcore_sync_disable(fixture.context, fixture.notebook.value), VXCORE_OK);
  ASSERT_FALSE(fs::exists(utf8_to_fs_path(fixture.root) / "vx_notebook/vx_sync"));
  return 0;
}

int test_refresh_reports_actual_reload_and_all_error_outputs() {
  int32_t refresh = 777;
  ASSERT_EQ(vxcore_sync_refresh_notebook_result(nullptr, "missing", &refresh), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(refresh, VXCORE_ERR_NULL_POINTER);
  Fixture fixture("sync_lifecycle_refresh");
  ASSERT_EQ(fixture.Open(), VXCORE_OK);
  refresh = 777;
  ASSERT_EQ(vxcore_sync_refresh_notebook_result(fixture.context, "missing", &refresh), VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(refresh, VXCORE_ERR_NOT_FOUND);
  const auto path = utf8_to_fs_path(fixture.root) / "vx_notebook/config.json";
  auto config = Json::parse(Bytes(path));
  config["name"] = "Reloaded without in-memory phase";
  write_file(fs_path_to_utf8(path), config.dump());
  ASSERT_EQ(vxcore_sync_refresh_notebook_result(fixture.context, fixture.notebook.value, &refresh), VXCORE_OK);
  ASSERT_EQ(refresh, VXCORE_OK);
  CoreString observed;
  ASSERT_EQ(vxcore_notebook_get_config(fixture.context, fixture.notebook.value, &observed.value), VXCORE_OK);
  ASSERT_EQ(Json::parse(observed.value).at("name"), config.at("name"));
  write_file(fs_path_to_utf8(path), "{broken");
  ASSERT_EQ(vxcore_sync_refresh_notebook_result(fixture.context, fixture.notebook.value, &refresh),
            VXCORE_ERR_JSON_PARSE);
  ASSERT_EQ(refresh, VXCORE_ERR_JSON_PARSE);
  write_file(fs_path_to_utf8(path), config.dump());
  ASSERT_EQ(vxcore_sync_refresh_notebook(fixture.context, fixture.notebook.value), VXCORE_OK);
  return 0;
}

Json CloneCommand(const fs::path &target) {
  auto command = Command("clone");
  command["target"] = fs_path_to_utf8(target);
  command["limits"] = {{"maxFileBytes", 1024}, {"maxTotalBytes", 8192},
                        {"maxEntries", 100}, {"maxDepth", 16}, {"maxPathBytes", 1024}};
  return command;
}
int test_clone_refuses_shared_context() {
  Fixture fixture("sync_lifecycle_shared_clone");
  ASSERT_EQ(fixture.Open(), VXCORE_OK);
  const auto target = utf8_to_fs_path(fixture.root) / "imports" / kOperation / "unpacked";
  fs::create_directories(target);
  Lifecycle lifecycle;
  const auto command = CloneCommand(target).dump();
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, nullptr, command.c_str(), kSecret,
                                          &lifecycle.value), VXCORE_ERR_INVALID_STATE);
  ASSERT_NULL(lifecycle.value);
  ASSERT_TRUE(fs::is_empty(target));
  return 0;
}

#if defined(_WIN32) && defined(VXCORE_MOBILE_API) && VXCORE_MOBILE_API
int test_private_clone_refuses_unregistrable_windows_paths_before_worker() {
  const fs::path fixed_paths[] = {
      fs::path("unpacked") / "vx_notebook" / "config.json",
      fs::path("runtime") / "local" / "notebooks" / kOperation / "metadata.db-journal",
      fs::path("runtime") / "local" / "vxsession.json"};
  for (const auto &fixed : fixed_paths) {
    Fixture fixture("sync_clone_registration_limit");
    const auto base = utf8_to_fs_path(fixture.root);
    const auto shortest = base / "x" / "imports" / kOperation / fixed;
    ASSERT_TRUE(shortest.native().size() < 260);
    const auto padding = std::string(261 - shortest.native().size(), 'p');
    const auto wrapper = base / padding / "imports" / kOperation;
    const auto target = wrapper / "unpacked";
    const auto local = wrapper / "runtime" / "local";
    ASSERT_TRUE(target.native().size() < 260);
    ASSERT_EQ((wrapper / fixed).native().size(), size_t{260});
    vxcore::private_io::CreateDirectories(target);
    const auto app_path = fs_path_to_utf8(wrapper / "runtime" / "app");
    const auto local_path = fs_path_to_utf8(local);
    ASSERT_EQ(vxcore_context_create_with_paths(nullptr, app_path.c_str(), local_path.c_str(),
                                               &fixture.context), VXCORE_OK);
    vxcore::FileIdentity before, after;
    ASSERT_EQ(vxcore::GetFileIdentity(target, before, vxcore::FileAccess::kPrivate), VXCORE_OK);
    Lifecycle lifecycle;
    const auto command = CloneCommand(target).dump();
    ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, nullptr, command.c_str(), kSecret,
                                            &lifecycle.value), VXCORE_ERR_UNSUPPORTED);
    ASSERT_NULL(lifecycle.value);  // No executable handle, so no worker can start.
    ASSERT_TRUE(vxcore::private_io::IsEmpty(target));
    ASSERT_EQ(vxcore::GetFileIdentity(target, after, vxcore::FileAccess::kPrivate), VXCORE_OK);
    ASSERT_TRUE(before.SameObject(after));
    CoreString notebooks;
    ASSERT_EQ(vxcore_notebook_list(fixture.context, &notebooks.value), VXCORE_OK);
    ASSERT_TRUE(Json::parse(notebooks.value).empty());
    // Refusal precedes a successful handle/TLS lock and leaves startup policy mutable.
    ASSERT_EQ(vxcore_sync_set_tls_verifier(fixture.context, RejectTrust, nullptr), VXCORE_OK);
  }
  return 0;
}

int test_private_clone_accepts_last_supported_windows_registration_length() {
  Fixture fixture("sync_clone_registration_fits");
  const auto base = utf8_to_fs_path(fixture.root);
  const auto journal = fs::path("runtime") / "local" / "notebooks" / kOperation /
                       "metadata.db-journal";
  const auto shortest = base / "x" / "imports" / kOperation / journal;
  ASSERT_TRUE(shortest.native().size() < 259);
  const auto padding = std::string(260 - shortest.native().size(), 'p');
  const auto wrapper = base / padding / "imports" / kOperation;
  const auto target = wrapper / "unpacked";
  ASSERT_EQ((wrapper / journal).native().size(), size_t{259});
  ASSERT_TRUE((target / "vx_notebook" / "config.json").native().size() < 260);
  ASSERT_TRUE((wrapper / "runtime" / "local" / "vxsession.json").native().size() < 260);
  vxcore::private_io::CreateDirectories(target);
  const auto app = fs_path_to_utf8(wrapper / "runtime" / "app");
  const auto local = fs_path_to_utf8(wrapper / "runtime" / "local");
  ASSERT_EQ(vxcore_context_create_with_paths(nullptr, app.c_str(), local.c_str(), &fixture.context), VXCORE_OK);
  Lifecycle lifecycle;
  const auto command = CloneCommand(target).dump();
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, nullptr, command.c_str(), kSecret,
                                          &lifecycle.value), VXCORE_OK);
  ASSERT_NOT_NULL(lifecycle.value);
  ASSERT_TRUE(vxcore::private_io::IsEmpty(target));
  return 0;
}
#endif

#if defined(VXCORE_MOBILE_API) && VXCORE_MOBILE_API
int test_private_clone_precancel_does_not_register() {
  Fixture fixture("sync_lifecycle_private_clone");
  const auto wrapper = utf8_to_fs_path(fixture.root) / "imports" / kOperation;
  const auto target = wrapper / "unpacked";
  fs::create_directories(target);
  const auto app = fs_path_to_utf8(wrapper / "runtime/app");
  const auto local = fs_path_to_utf8(wrapper / "runtime/local");
  ASSERT_EQ(vxcore_context_create_with_paths(nullptr, app.c_str(), local.c_str(), &fixture.context), VXCORE_OK);
  Lifecycle lifecycle;
  const auto command = CloneCommand(target).dump();
  ASSERT_EQ(vxcore_sync_prepare_lifecycle(fixture.context, nullptr, command.c_str(), kSecret,
                                          &lifecycle.value), VXCORE_OK);
  Cancellation cancellation;
  vxcore_sync_cancel(cancellation.value);
  VxCoreError execution = VXCORE_ERR_UNKNOWN;
  std::thread worker([&] { execution = vxcore_sync_execute_lifecycle(lifecycle.value, cancellation.value); });
  worker.join();
  ASSERT_EQ(execution, VXCORE_ERR_CANCELLED);
  CoreString result;
  ASSERT_EQ(vxcore_sync_complete_lifecycle(fixture.context, lifecycle.value, &result.value), VXCORE_ERR_CANCELLED);
  ASSERT_FALSE(Json::parse(result.value).at("committed").get<bool>());
  CoreString notebooks;
  ASSERT_EQ(vxcore_notebook_list(fixture.context, &notebooks.value), VXCORE_OK);
  ASSERT_TRUE(Json::parse(notebooks.value).empty());
  ASSERT_TRUE(fs::is_empty(target));
  return 0;
}
#endif
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_runtime_policy_and_clone_limits_are_not_portable);
  RUN_TEST(test_closed_prepare_has_no_storage_effects);
  RUN_TEST(test_failed_prepare_preserves_tls_policy_mutability);
  RUN_TEST(test_successful_handle_keeps_tls_policy_locked_after_free);
  RUN_TEST(test_prepare_worker_cancel_complete_and_lease);
  RUN_TEST(test_refresh_reports_actual_reload_and_all_error_outputs);
  RUN_TEST(test_clone_refuses_shared_context);
#if defined(_WIN32) && defined(VXCORE_MOBILE_API) && VXCORE_MOBILE_API
  RUN_TEST(test_private_clone_refuses_unregistrable_windows_paths_before_worker);
  RUN_TEST(test_private_clone_accepts_last_supported_windows_registration_length);
#endif
#if defined(VXCORE_MOBILE_API) && VXCORE_MOBILE_API
  RUN_TEST(test_private_clone_precancel_does_not_register);
#endif
  return 0;
}
