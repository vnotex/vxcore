#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "core/context.h"
#include "sync/sync_backend.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

// Force-link the test_internals TU so its anonymous-namespace
// BackendRegistration token runs at static-init time. Without this reference,
// MSVC's linker would drop the entire mock_sync_backend.cpp TU from the static
// library and the "mock" backend would never be registered in
// SyncBackendRegistry::Instance(), causing vxcore_sync_enable(...,"mock",...)
// to fail with VXCORE_ERR_UNKNOWN_BACKEND.
namespace {
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();
}  // namespace

namespace {

// GetCurrentTimestampMillis() lives inside the vxcore DLL and is not exported
// across the DLL boundary, so we re-derive the same value here using the same
// units (std::chrono::milliseconds since Unix epoch) for the bounds check in
// test_last_sync_utc_persistence.
int64_t TestNowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Minimal in-memory ISyncBackend stub: every operation succeeds, no real I/O.
// Used to drive SyncManager::TriggerSync through a known-OK path so we can
// observe the side effect that persists last_sync_utc.
class MockSyncBackend : public vxcore::ISyncBackend {
 public:
  // Task 4.1 identity methods (added so this local stub implements the full
  // ISyncBackend surface; Task 4.2 leaves these untouched).
  std::string GetName() const override { return "mock"; }
  vxcore::SyncCapabilities GetCapabilities() const override { return 0; }
  bool IsInitialized() const override { return true; }
  VxCoreError Initialize(const std::string &, const vxcore::SyncConfig &) override {
    return VXCORE_OK;
  }
  VxCoreError Sync(vxcore::SyncProgressCallback, void *) override { return VXCORE_OK; }
  VxCoreError GetStatus(std::vector<vxcore::SyncFileInfo> &) override { return VXCORE_OK; }
  VxCoreError GetConflicts(std::vector<vxcore::SyncConflictInfo> &) override { return VXCORE_OK; }
  VxCoreError ResolveConflict(const std::string &,
                              vxcore::SyncConflictResolution) override {
    return VXCORE_OK;
  }
};

}  // namespace

int test_sync_error_codes() {
  std::cout << "  Running test_sync_error_codes..." << std::endl;

  ASSERT_EQ(VXCORE_ERR_SYNC_IN_PROGRESS, 21);
  ASSERT_EQ(VXCORE_ERR_SYNC_CONFLICT, 22);
  ASSERT_EQ(VXCORE_ERR_SYNC_AUTH_FAILED, 23);
  ASSERT_EQ(VXCORE_ERR_SYNC_NETWORK, 24);
  ASSERT_EQ(VXCORE_ERR_SYNC_NOT_ENABLED, 25);

  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_SYNC_IN_PROGRESS));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_SYNC_CONFLICT));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_SYNC_AUTH_FAILED));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_SYNC_NETWORK));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_SYNC_NOT_ENABLED));

  std::cout << "  \xE2\x9C\x93 test_sync_error_codes passed" << std::endl;
  return 0;
}

int test_sync_enable_disable() {
  std::cout << "  Running test_sync_enable_disable..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_enable"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_sync_enable").c_str(),
                               "{\"name\":\"Sync Enable Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

   // Task 5.2 (F4.1): "mock" is self-registered via BackendRegistration in
   // test_internals. The force-link reference at the top of this file ensures
   // the registration TU is pulled into the test binary, so the public C API
   // can enable the mock backend directly.
   err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}", nullptr);
   ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_disable(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_enable"));
  std::cout << "  \xE2\x9C\x93 test_sync_enable_disable passed" << std::endl;
  return 0;
}

int test_sync_status_not_enabled() {
  std::cout << "  Running test_sync_status_not_enabled..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_status"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_sync_status").c_str(),
                               "{\"name\":\"Sync Status Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  char *status_json = nullptr;
  err = vxcore_sync_get_status(ctx, notebook_id, &status_json);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_NOT_ENABLED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_status"));
  std::cout << "  \xE2\x9C\x93 test_sync_status_not_enabled passed" << std::endl;
  return 0;
}

