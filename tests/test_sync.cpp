#include <chrono>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/context.h"
#include "sync/sync_backend.h"
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
   err = vxcore_sync_enable(
       ctx, notebook_id,
       "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}");
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

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}");
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

  err = vxcore_sync_enable(ctx, "nonexistent-id", "{\"backend\":\"mock\"}");
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
  err = vxcore_sync_enable(ctx, notebook_id, config_json);
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
   ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                                "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
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

int main() {
  std::cout << "Running sync tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_sync_error_codes);
  RUN_TEST(test_sync_enable_disable);
  RUN_TEST(test_sync_status_not_enabled);
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

  std::cout << "\xE2\x9C\x93 All sync tests passed" << std::endl;
  return 0;
}
