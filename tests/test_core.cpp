#include <iostream>
#include <nlohmann/json.hpp>

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_version() {
  std::cout << "  Running test_version..." << std::endl;
  VxCoreVersion version = vxcore_get_version();
  ASSERT_EQ(version.major, 0);
  ASSERT_EQ(version.minor, 1);
  ASSERT_EQ(version.patch, 0);
  std::cout << "  ✓ test_version passed" << std::endl;
  return 0;
}

int test_error_message() {
  std::cout << "  Running test_error_message..." << std::endl;
  const char *msg = vxcore_error_message(VXCORE_OK);
  ASSERT_NOT_NULL(msg);
  ASSERT_NE(std::string(msg), "");
  std::cout << "  ✓ test_error_message passed" << std::endl;
  return 0;
}

int test_context_create_destroy() {
  std::cout << "  Running test_context_create_destroy..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_context_create_destroy passed" << std::endl;
  return 0;
}

int test_get_config_by_name_not_found() {
  std::cout << "  Running test_get_config_by_name_not_found..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  char *json = nullptr;
  err = vxcore_context_get_config_by_name(ctx, VXCORE_DATA_APP, "nonexistent_config", &json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(json);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_get_config_by_name_not_found passed" << std::endl;
  return 0;
}

int test_update_and_get_config_by_name() {
  std::cout << "  Running test_update_and_get_config_by_name..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Write config
  err = vxcore_context_update_config_by_name(ctx, VXCORE_DATA_LOCAL, "myui_test",
                                             R"({"theme": "dark", "fontSize": 14})");
  ASSERT_EQ(err, VXCORE_OK);

  // Read it back
  char *json = nullptr;
  err = vxcore_context_get_config_by_name(ctx, VXCORE_DATA_LOCAL, "myui_test", &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);

  // Verify content
  nlohmann::json parsed = nlohmann::json::parse(json);
  ASSERT_EQ(parsed["theme"], "dark");
  ASSERT_EQ(parsed["fontSize"], 14);

  vxcore_string_free(json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_update_and_get_config_by_name passed" << std::endl;
  return 0;
}

int test_update_config_raw_content() {
  std::cout << "  Running test_update_config_raw_content..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Write raw content (not necessarily valid JSON - UI layer's responsibility)
  const char *raw_content = "this is raw content\nwith newlines";
  err = vxcore_context_update_config_by_name(ctx, VXCORE_DATA_APP, "raw_test", raw_content);
  ASSERT_EQ(err, VXCORE_OK);

  // Read it back - should be identical
  char *content = nullptr;
  err = vxcore_context_get_config_by_name(ctx, VXCORE_DATA_APP, "raw_test", &content);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content);
  ASSERT_EQ(std::string(content), std::string(raw_content));

  vxcore_string_free(content);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_update_config_raw_content passed" << std::endl;
  return 0;
}

int test_config_by_name_both_locations() {
  std::cout << "  Running test_config_by_name_both_locations..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Write to APP location
  err = vxcore_context_update_config_by_name(ctx, VXCORE_DATA_APP, "app_config",
                                             R"({"location": "app"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Write to LOCAL location with different name
  err = vxcore_context_update_config_by_name(ctx, VXCORE_DATA_LOCAL, "local_config",
                                             R"({"location": "local"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Read back from APP
  char *app_json = nullptr;
  err = vxcore_context_get_config_by_name(ctx, VXCORE_DATA_APP, "app_config", &app_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json app_parsed = nlohmann::json::parse(app_json);
  ASSERT_EQ(app_parsed["location"], "app");

  // Read back from LOCAL
  char *local_json = nullptr;
  err = vxcore_context_get_config_by_name(ctx, VXCORE_DATA_LOCAL, "local_config", &local_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json local_parsed = nlohmann::json::parse(local_json);
  ASSERT_EQ(local_parsed["location"], "local");

  vxcore_string_free(app_json);
  vxcore_string_free(local_json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_config_by_name_both_locations passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running core tests..." << std::endl;

  vxcore_set_test_mode(1);

  RUN_TEST(test_version);
  RUN_TEST(test_error_message);
  RUN_TEST(test_context_create_destroy);
  RUN_TEST(test_get_config_by_name_not_found);
  RUN_TEST(test_update_and_get_config_by_name);
  RUN_TEST(test_update_config_raw_content);
  RUN_TEST(test_config_by_name_both_locations);

  std::cout << "✓ All core tests passed" << std::endl;
  return 0;
}