// Verifies that vxcore_context_create() registers built-in backends via
// RegisterBuiltinBackends(). Replaces the deleted test_git_backend_autoreg —
// the explicit registration path replaces the old static-init dance.
int test_sync_builtin_git_registered() {
  std::cout << "  Running test_sync_builtin_git_registered..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  auto names = vxcore::SyncBackendRegistry::Instance().Names();
  bool has_git = std::find(names.begin(), names.end(), std::string("git")) !=
                 names.end();
  ASSERT_TRUE(has_git);

  vxcore_context_destroy(ctx);
  std::cout << "  \xE2\x9C\x93 test_sync_builtin_git_registered passed" << std::endl;
  return 0;
}

int test_sync_trigger_not_implemented() {
  std::cout << "  Running test_sync_trigger_not_implemented..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_trigger"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_sync_trigger").c_str(),
                               "{\"name\":\"Sync Trigger Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Wave 4.4 / F1.1: "mock" is no longer an allow-listed placeholder backend; without a
  // factory in the registry, enable would fail. With no backend ever enabled, trigger
  // reports SYNC_NOT_ENABLED. The "enabled but no backend → NOT_IMPLEMENTED" path is
  // covered by test_sync_trigger_no_backend_returns_not_implemented in
  // test_sync_manager_dispatch.cpp (which uses C++-side EnableSync paths).
  err = vxcore_sync_trigger(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_NOT_ENABLED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_trigger"));
  std::cout << "  \xE2\x9C\x93 test_sync_trigger_not_implemented passed" << std::endl;
  return 0;
}

int test_sync_raw_notebook_rejected() {
  std::cout << "  Running test_sync_raw_notebook_rejected..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_raw"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_sync_raw").c_str(),
                               "{\"name\":\"Sync Raw Test\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_raw"));
  std::cout << "  \xE2\x9C\x93 test_sync_raw_notebook_rejected passed" << std::endl;
  return 0;
}

int test_sync_nonexistent_notebook() {
  std::cout << "  Running test_sync_nonexistent_notebook..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_enable(ctx, "nonexistent-id", "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_context_destroy(ctx);
  std::cout << "  \xE2\x9C\x93 test_sync_nonexistent_notebook passed" << std::endl;
  return 0;
}

