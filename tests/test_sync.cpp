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
  ASSERT_EQ(config["autoSyncEnabled"].get<bool>(), true);
  vxcore_string_free(config_json);

  // Update with sync fields
  err = vxcore_notebook_update_config(
      ctx, notebook_id,
      "{\"syncEnabled\":true,\"syncBackend\":\"git\","
      "\"syncRemoteUrl\":\"https://example.com/repo.git\",\"autoSyncEnabled\":false}");
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
  ASSERT_EQ(updated["autoSyncEnabled"].get<bool>(), false);
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
      {"autoSyncEnabled", false},
      {"backendOptions", {{"authHeader", "Bearer token123"}, {"depth", 1}}}};
  auto config = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(config.backend, "webdav");
  ASSERT_EQ(config.remote_url, "https://example.com/dav");
  ASSERT_FALSE(config.auto_sync_enabled);
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
  original.auto_sync_enabled = false;
  original.backend_options = {{"nested", {{"key", "value"}}}, {"list", {1, 2, 3}}};
  auto j = original.ToJson();
  auto restored = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(restored.backend, original.backend);
  ASSERT_EQ(restored.remote_url, original.remote_url);
  ASSERT_EQ(restored.auto_sync_enabled, original.auto_sync_enabled);
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

// Setter API (vxcore_sync_set_last_sync_utc) round-trip. VNote drives this from
// the GUI thread on a successful two-phase sync, since StageOnly/NetworkPhaseOnly
// (the production path) do NOT write last_sync_utc themselves. No mock backend /
// trigger needed: we assert the setter↔getter contract directly.
int test_set_last_sync_utc_roundtrip() {
  std::cout << "  Running test_set_last_sync_utc_roundtrip..." << std::endl;
  const std::string root = get_test_path("test_set_last_sync_utc");
  cleanup_test_dir(root);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(),
                                   "{\"name\":\"SetLastSyncUtc Test\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Before any write: getter returns 0 (never synced on this device).
  int64_t v = -1;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, notebook_id, &v), VXCORE_OK);
  ASSERT_EQ(v, 0);

  // Set an explicit value and read it straight back.
  const int64_t kStamp = 1700000000000LL;  // 2023-11-14T22:13:20Z
  ASSERT_EQ(vxcore_sync_set_last_sync_utc(ctx, notebook_id, kStamp), VXCORE_OK);
  v = 0;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, notebook_id, &v), VXCORE_OK);
  ASSERT_EQ(v, kStamp);

  // Overwrite with a newer value (idempotent last-writer-wins).
  const int64_t kStamp2 = 1800000000000LL;
  ASSERT_EQ(vxcore_sync_set_last_sync_utc(ctx, notebook_id, kStamp2), VXCORE_OK);
  v = 0;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(ctx, notebook_id, &v), VXCORE_OK);
  ASSERT_EQ(v, kStamp2);

  // Null-pointer guards.
  ASSERT_EQ(vxcore_sync_set_last_sync_utc(nullptr, notebook_id, kStamp),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_set_last_sync_utc(ctx, nullptr, kStamp), VXCORE_ERR_NULL_POINTER);

  // Not-found: unknown notebook id returns NOT_FOUND (no persistence).
  ASSERT_EQ(vxcore_sync_set_last_sync_utc(ctx, "nonexistent-nb-id", kStamp),
            VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(root);
  std::cout << "  \xE2\x9C\x93 test_set_last_sync_utc_roundtrip passed" << std::endl;
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
                                "\"autoSyncEnabled\":true}",
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

  // vxcore-metadata-events T4 (folder.config_changed) is also subscribed by
  // SyncManager::mark_dirty, so vxcore_file_create now legitimately produces
  // TWO sync.should_run emits per call: one for file.created and one for
  // folder.config_changed (fired from BundledFolderManager::SaveFolderConfig
  // when the parent folder's vx.json is rewritten). The local auto-route
  // subscriber drives a TriggerSync per emit, so we see 2 sync.started + 2
  // sync.finished. The name "does_not_double_emit" predates T4; the invariant
  // it still locks in is "exactly one TriggerSync per sync.should_run emit"
  // (i.e., no SPURIOUS extra round per event, just one round per legitimate
  // persistence-level fact).
  ASSERT_EQ(counters.started.load(), 2);
  ASSERT_EQ(counters.finished.load(), 2);

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
                                "\"autoSyncEnabled\":true}",
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

  // vxcore-metadata-events T4: vxcore_file_create produces TWO sync.should_run
  // emits (file.created + folder.config_changed from SaveFolderConfig). The
  // auto-route subscriber drives a TriggerSync per emit, and the mock backend
  // is configured to keep returning the same SyncConflictInfo set, so we get
  // two sync.conflict events (both carrying the same file list).
  ASSERT_EQ(conflict.count.load(), 2);
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

// ----------------------------------------------------------------------------
// vxcore-metadata-events T10 (acceptance test): proves the full persistence-
// event sync chain end-to-end for a tag mutation. Tags are the canonical
// "non-content" metadata operation that the plan added emissions for; this
// test wires the chain together with a MockSyncBackend-enabled notebook so
// we can observe the resulting sync.should_run + dirty-list facts.
//
// Chain under test (post T3 + T4 + T5):
//   vxcore_file_tag
//     -> BundledFolderManager::TagFile (success path)
//          -> EmitEvent(events::kFileTagged)             [T5 semantic event;
//             NOT subscribed by SyncManager::mark_dirty, so does NOT drive
//             sync.should_run]
//          -> SaveFolderConfig (containing folder only;
//             TagFile does no parent walk)
//               -> EmitEvent(events::kFolderConfigChanged)  [T4 persistence
//                  event; IS subscribed by mark_dirty in T3]
//                    -> mark_dirty
//                         -> DirtyTracker::MarkDirty(notebookId)
//                         -> MaybeEnqueueSync
//                              -> EmitEvent(events::kSyncShouldRun)
//                                 (EXACTLY 1 per vxcore_file_tag call)
// ----------------------------------------------------------------------------
int test_tag_file_triggers_sync_chain() {
  std::cout << "  Running test_tag_file_triggers_sync_chain..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_chain"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_tag_sync_chain").c_str(),
                                   "{\"name\":\"TagSyncChain\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  // Register this notebook with SyncManager so mark_dirty's predicate
  // (configs_cache_.count(nb_id) > 0) passes when folder.config_changed
  // arrives. Without enable, mark_dirty silently no-ops and the chain is
  // unobservable.
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\","
                                "\"autoSyncEnabled\":true}",
                               nullptr),
            VXCORE_OK);

  // Set up the file tree: one folder containing one file. Each of these
  // creates also drives folder.config_changed -> mark_dirty -> sync.should_run,
  // but we intentionally do NOT subscribe yet so they do not pollute the
  // counter we install below.
  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "notes", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "notes", "todo.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Define the tag at notebook level so TagFile's notebook_->FindTag() check
  // passes (otherwise it returns VXCORE_ERR_INVALID_PARAM with no emit).
  ASSERT_EQ(vxcore_tag_create(ctx, notebook_id, "important"), VXCORE_OK);

  // Clear the dirty flag that the setup writes left behind so the dirty-list
  // assertion below proves the TagFile call itself re-dirties the notebook
  // (and not just that prior writes had already marked it).
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  vctx->sync_manager->ClearDirty(notebook_id);
  ASSERT_TRUE(vctx->sync_manager->GetDirtyNotebooks().empty());

  // Subscribe AFTER setup so the counter captures ONLY the emit produced by
  // the upcoming vxcore_file_tag call. The capture-less lambda converts to a
  // C function pointer, matching the typedef for VxCoreEventCallback.
  std::atomic<int> should_run_count{0};
  auto count_cb = [](const char *, const char *, void *userdata) {
    static_cast<std::atomic<int> *>(userdata)->fetch_add(1);
  };
  ASSERT_EQ(vxcore_on_event(ctx, "sync.should_run", count_cb, &should_run_count), VXCORE_OK);

  // ACT: apply the tag — the full persistence-event chain is driven from here.
  ASSERT_EQ(vxcore_file_tag(ctx, notebook_id, "notes/todo.md", "important"), VXCORE_OK);

  // ASSERT 1: at least one sync.should_run reached the subscriber. In
  // practice the count is EXACTLY 1 (only folder.config_changed is subscribed
  // by mark_dirty for TagFile; file.tagged is the semantic event and is NOT
  // subscribed), but the integration contract locked in here is the lower
  // bound (>= 1) so future event-protocol additions stay backward-compatible.
  ASSERT_TRUE(should_run_count.load() >= 1);

  // ASSERT 2: the notebook is registered in SyncManager's dirty list. This
  // is the polling-fallback half of the auto-sync contract (the API non-Qt
  // embedders use to discover what needs syncing). mark_dirty added it; no
  // call to vxcore_sync_trigger has cleared it.
  auto dirty = vctx->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], std::string(notebook_id));

  vxcore_off_event(ctx, "sync.should_run", count_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_chain"));
  std::cout << "  \xE2\x9C\x93 test_tag_file_triggers_sync_chain passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// T11: integration test proving structural ops (RenameFolder/MoveFolder/
// CopyFolder/CopyFile/etc.) are already covered by the auto-sync contract
// WITHOUT needing dedicated structural events (folder.moved, folder.copied,
// ...). The mechanism: every structural op rewrites at least one vx.json via
// SaveFolderConfig, which fires the T4 persistence event
// (events::kFolderConfigChanged). SyncManager::mark_dirty is subscribed to
// that persistence event (T3), so the full chain runs end-to-end:
//
//   vxcore_node_rename (folder path)
//      -> BundledFolderManager::RenameFolder
//           -> SaveFolderConfig(new_folder_path, ...)      [folder's own vx.json
//                                                           at the new location]
//                -> EmitEvent(events::kFolderConfigChanged) [+1]
//                     -> mark_dirty
//                          -> DirtyTracker::MarkDirty(notebookId)
//                          -> MaybeEnqueueSync
//                               -> EmitEvent(events::kSyncShouldRun) [+1]
//           -> SaveFolderConfig(parent_path, ...)          [parent's vx.json
//                                                           with renamed entry]
//                -> EmitEvent(events::kFolderConfigChanged) [+1]
//                     -> mark_dirty -> ... -> EmitEvent(kSyncShouldRun) [+1]
//
// Observed counts at the time of writing: 2 folder.config_changed +
// 2 sync.should_run per vxcore_node_rename of a folder. The integration
// contract locked in here is the LOWER BOUND (>= 1 each) so future refactors
// of RenameFolder (e.g., collapsing the two SaveFolderConfig calls into one
// batched write) stay backward-compatible. The point is that *structural*
// auto-sync works today through the persistence chain alone; no dedicated
// folder.moved/folder.copied event is required.
// ----------------------------------------------------------------------------
int test_rename_folder_triggers_sync_chain_via_persistence() {
  std::cout << "  Running test_rename_folder_triggers_sync_chain_via_persistence..." << std::endl;
  cleanup_test_dir(get_test_path("test_rename_folder_sync_chain"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_rename_folder_sync_chain").c_str(),
                                   "{\"name\":\"RenameFolderSyncChain\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Register this notebook with SyncManager so mark_dirty's predicate
  // (configs_cache_.count(nb_id) > 0) passes when folder.config_changed
  // arrives. Without enable, mark_dirty silently no-ops and the chain is
  // unobservable.
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\","
                                "\"autoSyncEnabled\":true}",
                               nullptr),
            VXCORE_OK);

  // Create a folder so there's something to rename. This SaveFolderConfig
  // itself drives the persistence chain too, but we subscribe AFTER setup
  // so it does NOT pollute the counters.
  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, ".", "old_name", &folder_id), VXCORE_OK);
  vxcore_string_free(folder_id);

  // Clear the dirty flag that setup writes left behind so the dirty-list
  // assertion below proves the RenameFolder call itself re-dirties the
  // notebook (and not just that the prior create had already marked it).
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  vctx->sync_manager->ClearDirty(notebook_id);
  ASSERT_TRUE(vctx->sync_manager->GetDirtyNotebooks().empty());

  // Subscribe AFTER setup. Two separate counters: one proves the persistence
  // event fires (kFolderConfigChanged); the other proves the auto-sync
  // wakeup happens via the chain (kSyncShouldRun). Capture-less lambdas
  // convert to C function pointers, matching VxCoreEventCallback's typedef.
  std::atomic<int> folder_config_count{0};
  std::atomic<int> should_run_count{0};
  auto count_cb = [](const char *, const char *, void *userdata) {
    static_cast<std::atomic<int> *>(userdata)->fetch_add(1);
  };
  ASSERT_EQ(vxcore_on_event(ctx, "folder.config_changed", count_cb, &folder_config_count),
            VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(ctx, "sync.should_run", count_cb, &should_run_count), VXCORE_OK);

  // ACT: rename the folder via the unified node API; the path "old_name"
  // routes through DetectNodeType -> RenameFolder under the hood. Note that
  // new_name is just the basename, not a full path.
  ASSERT_EQ(vxcore_node_rename(ctx, notebook_id, "old_name", "new_name"), VXCORE_OK);

  // ASSERT 1: the persistence event (folder.config_changed) fired at least
  // once. RenameFolder currently calls SaveFolderConfig twice (folder's own
  // vx.json at the new location + parent's vx.json with the renamed entry),
  // so the practical count is 2. The contract assertion is the lower bound
  // to keep this test stable across future implementation tweaks.
  ASSERT_TRUE(folder_config_count.load() >= 1);

  // ASSERT 2: at least one sync.should_run reached the subscriber. mark_dirty
  // is subscribed to folder.config_changed (T3), so each persistence emit
  // drives one sync.should_run. The integration contract is >= 1; observed
  // count is 2.
  ASSERT_TRUE(should_run_count.load() >= 1);

  // ASSERT 3: the notebook is registered in SyncManager's dirty list — the
  // polling-fallback half of the auto-sync contract. mark_dirty added it;
  // no call to vxcore_sync_trigger has cleared it.
  auto dirty = vctx->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], std::string(notebook_id));

  vxcore_off_event(ctx, "sync.should_run", count_cb);
  vxcore_off_event(ctx, "folder.config_changed", count_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_rename_folder_sync_chain"));
  std::cout
      << "  \xE2\x9C\x93 test_rename_folder_triggers_sync_chain_via_persistence passed"
      << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Auto-sync gate (sync-interval-debounce): enabling a notebook with
// "autoSyncEnabled":false must suppress the sync.should_run emission that the
// auto path would otherwise fire when a watched mutation marks the notebook
// dirty. The gate lives in SyncManager::MaybeEnqueueSync (it skips the Emit
// when effective_cfg.auto_sync_enabled is false). Crucially the notebook is
// STILL marked dirty (the polling-fallback half of the contract via
// GetDirtyNotebooks); only the event-driven wakeup is suppressed. This mirrors
// the "should NOT emit" assertions in test_buffer_save_marks_dirty.cpp and the
// dirty-tracking gold standard in test_event_manager.cpp.
// ----------------------------------------------------------------------------
int test_auto_sync_disabled_suppresses_should_run() {
  std::cout << "  Running test_auto_sync_disabled_suppresses_should_run..." << std::endl;
  cleanup_test_dir(get_test_path("test_auto_sync_disabled"));

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, get_test_path("test_auto_sync_disabled").c_str(),
                                   "{\"name\":\"AutoSyncDisabled\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &notebook_id),
            VXCORE_OK);

  // Enable sync but with the auto-sync gate OFF for this notebook.
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\","
                               "\"autoSyncEnabled\":false}",
                               nullptr),
            VXCORE_OK);

  // Subscribe a counter to sync.should_run. The capture-less lambda converts
  // to a C function pointer matching VxCoreEventCallback.
  std::atomic<int> should_run_count{0};
  auto count_cb = [](const char *, const char *, void *userdata) {
    static_cast<std::atomic<int> *>(userdata)->fetch_add(1);
  };
  ASSERT_EQ(vxcore_on_event(ctx, "sync.should_run", count_cb, &should_run_count), VXCORE_OK);

  // ACT: a watched mutation. file.created -> mark_dirty -> MaybeEnqueueSync,
  // whose gate suppresses the emit because auto_sync_enabled == false.
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "no_auto.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // ASSERT 1: the gate suppressed EVERY sync.should_run emit.
  ASSERT_EQ(should_run_count.load(), 0);

  // ASSERT 2: the notebook is STILL dirty. The gate suppresses only the
  // event-driven wakeup; mark_dirty's DirtyTracker entry (the polling
  // fallback) is unaffected. This proves we suppressed emission, not tracking.
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto dirty = vctx->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], std::string(notebook_id));

  vxcore_off_event(ctx, "sync.should_run", count_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_auto_sync_disabled"));
  std::cout << "  \xE2\x9C\x93 test_auto_sync_disabled_suppresses_should_run passed" << std::endl;
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
  RUN_TEST(test_set_last_sync_utc_roundtrip);
  RUN_TEST(test_trigger_sync_emits_started_and_finished);
  RUN_TEST(test_trigger_sync_emits_finished_with_error_code);
  RUN_TEST(test_auto_sync_does_not_double_emit);
  RUN_TEST(test_trigger_sync_emits_conflict_with_files);
  RUN_TEST(test_auto_sync_conflict_carries_files);
  RUN_TEST(test_tag_file_triggers_sync_chain);
  RUN_TEST(test_rename_folder_triggers_sync_chain_via_persistence);
  RUN_TEST(test_auto_sync_disabled_suppresses_should_run);

  std::cout << "\xE2\x9C\x93 All sync tests passed" << std::endl;
  return 0;
}
