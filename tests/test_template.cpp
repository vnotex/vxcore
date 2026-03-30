#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

#include "test_utils.h"
#include "vxcore/vxcore.h"

// Helper: remove the templates folder for test isolation.
// Each test creates files that would contaminate subsequent tests.
static void clean_templates_folder(VxCoreContextHandle ctx) {
  char *path = nullptr;
  VxCoreError err = vxcore_template_get_folder_path(ctx, &path);
  if (err == VXCORE_OK && path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    vxcore_string_free(path);
  }
}

int test_template_get_folder_path() {
  std::cout << "  Running test_template_get_folder_path..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  char *path = nullptr;
  err = vxcore_template_get_folder_path(ctx, &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);

  // Path should contain "templates" substring
  ASSERT_TRUE(std::string(path).find("templates") != std::string::npos);

  // Folder should be auto-created
  ASSERT_TRUE(path_exists(path));

  vxcore_string_free(path);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_get_folder_path passed" << std::endl;
  return 0;
}

int test_template_create_and_get_content() {
  std::cout << "  Running test_template_create_and_get_content..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  // Add a template
  err = vxcore_template_create(ctx, "test.md", "# Hello\nWorld");
  ASSERT_EQ(err, VXCORE_OK);

  // Read it back
  char *content = nullptr;
  err = vxcore_template_get_content(ctx, "test.md", &content);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content);
  ASSERT_EQ(std::string(content), std::string("# Hello\nWorld"));

  vxcore_string_free(content);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_create_and_get_content passed" << std::endl;
  return 0;
}