int test_notebook_config_sync_fields() {
  std::cout << "  Running test_notebook_config_sync_fields..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_config"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_sync_config").c_str(),
                               "{\"name\":\"Sync Config Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Verify defaults
  char *config_json = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_EQ(config["syncEnabled"].get<bool>(), false);
  ASSERT_EQ(config["syncBackend"].get<std::string>(), "");
  ASSERT_EQ(config["syncIntervalSeconds"].get<int>(), 60);
  vxcore_string_free(config_json);

  // Update with sync fields
  err = vxcore_notebook_update_config(
      ctx, notebook_id,
      "{\"syncEnabled\":true,\"syncBackend\":\"git\","
      "\"syncRemoteUrl\":\"https://example.com/repo.git\",\"syncIntervalSeconds\":120}");
  ASSERT_EQ(err, VXCORE_OK);

  // Read back and verify
  char *updated_json = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &updated_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(updated_json);

  nlohmann::json updated = nlohmann::json::parse(updated_json);
  ASSERT_EQ(updated["syncEnabled"].get<bool>(), true);
  ASSERT_EQ(updated["syncBackend"].get<std::string>(), "git");
  ASSERT_EQ(updated["syncRemoteUrl"].get<std::string>(), "https://example.com/repo.git");
  ASSERT_EQ(updated["syncIntervalSeconds"].get<int>(), 120);
  vxcore_string_free(updated_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_config"));
  std::cout << "  \xE2\x9C\x93 test_notebook_config_sync_fields passed" << std::endl;
  return 0;
}

int test_sync_config_from_json_with_backend_options() {
  std::cout << "  Running test_sync_config_from_json_with_backend_options..." << std::endl;
  nlohmann::json j = {
      {"backend", "webdav"},
      {"remoteUrl", "https://example.com/dav"},
      {"intervalSeconds", 120},
      {"backendOptions", {{"authHeader", "Bearer token123"}, {"depth", 1}}}};
  auto config = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(config.backend, "webdav");
  ASSERT_EQ(config.remote_url, "https://example.com/dav");
  ASSERT_EQ(config.interval_seconds, 120);
  ASSERT_TRUE(config.backend_options.is_object());
  ASSERT_EQ(config.backend_options["authHeader"], "Bearer token123");
  ASSERT_EQ(config.backend_options["depth"], 1);
  std::cout << "  \xE2\x9C\x93 test_sync_config_from_json_with_backend_options passed" << std::endl;
  return 0;
}

int test_sync_config_to_json_includes_backend_options() {
  std::cout << "  Running test_sync_config_to_json_includes_backend_options..." << std::endl;
  vxcore::SyncConfig config;
  config.backend = "s3";
  config.remote_url = "s3://bucket";
  config.backend_options = {{"region", "us-east-1"}, {"prefix", "notebooks/"}};
  auto j = config.ToJson();
  ASSERT_TRUE(j.contains("backendOptions"));
  ASSERT_EQ(j["backendOptions"]["region"], "us-east-1");
  ASSERT_EQ(j["backendOptions"]["prefix"], "notebooks/");
  std::cout << "  \xE2\x9C\x93 test_sync_config_to_json_includes_backend_options passed" << std::endl;
  return 0;
}

int test_sync_config_roundtrip_backend_options() {
  std::cout << "  Running test_sync_config_roundtrip_backend_options..." << std::endl;
  vxcore::SyncConfig original;
  original.backend = "custom";
  original.remote_url = "custom://host";
  original.interval_seconds = 60;
  original.backend_options = {{"nested", {{"key", "value"}}}, {"list", {1, 2, 3}}};
  auto j = original.ToJson();
  auto restored = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(restored.backend, original.backend);
  ASSERT_EQ(restored.remote_url, original.remote_url);
  ASSERT_EQ(restored.interval_seconds, original.interval_seconds);
  ASSERT_EQ(restored.backend_options, original.backend_options);
  std::cout << "  \xE2\x9C\x93 test_sync_config_roundtrip_backend_options passed" << std::endl;
  return 0;
}

int test_sync_config_from_json_without_backend_options() {
  std::cout << "  Running test_sync_config_from_json_without_backend_options..." << std::endl;
  nlohmann::json j = {{"backend", "git"}, {"remoteUrl", "https://github.com/user/repo.git"}};
  auto config = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(config.backend, "git");
  ASSERT_TRUE(config.backend_options.is_null());
  std::cout << "  \xE2\x9C\x93 test_sync_config_from_json_without_backend_options passed" << std::endl;
  return 0;
}

int test_sync_credentials_extra_field() {
  std::cout << "  Running test_sync_credentials_extra_field..." << std::endl;
  vxcore::SyncCredentials creds;
  creds.personal_access_token = "ghp_test";
  creds.extra = {{"access_key_id", "AKIA..."}, {"secret_access_key", "secret"}};
  ASSERT_TRUE(creds.extra.is_object());
  ASSERT_EQ(creds.extra["access_key_id"], "AKIA...");
  std::cout << "  \xE2\x9C\x93 test_sync_credentials_extra_field passed" << std::endl;
  return 0;
}

int test_sync_enable_with_backend_options_via_c_api() {
  std::cout << "  Running test_sync_enable_with_backend_options_via_c_api..." << std::endl;
  cleanup_test_dir(get_test_path("test_sync_bo"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_sync_bo").c_str(),
                               "{\"name\":\"BackendOptionsTest\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable sync with backendOptions. Wave 4.4 / F1.1: unknown backends fail-fast with
  // UNKNOWN_BACKEND. The C API still parses the JSON (no parse error returned) — only
  // the registry lookup fails. This preserves coverage of the JSON parsing path.
  // (Note: "mock" is self-registered, so we use a name guaranteed not in the registry.)
  const char *config_json =
      R"({"backend":"nonexistent_backend_xyz","remoteUrl":"https://dav.example.com","backendOptions":{"depth":2}})";
  err = vxcore_sync_enable(ctx, notebook_id, config_json, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_sync_bo"));
  std::cout << "  \xE2\x9C\x93 test_sync_enable_with_backend_options_via_c_api passed" << std::endl;
  return 0;
}

int test_sync_config_to_json_omits_empty_backend_options() {
  std::cout << "  Running test_sync_config_to_json_omits_empty_backend_options..." << std::endl;
  vxcore::SyncConfig config;
  config.backend = "git";
  auto j = config.ToJson();
  ASSERT_FALSE(j.contains("backendOptions"));
  std::cout << "  \xE2\x9C\x93 test_sync_config_to_json_omits_empty_backend_options passed"
            << std::endl;
  return 0;
}

int test_last_sync_utc_persistence() {
  std::cout << "  Running test_last_sync_utc_persistence..." << std::endl;
  const std::string root = get_test_path("test_last_sync_utc");
  cleanup_test_dir(root);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(),
                                   "{\"name\":\"LastSyncUtc Test\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Before any sync: should be 0 (never synced on this device).
  int64_t before_utc = -1;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, notebook_id, &before_utc), VXCORE_OK);
  ASSERT_EQ(before_utc, 0);

   // Task 5.2 (F4.1): "mock" is self-registered via BackendRegistration in
   // test_internals. The force-link reference at the top of this file ensures
   // the registration TU is pulled into the test binary.
   ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}", nullptr),
             VXCORE_OK);

   const int64_t before_trigger = TestNowMillis();
   ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_OK);
   const int64_t after_trigger = TestNowMillis();

   // After successful trigger: should be a recent timestamp within the window.
   int64_t after_utc = 0;
   ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, notebook_id, &after_utc), VXCORE_OK);
   ASSERT_TRUE(after_utc >= before_trigger);
   ASSERT_TRUE(after_utc <= after_trigger);

  // Null-pointer guards.
  int64_t dummy = 0;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(nullptr, notebook_id, &dummy),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, nullptr, &dummy), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, notebook_id, nullptr),
            VXCORE_ERR_NULL_POINTER);

  // Not-found case: unknown notebook id returns NOT_FOUND and zeros the out param.
  int64_t nf = 42;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, "nonexistent-nb-id", &nf),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(nf, 0);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(root);
  std::cout << "  \xE2\x9C\x93 test_last_sync_utc_persistence passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// T7 (sync-queue-convergence): TriggerSync now emits sync.started and
