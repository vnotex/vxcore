#include <iostream>
#include <nlohmann/json.hpp>

#include "test_utils.h"
#include "vxcore/vxcore.h"

// Test default file types are correctly initialized
int test_filetype_defaults() {
  std::cout << "  Running test_filetype_defaults..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  char *json_str = nullptr;
  err = vxcore_filetype_list(ctx, &json_str);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json_str);

  nlohmann::json j = nlohmann::json::parse(json_str);
  ASSERT_EQ(j.size(), 5);
  ASSERT_EQ(j[0]["name"], "Markdown");
  ASSERT_EQ(j[1]["name"], "Text");
  ASSERT_EQ(j[2]["name"], "PDF");
  ASSERT_EQ(j[3]["name"], "MindMap");
  ASSERT_EQ(j[4]["name"], "Others");

  vxcore_string_free(json_str);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_defaults passed" << std::endl;
  return 0;
}

// Test typeIds are correctly mapped (0-4)
int test_filetype_type_ids() {
  std::cout << "  Running test_filetype_type_ids..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json_str = nullptr;
  err = vxcore_filetype_list(ctx, &json_str);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json j = nlohmann::json::parse(json_str);
  ASSERT_EQ(j[0]["typeId"], 0);
  ASSERT_EQ(j[1]["typeId"], 1);
  ASSERT_EQ(j[2]["typeId"], 2);
  ASSERT_EQ(j[3]["typeId"], 3);
  ASSERT_EQ(j[4]["typeId"], 4);

  vxcore_string_free(json_str);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_type_ids passed" << std::endl;
  return 0;
}

// Test GetBySuffix returns Markdown for "md"
int test_filetype_get_by_suffix_md() {
  std::cout << "  Running test_filetype_get_by_suffix_md..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json_str = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "md", &json_str);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json_str);

  nlohmann::json j = nlohmann::json::parse(json_str);
  ASSERT_EQ(j["typeId"], 0);
  ASSERT_EQ(j["name"], "Markdown");

  vxcore_string_free(json_str);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_get_by_suffix_md passed" << std::endl;
  return 0;
}

// Test GetBySuffix returns Others for unknown suffix
int test_filetype_get_by_suffix_unknown() {
  std::cout << "  Running test_filetype_get_by_suffix_unknown..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json_str = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "xyz", &json_str);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json_str);

  nlohmann::json j = nlohmann::json::parse(json_str);
  ASSERT_EQ(j["typeId"], 4);
  ASSERT_EQ(j["name"], "Others");

  vxcore_string_free(json_str);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_get_by_suffix_unknown passed" << std::endl;
  return 0;
}

// Test case-insensitive suffix lookup
int test_filetype_case_insensitive() {
  std::cout << "  Running test_filetype_case_insensitive..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json1 = nullptr, *json2 = nullptr, *json3 = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "md", &json1);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_filetype_get_by_suffix(ctx, "MD", &json2);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_filetype_get_by_suffix(ctx, "Md", &json3);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json j1 = nlohmann::json::parse(json1);
  nlohmann::json j2 = nlohmann::json::parse(json2);
  nlohmann::json j3 = nlohmann::json::parse(json3);

  ASSERT_EQ(j1["typeId"], j2["typeId"]);
  ASSERT_EQ(j2["typeId"], j3["typeId"]);
  ASSERT_EQ(j1["typeId"], 0);

  vxcore_string_free(json1);
  vxcore_string_free(json2);
  vxcore_string_free(json3);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_case_insensitive passed" << std::endl;
  return 0;
}

// Test PDF has isNewable=false
int test_filetype_pdf_not_newable() {
  std::cout << "  Running test_filetype_pdf_not_newable..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Get PDF type
  char *pdf_json = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "pdf", &pdf_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(pdf_json);

  nlohmann::json pdf = nlohmann::json::parse(pdf_json);
  ASSERT_EQ(pdf["name"], "PDF");
  ASSERT_FALSE(pdf["isNewable"]);
  vxcore_string_free(pdf_json);

  // Get all types to verify others are newable
  char *list_json = nullptr;
  err = vxcore_filetype_list(ctx, &list_json);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json types = nlohmann::json::parse(list_json);
  ASSERT_TRUE(types[0]["isNewable"]);  // Markdown
  ASSERT_TRUE(types[1]["isNewable"]);  // Text
  ASSERT_TRUE(types[3]["isNewable"]);  // MindMap
  ASSERT_TRUE(types[4]["isNewable"]);  // Others
  vxcore_string_free(list_json);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_pdf_not_newable passed" << std::endl;
  return 0;
}

// Test GetDisplayName returns default display name
int test_filetype_display_name_default() {
  std::cout << "  Running test_filetype_display_name_default..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json_str = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "md", &json_str);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json j = nlohmann::json::parse(json_str);
  ASSERT_EQ(j["displayName"], "Markdown");

  vxcore_string_free(json_str);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_display_name_default passed" << std::endl;
  return 0;
}

