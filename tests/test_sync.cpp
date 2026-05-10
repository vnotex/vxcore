#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

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

  err = vxcore_sync_enable(
      ctx, notebook_id,
      "{\"backend\":\"mock\",\"remoteUrl\":\"test://repo\",\"intervalSeconds\":60}");
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

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_trigger(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_IMPLEMENTED);

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
  ASSERT_EQ(config["syncIntervalSeconds"].get<int>(), 300);
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

  std::cout << "\xE2\x9C\x93 All sync tests passed" << std::endl;
  return 0;
}