// sync.finished events directly (single source). The lambda inside
// MaybeEnqueueSync used to emit them; that duplicate emission was removed.
// ----------------------------------------------------------------------------

namespace {

struct SyncEventCounters {
  std::atomic<int> started{0};
  std::atomic<int> finished{0};
  std::string last_finished_notebook;
  int last_finished_result = -999;
  std::string last_started_notebook;
  std::mutex mu;
};

void started_cb(const char *event_name, const char *json_data, void *userdata) {
  (void)event_name;
  auto *c = static_cast<SyncEventCounters *>(userdata);
  auto j = nlohmann::json::parse(json_data);
  {
    std::lock_guard<std::mutex> lk(c->mu);
    c->last_started_notebook = j.value("notebookId", "");
  }
  c->started.fetch_add(1);
}

void finished_cb(const char *event_name, const char *json_data, void *userdata) {
  (void)event_name;
  auto *c = static_cast<SyncEventCounters *>(userdata);
  auto j = nlohmann::json::parse(json_data);
  {
    std::lock_guard<std::mutex> lk(c->mu);
    c->last_finished_notebook = j.value("notebookId", "");
    c->last_finished_result = j.value("result", -999);
  }
  c->finished.fetch_add(1);
}

}  // namespace

int test_trigger_sync_emits_started_and_finished() {
  std::cout << "  Running test_trigger_sync_emits_started_and_finished..." << std::endl;
  cleanup_test_dir(get_test_path("test_trigger_events"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_trigger_events").c_str(),
                                   "{\"name\":\"TriggerEvents\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  SyncEventCounters counters;
  ASSERT_EQ(vxcore_on_event(ctx, "sync.started", started_cb, &counters), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(ctx, "sync.finished", finished_cb, &counters), VXCORE_OK);

  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                               nullptr),
            VXCORE_OK);

  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_OK);

  ASSERT_EQ(counters.started.load(), 1);
  ASSERT_EQ(counters.finished.load(), 1);
  ASSERT_EQ(counters.last_started_notebook, std::string(notebook_id));
  ASSERT_EQ(counters.last_finished_notebook, std::string(notebook_id));
  ASSERT_EQ(counters.last_finished_result, static_cast<int>(VXCORE_OK));

  vxcore_off_event(ctx, "sync.started", started_cb);
  vxcore_off_event(ctx, "sync.finished", finished_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_trigger_events"));
  std::cout << "  \xE2\x9C\x93 test_trigger_sync_emits_started_and_finished passed" << std::endl;
  return 0;
}