// Test metadata field is optional and can contain locale-specific names
int test_filetype_display_name_locale() {
  std::cout << "  Running test_filetype_display_name_locale..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json_str = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "md", &json_str);
  ASSERT_EQ(err, VXCORE_OK);

  nlohmann::json j = nlohmann::json::parse(json_str);

  // Metadata field is optional (only present if not empty)
  // If present, it must be a valid JSON object
  if (j.contains("metadata")) {
    ASSERT_TRUE(j["metadata"].is_object());
  }

  vxcore_string_free(json_str);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_filetype_display_name_locale passed" << std::endl;
  return 0;
}

// Test C API: vxcore_filetype_list returns 5 types
int test_filetype_c_api_list() {
  std::cout << "  Running test_filetype_c_api_list..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  char *json = nullptr;
  err = vxcore_filetype_list(ctx, &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);

  // Parse and verify
  nlohmann::json types_array = nlohmann::json::parse(json);
  ASSERT_TRUE(types_array.is_array());
  ASSERT_EQ(types_array.size(), 5);

  // Verify first entry structure
  ASSERT_TRUE(types_array[0].contains("typeId"));
  ASSERT_TRUE(types_array[0].contains("name"));
  ASSERT_TRUE(types_array[0].contains("suffixes"));
  ASSERT_TRUE(types_array[0].contains("isNewable"));
  ASSERT_TRUE(types_array[0].contains("displayName"));

  vxcore_string_free(json);
  vxcore_context_destroy(ctx);

  std::cout << "  ✓ test_filetype_c_api_list passed" << std::endl;
  return 0;
}

// Test C API: vxcore_filetype_get_by_suffix works
int test_filetype_c_api_suffix() {
  std::cout << "  Running test_filetype_c_api_suffix..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Test known suffix
  char *json1 = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "md", &json1);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json1);

  nlohmann::json entry1 = nlohmann::json::parse(json1);
  ASSERT_EQ(entry1["typeId"], 0);
  ASSERT_EQ(entry1["name"], "Markdown");
  vxcore_string_free(json1);

  // Test PDF (not newable)
  char *json2 = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "pdf", &json2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json2);

  nlohmann::json entry2 = nlohmann::json::parse(json2);
  ASSERT_EQ(entry2["typeId"], 2);
  ASSERT_EQ(entry2["name"], "PDF");
  ASSERT_FALSE(entry2["isNewable"]);
  vxcore_string_free(json2);

  // Test unknown suffix returns Others
  char *json3 = nullptr;
  err = vxcore_filetype_get_by_suffix(ctx, "xyz", &json3);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json3);

  nlohmann::json entry3 = nlohmann::json::parse(json3);
  ASSERT_EQ(entry3["typeId"], 4);
  ASSERT_EQ(entry3["name"], "Others");
  vxcore_string_free(json3);

  vxcore_context_destroy(ctx);

  std::cout << "  ✓ test_filetype_c_api_suffix passed" << std::endl;
  return 0;
}

// Test C API: vxcore_filetype_get_by_name works
int test_filetype_c_api_name() {
  std::cout << "  Running test_filetype_c_api_name..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json = nullptr;
  err = vxcore_filetype_get_by_name(ctx, "Markdown", &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);

  nlohmann::json entry = nlohmann::json::parse(json);
  ASSERT_EQ(entry["typeId"], 0);
  ASSERT_EQ(entry["name"], "Markdown");

  vxcore_string_free(json);
  vxcore_context_destroy(ctx);

  std::cout << "  ✓ test_filetype_c_api_name passed" << std::endl;
  return 0;
}

// Test C API: Unknown name returns VXCORE_ERR_NOT_FOUND
int test_filetype_c_api_name_not_found() {
  std::cout << "  Running test_filetype_c_api_name_not_found..." << std::endl;
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *json = nullptr;
  err = vxcore_filetype_get_by_name(ctx, "UnknownType", &json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(json);

  vxcore_context_destroy(ctx);

  std::cout << "  ✓ test_filetype_c_api_name_not_found passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running filetype tests..." << std::endl;

  vxcore_set_test_mode(1);

  RUN_TEST(test_filetype_defaults);
  RUN_TEST(test_filetype_type_ids);
  RUN_TEST(test_filetype_get_by_suffix_md);
  RUN_TEST(test_filetype_get_by_suffix_unknown);
  RUN_TEST(test_filetype_case_insensitive);
  RUN_TEST(test_filetype_pdf_not_newable);
  RUN_TEST(test_filetype_display_name_default);
  RUN_TEST(test_filetype_display_name_locale);
  RUN_TEST(test_filetype_c_api_list);
  RUN_TEST(test_filetype_c_api_suffix);
  RUN_TEST(test_filetype_c_api_name);
  RUN_TEST(test_filetype_c_api_name_not_found);

  std::cout << "✓ All filetype tests passed" << std::endl;
  return 0;
}