int test_template_list() {
  std::cout << "  Running test_template_list..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  // Add 3 templates with different extensions
  err = vxcore_template_create(ctx, "a.md", "a");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_create(ctx, "b.txt", "b");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_create(ctx, "c.html", "c");
  ASSERT_EQ(err, VXCORE_OK);

  // List all templates
  char *json = nullptr;
  err = vxcore_template_list(ctx, &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);

  auto arr = nlohmann::json::parse(json);
  ASSERT_EQ(arr.size(), 3u);

  // Verify all 3 names are present
  bool found_a = false, found_b = false, found_c = false;
  for (const auto &item : arr) {
    std::string name = item.get<std::string>();
    if (name == "a.md") found_a = true;
    if (name == "b.txt") found_b = true;
    if (name == "c.html") found_c = true;
  }
  ASSERT_TRUE(found_a);
  ASSERT_TRUE(found_b);
  ASSERT_TRUE(found_c);

  vxcore_string_free(json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_list passed" << std::endl;
  return 0;
}

int test_template_list_by_suffix() {
  std::cout << "  Running test_template_list_by_suffix..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  // Add templates with mixed extensions
  err = vxcore_template_create(ctx, "a.md", "a");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_create(ctx, "b.txt", "b");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_create(ctx, "c.md", "c");
  ASSERT_EQ(err, VXCORE_OK);

  // List by ".md" — should get 2
  char *json = nullptr;
  err = vxcore_template_list_by_suffix(ctx, ".md", &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);
  auto arr = nlohmann::json::parse(json);
  ASSERT_EQ(arr.size(), 2u);
  vxcore_string_free(json);

  // List by ".txt" — should get 1
  json = nullptr;
  err = vxcore_template_list_by_suffix(ctx, ".txt", &json);
  ASSERT_EQ(err, VXCORE_OK);
  arr = nlohmann::json::parse(json);
  ASSERT_EQ(arr.size(), 1u);
  vxcore_string_free(json);

  // List by ".xyz" — should get 0
  json = nullptr;
  err = vxcore_template_list_by_suffix(ctx, ".xyz", &json);
  ASSERT_EQ(err, VXCORE_OK);
  arr = nlohmann::json::parse(json);
  ASSERT_EQ(arr.size(), 0u);
  vxcore_string_free(json);

  // Case-insensitivity: add "d.MD" and list by ".md"
  err = vxcore_template_create(ctx, "d.MD", "d");
  ASSERT_EQ(err, VXCORE_OK);

  json = nullptr;
  err = vxcore_template_list_by_suffix(ctx, ".md", &json);
  ASSERT_EQ(err, VXCORE_OK);
  arr = nlohmann::json::parse(json);
  ASSERT_EQ(arr.size(), 3u);
  vxcore_string_free(json);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_list_by_suffix passed" << std::endl;
  return 0;
}

int test_template_delete() {
  std::cout << "  Running test_template_delete..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  // Add a template
  err = vxcore_template_create(ctx, "to_delete.md", "content");
  ASSERT_EQ(err, VXCORE_OK);

  // Delete it
  err = vxcore_template_delete(ctx, "to_delete.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Reading deleted template should fail
  char *content = nullptr;
  err = vxcore_template_get_content(ctx, "to_delete.md", &content);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_delete passed" << std::endl;
  return 0;
}

int test_template_rename() {
  std::cout << "  Running test_template_rename..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  // Add a template
  err = vxcore_template_create(ctx, "old.md", "content");
  ASSERT_EQ(err, VXCORE_OK);

  // Rename it
  err = vxcore_template_rename(ctx, "old.md", "new.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Old name should not exist
  char *content = nullptr;
  err = vxcore_template_get_content(ctx, "old.md", &content);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // New name should have the content
  content = nullptr;
  err = vxcore_template_get_content(ctx, "new.md", &content);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content);
  ASSERT_EQ(std::string(content), std::string("content"));

  vxcore_string_free(content);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_rename passed" << std::endl;
  return 0;
}

int test_template_error_cases() {
  std::cout << "  Running test_template_error_cases..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  char *json = nullptr;
  char *content = nullptr;

  // Null context
  err = vxcore_template_list(nullptr, &json);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Null name parameter
  err = vxcore_template_get_content(ctx, nullptr, &content);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Path traversal with forward slash
  err = vxcore_template_create(ctx, "a/b.md", "x");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Path traversal with backslash
  err = vxcore_template_create(ctx, "a\\b.md", "x");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Read nonexistent template
  err = vxcore_template_get_content(ctx, "nonexistent.md", &content);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Delete nonexistent template
  err = vxcore_template_delete(ctx, "nonexistent.md");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Add duplicate template
  err = vxcore_template_create(ctx, "exists.md", "x");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_create(ctx, "exists.md", "y");
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  // Rename to existing name
  err = vxcore_template_create(ctx, "a.md", "a");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_create(ctx, "b.md", "b");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_rename(ctx, "a.md", "b.md");
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_error_cases passed" << std::endl;
  return 0;
}

int test_template_edge_cases() {
  std::cout << "  Running test_template_edge_cases..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  clean_templates_folder(ctx);

  // "." as filename
  err = vxcore_template_create(ctx, ".", "x");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // ".." as filename
  err = vxcore_template_create(ctx, "..", "x");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // Empty content is allowed
  err = vxcore_template_create(ctx, "empty.md", "");
  ASSERT_EQ(err, VXCORE_OK);

  // Read empty content back — should be empty string, not null
  char *content = nullptr;
  err = vxcore_template_get_content(ctx, "empty.md", &content);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content);
  ASSERT_EQ(std::string(content), std::string(""));
  vxcore_string_free(content);

  // Hidden files should be excluded from list
  char *path = nullptr;
  err = vxcore_template_get_folder_path(ctx, &path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(path);

  // Manually create a hidden file in the templates folder
  std::string hidden_path = std::string(path) + "/.hidden";
  write_file(hidden_path, "hidden content");
  ASSERT_TRUE(path_exists(hidden_path));

  // List should NOT include the hidden file
  char *json = nullptr;
  err = vxcore_template_list(ctx, &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);

  auto arr = nlohmann::json::parse(json);
  bool found_hidden = false;
  for (const auto &item : arr) {
    if (item.get<std::string>() == ".hidden") {
      found_hidden = true;
    }
  }
  ASSERT_FALSE(found_hidden);

  vxcore_string_free(json);
  vxcore_string_free(path);

  // Same-name rename should succeed (no-op)
  err = vxcore_template_create(ctx, "same.md", "s");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_template_rename(ctx, "same.md", "same.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Content should still be intact after same-name rename
  content = nullptr;
  err = vxcore_template_get_content(ctx, "same.md", &content);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content);
  ASSERT_EQ(std::string(content), std::string("s"));
  vxcore_string_free(content);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_template_edge_cases passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  std::cout << "Running template tests..." << std::endl;

  RUN_TEST(test_template_get_folder_path);
  RUN_TEST(test_template_create_and_get_content);
  RUN_TEST(test_template_list);
  RUN_TEST(test_template_list_by_suffix);
  RUN_TEST(test_template_delete);
  RUN_TEST(test_template_rename);
  RUN_TEST(test_template_error_cases);
  RUN_TEST(test_template_edge_cases);

  std::cout << "\nAll template tests passed!" << std::endl;
  vxcore_clear_test_directory();
  return 0;
}