int test_trigger_sync_emits_finished_with_error_code() {
  std::cout << "  Running test_trigger_sync_emits_finished_with_error_code..." << std::endl;
  cleanup_test_dir(get_test_path("test_trigger_err"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_trigger_err").c_str(),
                                   "{\"name\":\"TriggerErr\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  SyncEventCounters counters;
  ASSERT_EQ(vxcore_on_event(ctx, "sync.started", started_cb, &counters), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(ctx, "sync.finished", finished_cb, &counters), VXCORE_OK);

  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                               nullptr),
            VXCORE_OK);

  // Reach into the registered backend and force Sync() to return NETWORK.
  // BackendsForTesting is a thin accessor exposed on SyncManager for tests.
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto &backends = vctx->sync_manager->BackendsForTesting();
  auto it = backends.find(notebook_id);
  ASSERT_TRUE(it != backends.end());
  auto *mock = dynamic_cast<vxcore::MockSyncBackend *>(it->second.get());
  ASSERT_NOT_NULL(mock);
  mock->SetReturnCode("Sync", VXCORE_ERR_SYNC_NETWORK);

  VxCoreError err = vxcore_sync_trigger(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_NETWORK);

  ASSERT_EQ(counters.started.load(), 1);
  ASSERT_EQ(counters.finished.load(), 1);
  ASSERT_EQ(counters.last_finished_result, static_cast<int>(VXCORE_ERR_SYNC_NETWORK));

  vxcore_off_event(ctx, "sync.started", started_cb);
  vxcore_off_event(ctx, "sync.finished", finished_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_trigger_err"));
  std::cout << "  \xE2\x9C\x93 test_trigger_sync_emits_finished_with_error_code passed"
            << std::endl;
  return 0;
}

int test_auto_sync_does_not_double_emit() {
  std::cout << "  Running test_auto_sync_does_not_double_emit..." << std::endl;
  cleanup_test_dir(get_test_path("test_auto_no_dup"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_auto_no_dup").c_str(),
                                   "{\"name\":\"AutoNoDup\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  SyncEventCounters counters;
  ASSERT_EQ(vxcore_on_event(ctx, "sync.started", started_cb, &counters), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(ctx, "sync.finished", finished_cb, &counters), VXCORE_OK);

  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\","
                               "\"intervalSeconds\":1}",
                               nullptr),
            VXCORE_OK);

  // Trigger the auto path: create a file -> file.created -> mark_dirty ->
  // MaybeEnqueueSync -> emit sync.should_run. T9 (sync-queue-convergence)
  // removed the WorkQueue enqueue; the auto-route consumer (future T31)
  // subscribes to sync.should_run and drives TriggerSync. We install a
  // local subscriber here to mimic that future consumer.
  struct AutoRoute {
    VxCoreContextHandle ctx;
  } auto_route{ctx};
  auto auto_route_cb = [](const char *, const char *json_data, void *userdata) {
    auto *ar = static_cast<AutoRoute *>(userdata);
    auto j = nlohmann::json::parse(json_data);
    if (j.contains("notebookId") && j["notebookId"].is_string()) {
      vxcore_sync_trigger(ar->ctx, j["notebookId"].get<std::string>().c_str());
    }
  };
  ASSERT_EQ(vxcore_on_event(ctx, "sync.should_run", auto_route_cb, &auto_route), VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "auto_dup.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // After T9 the auto path emits exactly one sync.should_run; the local
  // subscriber drives a single TriggerSync, which (post-T7) emits exactly
  // one sync.started + one sync.finished.
  ASSERT_EQ(counters.started.load(), 1);
  ASSERT_EQ(counters.finished.load(), 1);

  vxcore_off_event(ctx, "sync.should_run", auto_route_cb);
  vxcore_off_event(ctx, "sync.started", started_cb);
  vxcore_off_event(ctx, "sync.finished", finished_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_auto_no_dup"));
  std::cout << "  \xE2\x9C\x93 test_auto_sync_does_not_double_emit passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// T8 (sync-queue-convergence): sync.conflict event now carries the conflict
// file list. TriggerSync is the sole emitter (both manual + auto paths).
// ----------------------------------------------------------------------------

namespace {

struct ConflictEventCapture {
  std::atomic<int> count{0};
  std::string last_notebook;
  std::vector<std::string> last_files;
  std::mutex mu;
};

void conflict_cb(const char *event_name, const char *json_data, void *userdata) {
  (void)event_name;
  auto *c = static_cast<ConflictEventCapture *>(userdata);
  auto j = nlohmann::json::parse(json_data);
  std::lock_guard<std::mutex> lk(c->mu);
  c->last_notebook = j.value("notebookId", "");
  c->last_files.clear();
  if (j.contains("files") && j["files"].is_array()) {
    for (const auto &f : j["files"]) {
      c->last_files.push_back(f.get<std::string>());
    }
  }
  c->count.fetch_add(1);
}

}  // namespace

int test_trigger_sync_emits_conflict_with_files() {
  std::cout << "  Running test_trigger_sync_emits_conflict_with_files..." << std::endl;
  cleanup_test_dir(get_test_path("test_conflict_files"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_conflict_files").c_str(),
                                   "{\"name\":\"ConflictFiles\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  SyncEventCounters counters;
  ConflictEventCapture conflict;
  ASSERT_EQ(vxcore_on_event(ctx, "sync.started", started_cb, &counters), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(ctx, "sync.finished", finished_cb, &counters), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(ctx, "sync.conflict", conflict_cb, &conflict), VXCORE_OK);

  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                               nullptr),
            VXCORE_OK);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto &backends = vctx->sync_manager->BackendsForTesting();
  auto it = backends.find(notebook_id);
  ASSERT_TRUE(it != backends.end());
  auto *mock = dynamic_cast<vxcore::MockSyncBackend *>(it->second.get());
  ASSERT_NOT_NULL(mock);

  std::vector<vxcore::SyncConflictInfo> fake;
  vxcore::SyncConflictInfo c1; c1.path = "notes/a.md"; fake.push_back(c1);
  vxcore::SyncConflictInfo c2; c2.path = "b.md"; fake.push_back(c2);
  mock->SetConflicts(fake);

  VxCoreError err = vxcore_sync_trigger(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_CONFLICT);

  ASSERT_EQ(conflict.count.load(), 1);
  ASSERT_EQ(conflict.last_notebook, std::string(notebook_id));
  ASSERT_EQ(conflict.last_files.size(), static_cast<size_t>(2));
  ASSERT_EQ(conflict.last_files[0], std::string("notes/a.md"));
  ASSERT_EQ(conflict.last_files[1], std::string("b.md"));
  ASSERT_EQ(counters.finished.load(), 1);
  ASSERT_EQ(counters.last_finished_result, static_cast<int>(VXCORE_ERR_SYNC_CONFLICT));

  vxcore_off_event(ctx, "sync.started", started_cb);
  vxcore_off_event(ctx, "sync.finished", finished_cb);
  vxcore_off_event(ctx, "sync.conflict", conflict_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_conflict_files"));
  std::cout << "  \xE2\x9C\x93 test_trigger_sync_emits_conflict_with_files passed" << std::endl;
  return 0;
}

int test_auto_sync_conflict_carries_files() {
  std::cout << "  Running test_auto_sync_conflict_carries_files..." << std::endl;
  cleanup_test_dir(get_test_path("test_auto_conflict"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_auto_conflict").c_str(),
                                   "{\"name\":\"AutoConflict\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  ConflictEventCapture conflict;
  ASSERT_EQ(vxcore_on_event(ctx, "sync.conflict", conflict_cb, &conflict), VXCORE_OK);

  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\","
                               "\"intervalSeconds\":1}",
                               nullptr),
            VXCORE_OK);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto &backends = vctx->sync_manager->BackendsForTesting();
  auto *mock = dynamic_cast<vxcore::MockSyncBackend *>(
      backends.find(notebook_id)->second.get());
  ASSERT_NOT_NULL(mock);

  std::vector<vxcore::SyncConflictInfo> fake;
  vxcore::SyncConflictInfo c1; c1.path = "x/y.md"; fake.push_back(c1);
  mock->SetConflicts(fake);

  // T9 (sync-queue-convergence): the auto path emits sync.should_run instead
  // of enqueueing into WorkQueue("sync"). Subscribe and drive TriggerSync
  // ourselves to mimic the future Qt auto-route consumer.
  struct AutoRoute {
    VxCoreContextHandle ctx;
  } auto_route{ctx};
  auto auto_route_cb = [](const char *, const char *json_data, void *userdata) {
    auto *ar = static_cast<AutoRoute *>(userdata);
    auto j = nlohmann::json::parse(json_data);
    if (j.contains("notebookId") && j["notebookId"].is_string()) {
      vxcore_sync_trigger(ar->ctx, j["notebookId"].get<std::string>().c_str());
    }
  };
  ASSERT_EQ(vxcore_on_event(ctx, "sync.should_run", auto_route_cb, &auto_route), VXCORE_OK);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "auto_conflict.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);

  ASSERT_EQ(conflict.count.load(), 1);
  ASSERT_EQ(conflict.last_notebook, std::string(notebook_id));
  ASSERT_EQ(conflict.last_files.size(), static_cast<size_t>(1));
  ASSERT_EQ(conflict.last_files[0], std::string("x/y.md"));

  vxcore_off_event(ctx, "sync.should_run", auto_route_cb);
  vxcore_off_event(ctx, "sync.conflict", conflict_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_auto_conflict"));
  std::cout << "  \xE2\x9C\x93 test_auto_sync_conflict_carries_files passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running sync tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_sync_error_codes);
  RUN_TEST(test_sync_enable_disable);
  RUN_TEST(test_sync_status_not_enabled);
  RUN_TEST(test_sync_builtin_git_registered);
  RUN_TEST(test_sync_trigger_not_implemented);
  RUN_TEST(test_sync_raw_notebook_rejected);
  RUN_TEST(test_sync_nonexistent_notebook);
  RUN_TEST(test_notebook_config_sync_fields);
  RUN_TEST(test_sync_config_from_json_with_backend_options);
  RUN_TEST(test_sync_config_to_json_includes_backend_options);
  RUN_TEST(test_sync_config_roundtrip_backend_options);
  RUN_TEST(test_sync_config_from_json_without_backend_options);
  RUN_TEST(test_sync_credentials_extra_field);
  RUN_TEST(test_sync_enable_with_backend_options_via_c_api);
  RUN_TEST(test_sync_config_to_json_omits_empty_backend_options);
  RUN_TEST(test_last_sync_utc_persistence);
  RUN_TEST(test_trigger_sync_emits_started_and_finished);
  RUN_TEST(test_trigger_sync_emits_finished_with_error_code);
  RUN_TEST(test_auto_sync_does_not_double_emit);
  RUN_TEST(test_trigger_sync_emits_conflict_with_files);
  RUN_TEST(test_auto_sync_conflict_carries_files);

  std::cout << "\xE2\x9C\x93 All sync tests passed" << std::endl;
  return 0;
}
