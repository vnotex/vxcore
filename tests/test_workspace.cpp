#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_workspace_create() {
  std::cout << "  Running test_workspace_create..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "Test Workspace", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(workspace_id);

  vxcore_string_free(workspace_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_create passed" << std::endl;
  return 0;
}

int test_workspace_get() {
  std::cout << "  Running test_workspace_get..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "Get Test", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_json = nullptr;
  err = vxcore_workspace_get(ctx, workspace_id, &workspace_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(workspace_json);

  // Verify JSON contains expected fields
  std::string json_str(workspace_json);
  ASSERT(json_str.find("\"id\"") != std::string::npos);
  ASSERT(json_str.find("\"name\"") != std::string::npos);
  ASSERT(json_str.find("Get Test") != std::string::npos);

  vxcore_string_free(workspace_id);
  vxcore_string_free(workspace_json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_get passed" << std::endl;
  return 0;
}

int test_workspace_list() {
  std::cout << "  Running test_workspace_list..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a workspace first
  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "List Test", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *list_json = nullptr;
  err = vxcore_workspace_list(ctx, &list_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(list_json);

  // Should contain the created workspace
  std::string json_str(list_json);
  ASSERT(json_str.find("List Test") != std::string::npos);

  vxcore_string_free(workspace_id);
  vxcore_string_free(list_json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_list passed" << std::endl;
  return 0;
}

int test_workspace_rename() {
  std::cout << "  Running test_workspace_rename..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "Original Name", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_workspace_rename(ctx, workspace_id, "Renamed Workspace");
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_json = nullptr;
  err = vxcore_workspace_get(ctx, workspace_id, &workspace_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string json_str(workspace_json);
  ASSERT(json_str.find("Renamed Workspace") != std::string::npos);

  vxcore_string_free(workspace_id);
  vxcore_string_free(workspace_json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_rename passed" << std::endl;
  return 0;
}

int test_workspace_delete() {
  std::cout << "  Running test_workspace_delete..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "To Delete", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_workspace_delete(ctx, workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to get deleted workspace - should fail
  char *workspace_json = nullptr;
  err = vxcore_workspace_get(ctx, workspace_id, &workspace_json);
  ASSERT_EQ(err, VXCORE_ERR_WORKSPACE_NOT_FOUND);

  vxcore_string_free(workspace_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_delete passed" << std::endl;
  return 0;
}

int test_workspace_current() {
  std::cout << "  Running test_workspace_current..." << std::endl;

  // Clear test directory to start fresh for this test
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // No workspaces at all - current should be empty initially
  char *current_id = nullptr;
  err = vxcore_workspace_get_current(ctx, &current_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(current_id);
  ASSERT_EQ(std::string(current_id), std::string(""));
  vxcore_string_free(current_id);

  // Create workspace and set as current
  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "New Current", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_workspace_set_current(ctx, workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *current_id2 = nullptr;
  err = vxcore_workspace_get_current(ctx, &current_id2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(std::string(workspace_id), std::string(current_id2));

  vxcore_string_free(current_id2);
  vxcore_string_free(workspace_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_current passed" << std::endl;
  return 0;
}

int test_workspace_buffer_add_remove() {
  std::cout << "  Running test_workspace_buffer_add_remove..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "Buffer Test", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Add buffer to workspace (using fake buffer ID for now)
  const char *buffer_id = "test-buffer-id-123";
  err = vxcore_workspace_add_buffer(ctx, workspace_id, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to add same buffer again - should succeed (idempotent operation)
  err = vxcore_workspace_add_buffer(ctx, workspace_id, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Remove buffer
  err = vxcore_workspace_remove_buffer(ctx, workspace_id, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(workspace_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_buffer_add_remove passed" << std::endl;
  return 0;
}

int test_workspace_current_buffer() {
  std::cout << "  Running test_workspace_current_buffer..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *workspace_id = nullptr;
  err = vxcore_workspace_create(ctx, "Current Buffer Test", &workspace_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *buffer_id = "test-buffer-id-456";
  err = vxcore_workspace_add_buffer(ctx, workspace_id, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Set current buffer
  err = vxcore_workspace_set_current_buffer(ctx, workspace_id, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify current buffer in JSON
  char *workspace_json = nullptr;
  err = vxcore_workspace_get(ctx, workspace_id, &workspace_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string json_str(workspace_json);
  ASSERT(json_str.find(buffer_id) != std::string::npos);

  vxcore_string_free(workspace_id);
  vxcore_string_free(workspace_json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_current_buffer passed" << std::endl;
  return 0;
}

int test_workspace_empty_initially() {
  std::cout << "  Running test_workspace_empty_initially..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // No default workspace - list should be empty
  char *list_json = nullptr;
  err = vxcore_workspace_list(ctx, &list_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string json_str(list_json);
  ASSERT_EQ(json_str, std::string("[]"));

  vxcore_string_free(list_json);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_empty_initially passed" << std::endl;
  return 0;
}

int test_workspace_persistence() {
  std::cout << "  Running test_workspace_persistence..." << std::endl;

  std::string workspace_name = "Persistent Workspace";
  std::string workspace_id_str;

  // Create workspace and save
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *workspace_id = nullptr;
    err = vxcore_workspace_create(ctx, workspace_name.c_str(), &workspace_id);
    ASSERT_EQ(err, VXCORE_OK);
    workspace_id_str = std::string(workspace_id);

    vxcore_string_free(workspace_id);
    vxcore_context_destroy(ctx);  // This should save session config
  }

  // Reload and verify workspace persisted
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *workspace_json = nullptr;
    err = vxcore_workspace_get(ctx, workspace_id_str.c_str(), &workspace_json);
    ASSERT_EQ(err, VXCORE_OK);

    std::string json_str(workspace_json);
    ASSERT(json_str.find(workspace_name) != std::string::npos);

    vxcore_string_free(workspace_json);
    vxcore_context_destroy(ctx);
  }

  std::cout << "  ✓ test_workspace_persistence passed" << std::endl;
  return 0;
}

// Helper: create a temp file that can be opened as an external buffer.
static std::string create_ws_temp_file(const std::string &name, const std::string &content) {
  auto dir = std::filesystem::temp_directory_path() / "vxcore_test_data" / "workspace_test";
  std::filesystem::create_directories(dir);
  auto path = dir / name;
  std::ofstream file(path, std::ios::binary);
  file << content;
  file.close();
  std::string result = path.string();
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

static void cleanup_ws_temp_files() {
  std::error_code ec;
  std::filesystem::remove_all(
      std::filesystem::temp_directory_path() / "vxcore_test_data" / "workspace_test", ec);
}

// Regression test: vxcore_prepare_shutdown() correctly persists workspace buffers.
// Verifies the "save session before closing buffers" pattern works.
int test_workspace_shutdown_saves_buffers() {
  std::cout << "  Running test_workspace_shutdown_saves_buffers..." << std::endl;

  vxcore_clear_test_directory();
  cleanup_ws_temp_files();

  std::string file_a = create_ws_temp_file("buf-a.md", "# Buffer A");
  std::string file_b = create_ws_temp_file("buf-b.md", "# Buffer B");

  std::string ws_id_str;
  std::string buf_a_id_str;
  std::string buf_b_id_str;

  // Session 1: Create workspace with buffers, shutdown to persist.
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *buf_a_id = nullptr;
    err = vxcore_buffer_open(ctx, nullptr, file_a.c_str(), &buf_a_id);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_NOT_NULL(buf_a_id);
    buf_a_id_str = buf_a_id;

    char *buf_b_id = nullptr;
    err = vxcore_buffer_open(ctx, nullptr, file_b.c_str(), &buf_b_id);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_NOT_NULL(buf_b_id);
    buf_b_id_str = buf_b_id;

    char *ws_id = nullptr;
    err = vxcore_workspace_create(ctx, "WS1", &ws_id);
    ASSERT_EQ(err, VXCORE_OK);
    ws_id_str = ws_id;

    err = vxcore_workspace_add_buffer(ctx, ws_id, buf_a_id);
    ASSERT_EQ(err, VXCORE_OK);
    err = vxcore_workspace_add_buffer(ctx, ws_id, buf_b_id);
    ASSERT_EQ(err, VXCORE_OK);

    err = vxcore_workspace_set_current(ctx, ws_id);
    ASSERT_EQ(err, VXCORE_OK);

    // Shutdown persists workspace state (including buffer list).
    err = vxcore_prepare_shutdown(ctx);
    ASSERT_EQ(err, VXCORE_OK);

    vxcore_string_free(buf_a_id);
    vxcore_string_free(buf_b_id);
    vxcore_string_free(ws_id);
    vxcore_context_destroy(ctx);
  }

  // Session 2: Reload and verify workspace + buffers survived.
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    // Workspace should exist.
    char *ws_json = nullptr;
    err = vxcore_workspace_get(ctx, ws_id_str.c_str(), &ws_json);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_NOT_NULL(ws_json);

    auto parsed = nlohmann::json::parse(ws_json);
    ASSERT_EQ(parsed["name"].get<std::string>(), std::string("WS1"));

    // bufferIds should contain both buffers.
    auto buffer_ids = parsed["bufferIds"];
    ASSERT_TRUE(buffer_ids.is_array());
    ASSERT_EQ(buffer_ids.size(), 2u);

    bool found_a = false;
    bool found_b = false;
    for (const auto &id : buffer_ids) {
      if (id.get<std::string>() == buf_a_id_str) found_a = true;
      if (id.get<std::string>() == buf_b_id_str) found_b = true;
    }
    ASSERT_TRUE(found_a);
    ASSERT_TRUE(found_b);

    // Current workspace should be WS1.
    char *current_ws = nullptr;
    err = vxcore_workspace_get_current(ctx, &current_ws);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_EQ(std::string(current_ws), ws_id_str);

    vxcore_string_free(current_ws);
    vxcore_string_free(ws_json);
    vxcore_context_destroy(ctx);
  }

  cleanup_ws_temp_files();
  std::cout << "  ✓ test_workspace_shutdown_saves_buffers passed" << std::endl;
  return 0;
}

// Regression test: removing all buffers then shutting down loses buffers (expected).
// This documents the vxcore behavior that caused the Qt-layer bug: if closeAllBuffers()
// runs before saveSession()/vxcore_prepare_shutdown(), the persisted workspace has no buffers.
int test_workspace_buffer_removal_then_shutdown() {
  std::cout << "  Running test_workspace_buffer_removal_then_shutdown..." << std::endl;

  vxcore_clear_test_directory();
  cleanup_ws_temp_files();

  std::string file_a = create_ws_temp_file("rem-a.md", "# Remove A");
  std::string file_b = create_ws_temp_file("rem-b.md", "# Remove B");

  std::string ws_id_str;

  // Session 1: Create workspace with buffers and persist.
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *buf_a_id = nullptr;
    err = vxcore_buffer_open(ctx, nullptr, file_a.c_str(), &buf_a_id);
    ASSERT_EQ(err, VXCORE_OK);

    char *buf_b_id = nullptr;
    err = vxcore_buffer_open(ctx, nullptr, file_b.c_str(), &buf_b_id);
    ASSERT_EQ(err, VXCORE_OK);

    char *ws_id = nullptr;
    err = vxcore_workspace_create(ctx, "WS1", &ws_id);
    ASSERT_EQ(err, VXCORE_OK);
    ws_id_str = ws_id;

    err = vxcore_workspace_add_buffer(ctx, ws_id, buf_a_id);
    ASSERT_EQ(err, VXCORE_OK);
    err = vxcore_workspace_add_buffer(ctx, ws_id, buf_b_id);
    ASSERT_EQ(err, VXCORE_OK);

    err = vxcore_workspace_set_current(ctx, ws_id);
    ASSERT_EQ(err, VXCORE_OK);

    err = vxcore_prepare_shutdown(ctx);
    ASSERT_EQ(err, VXCORE_OK);

    vxcore_string_free(buf_a_id);
    vxcore_string_free(buf_b_id);
    vxcore_string_free(ws_id);
    vxcore_context_destroy(ctx);
  }

  // Session 2: Reload, remove all buffers, then shutdown (the bug scenario).
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    // Workspace should have 2 buffers from session 1.
    char *ws_json = nullptr;
    err = vxcore_workspace_get(ctx, ws_id_str.c_str(), &ws_json);
    ASSERT_EQ(err, VXCORE_OK);

    auto parsed = nlohmann::json::parse(ws_json);
    auto buffer_ids = parsed["bufferIds"];
    ASSERT_EQ(buffer_ids.size(), 2u);
    vxcore_string_free(ws_json);

    // Remove all buffers from workspace (simulates closeAllBuffers).
    for (const auto &id : buffer_ids) {
      std::string buf_id = id.get<std::string>();
      err = vxcore_workspace_remove_buffer(ctx, ws_id_str.c_str(), buf_id.c_str());
      ASSERT_EQ(err, VXCORE_OK);
    }

    // Shutdown persists the now-empty workspace.
    err = vxcore_prepare_shutdown(ctx);
    ASSERT_EQ(err, VXCORE_OK);

    vxcore_context_destroy(ctx);
  }

  // Session 3: Reload and verify workspace has 0 buffers (expected vxcore behavior).
  // This is the scenario that the Qt-layer fix prevents: the caller must save session
  // BEFORE removing buffers from workspaces.
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    // Workspace should still exist but with no buffers.
    char *ws_json = nullptr;
    err = vxcore_workspace_get(ctx, ws_id_str.c_str(), &ws_json);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_NOT_NULL(ws_json);

    auto parsed = nlohmann::json::parse(ws_json);
    ASSERT_EQ(parsed["name"].get<std::string>(), std::string("WS1"));

    auto buffer_ids = parsed["bufferIds"];
    ASSERT_TRUE(buffer_ids.is_array());
    ASSERT_EQ(buffer_ids.size(), 0u);

    vxcore_string_free(ws_json);
    vxcore_context_destroy(ctx);
  }

  cleanup_ws_temp_files();
  std::cout << "  ✓ test_workspace_buffer_removal_then_shutdown passed" << std::endl;
  return 0;
}

int test_workspace_buffer_metadata_set_get() {
  std::cout << "  Running test_workspace_buffer_metadata_set_get..." << std::endl;

  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *ws_id = nullptr;
  err = vxcore_workspace_create(ctx, "MetaTest", &ws_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *buf_id = "buf-meta-1";
  err = vxcore_workspace_add_buffer(ctx, ws_id, buf_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Set per-buffer metadata.
  const char *meta_json = R"({"mode":"Edit","cursorPosition":42,"scrollPosition":150})";
  err = vxcore_workspace_set_buffer_metadata(ctx, ws_id, buf_id, meta_json);
  ASSERT_EQ(err, VXCORE_OK);

  // Get it back.
  char *out_json = nullptr;
  err = vxcore_workspace_get_buffer_metadata(ctx, ws_id, buf_id, &out_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_json);

  auto parsed = nlohmann::json::parse(out_json);
  ASSERT_EQ(parsed["mode"].get<std::string>(), std::string("Edit"));
  ASSERT_EQ(parsed["cursorPosition"].get<int>(), 42);
  ASSERT_EQ(parsed["scrollPosition"].get<int>(), 150);

  vxcore_string_free(out_json);
  vxcore_string_free(ws_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_buffer_metadata_set_get passed" << std::endl;
  return 0;
}

int test_workspace_buffer_metadata_missing_buffer() {
  std::cout << "  Running test_workspace_buffer_metadata_missing_buffer..." << std::endl;

  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *ws_id = nullptr;
  err = vxcore_workspace_create(ctx, "MetaMissing", &ws_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get metadata for a buffer that has none — should return empty object.
  char *out_json = nullptr;
  err = vxcore_workspace_get_buffer_metadata(ctx, ws_id, "no-such-buffer", &out_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_json);

  auto parsed = nlohmann::json::parse(out_json);
  ASSERT_TRUE(parsed.is_object());
  ASSERT_TRUE(parsed.empty());

  vxcore_string_free(out_json);
  vxcore_string_free(ws_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_buffer_metadata_missing_buffer passed" << std::endl;
  return 0;
}

int test_workspace_buffer_metadata_cleanup_on_remove() {
  std::cout << "  Running test_workspace_buffer_metadata_cleanup_on_remove..." << std::endl;

  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *ws_id = nullptr;
  err = vxcore_workspace_create(ctx, "MetaCleanup", &ws_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *buf_id = "buf-cleanup-1";
  err = vxcore_workspace_add_buffer(ctx, ws_id, buf_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Set metadata.
  err = vxcore_workspace_set_buffer_metadata(ctx, ws_id, buf_id,
                                             R"({"mode":"Edit"})");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify it exists.
  char *out_json = nullptr;
  err = vxcore_workspace_get_buffer_metadata(ctx, ws_id, buf_id, &out_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto parsed = nlohmann::json::parse(out_json);
  ASSERT_EQ(parsed["mode"].get<std::string>(), std::string("Edit"));
  vxcore_string_free(out_json);

  // Remove buffer from workspace — metadata should be cleaned up.
  err = vxcore_workspace_remove_buffer(ctx, ws_id, buf_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Metadata should now be empty.
  char *out_json2 = nullptr;
  err = vxcore_workspace_get_buffer_metadata(ctx, ws_id, buf_id, &out_json2);
  ASSERT_EQ(err, VXCORE_OK);
  auto parsed2 = nlohmann::json::parse(out_json2);
  ASSERT_TRUE(parsed2.empty());
  vxcore_string_free(out_json2);

  vxcore_string_free(ws_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_buffer_metadata_cleanup_on_remove passed" << std::endl;
  return 0;
}

int test_workspace_buffer_metadata_persistence() {
  std::cout << "  Running test_workspace_buffer_metadata_persistence..." << std::endl;

  vxcore_clear_test_directory();

  std::string ws_id_str;
  const std::string buf_id = "buf-persist-1";

  // Session 1: Create workspace, add buffer with metadata, shutdown.
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *ws_id = nullptr;
    err = vxcore_workspace_create(ctx, "MetaPersist", &ws_id);
    ASSERT_EQ(err, VXCORE_OK);
    ws_id_str = ws_id;

    err = vxcore_workspace_add_buffer(ctx, ws_id, buf_id.c_str());
    ASSERT_EQ(err, VXCORE_OK);

    err = vxcore_workspace_set_buffer_metadata(
        ctx, ws_id, buf_id.c_str(),
        R"({"mode":"Edit","cursorPosition":99})");
    ASSERT_EQ(err, VXCORE_OK);

    err = vxcore_prepare_shutdown(ctx);
    ASSERT_EQ(err, VXCORE_OK);

    vxcore_string_free(ws_id);
    vxcore_context_destroy(ctx);
  }

  // Session 2: Reload and verify metadata survived.
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *out_json = nullptr;
    err = vxcore_workspace_get_buffer_metadata(
        ctx, ws_id_str.c_str(), buf_id.c_str(), &out_json);
    ASSERT_EQ(err, VXCORE_OK);
    ASSERT_NOT_NULL(out_json);

    auto parsed = nlohmann::json::parse(out_json);
    ASSERT_EQ(parsed["mode"].get<std::string>(), std::string("Edit"));
    ASSERT_EQ(parsed["cursorPosition"].get<int>(), 99);

    vxcore_string_free(out_json);
    vxcore_context_destroy(ctx);
  }

  std::cout << "  ✓ test_workspace_buffer_metadata_persistence passed" << std::endl;
  return 0;
}

int test_workspace_buffer_metadata_in_workspace_json() {
  std::cout << "  Running test_workspace_buffer_metadata_in_workspace_json..." << std::endl;

  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *ws_id = nullptr;
  err = vxcore_workspace_create(ctx, "MetaInJson", &ws_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_workspace_add_buffer(ctx, ws_id, "b1");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_workspace_add_buffer(ctx, ws_id, "b2");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_workspace_set_buffer_metadata(ctx, ws_id, "b1",
                                             R"({"mode":"Edit"})");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_workspace_set_buffer_metadata(ctx, ws_id, "b2",
                                             R"({"mode":"Read","cursorPosition":10})");
  ASSERT_EQ(err, VXCORE_OK);

  // Get full workspace JSON and verify bufferMetadata is present.
  char *ws_json = nullptr;
  err = vxcore_workspace_get(ctx, ws_id, &ws_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto parsed = nlohmann::json::parse(ws_json);
  ASSERT_TRUE(parsed.contains("bufferMetadata"));
  ASSERT_TRUE(parsed["bufferMetadata"].is_object());
  ASSERT_EQ(parsed["bufferMetadata"]["b1"]["mode"].get<std::string>(), std::string("Edit"));
  ASSERT_EQ(parsed["bufferMetadata"]["b2"]["mode"].get<std::string>(), std::string("Read"));
  ASSERT_EQ(parsed["bufferMetadata"]["b2"]["cursorPosition"].get<int>(), 10);

  vxcore_string_free(ws_json);
  vxcore_string_free(ws_id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_workspace_buffer_metadata_in_workspace_json passed" << std::endl;
  return 0;
}

static std::filesystem::path ws_session_path() {
  return std::filesystem::temp_directory_path() / "vxcore_test_config" / "local" / "vxsession.json";
}

static nlohmann::json read_ws_session() {
  std::ifstream file(ws_session_path());
  return nlohmann::json::parse(file);
}

static bool ws_has_record(const nlohmann::json &records, const std::string &id) {
  return std::any_of(records.begin(), records.end(),
                     [&id](const auto &record) { return record.at("id") == id; });
}

static int check_ws_session(const nlohmann::json &session,
                            const std::vector<std::string> &workspace_ids,
                            const std::vector<std::string> &buffer_ids,
                            const std::string &current_id) {
  const auto &workspaces = session.at("workspaces");
  const auto &buffers = session.at("buffers");
  ASSERT_EQ(workspaces.size(), workspace_ids.size());
  ASSERT_EQ(buffers.size(), buffer_ids.size());
  for (const auto &id : workspace_ids) {
    ASSERT_TRUE(ws_has_record(workspaces, id));
  }
  for (const auto &workspace : workspaces) {
    ASSERT_TRUE(workspace.at("isPreview").is_boolean());
    ASSERT_FALSE(workspace.at("isPreview").get<bool>());
  }
  for (const auto &id : buffer_ids) {
    ASSERT_TRUE(ws_has_record(buffers, id));
  }
  ASSERT_EQ(session.at("currentWorkspaceId").get<std::string>(), current_id);
  return 0;
}

static int check_ws_runtime_buffers(VxCoreContextHandle ctx,
                                    const std::vector<std::string> &expected,
                                    const std::vector<std::string> &excluded = {}) {
  char *out = nullptr;
  ASSERT_EQ(vxcore_buffer_list(ctx, &out), VXCORE_OK);
  const auto buffers = nlohmann::json::parse(out);
  vxcore_string_free(out);
  ASSERT_EQ(buffers.size(), expected.size());
  for (const auto &id : expected) {
    ASSERT_TRUE(ws_has_record(buffers, id));
    out = nullptr;
    ASSERT_EQ(vxcore_buffer_get(ctx, id.c_str(), &out), VXCORE_OK);
    vxcore_string_free(out);
  }
  for (const auto &id : excluded) {
    out = nullptr;
    ASSERT_EQ(vxcore_buffer_get(ctx, id.c_str(), &out), VXCORE_ERR_BUFFER_NOT_FOUND);
    vxcore_string_free(out);
  }
  return 0;
}

static int open_ws_preview_test_buffers(VxCoreContextHandle ctx, std::vector<std::string> &ids) {
  for (const char *name :
       {"ordinary.md", "preview-a.md", "preview-b.md", "shared.md", "unowned.md"}) {
    const auto path = create_ws_temp_file(name, "# Preview persistence\n");
    char *id = nullptr;
    ASSERT_EQ(vxcore_buffer_open(ctx, nullptr, path.c_str(), &id), VXCORE_OK);
    ASSERT_NOT_NULL(id);
    ids.emplace_back(id);
    vxcore_string_free(id);
  }
  return 0;
}

int test_workspace_preview_creation_contract() {
  std::cout << "  Running test_workspace_preview_creation_contract..." << std::endl;
  vxcore_clear_test_directory();
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_workspace_create_preview(nullptr, "Preview", &id), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_workspace_create_preview(ctx, nullptr, &id), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_workspace_create_preview(ctx, "Preview", nullptr), VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(id);

  // Names and arbitrary metadata must not determine or mutate preview identity.
  ASSERT_EQ(vxcore_workspace_create(ctx, "Preview", &id), VXCORE_OK);
  const std::string ordinary_id(id);
  vxcore_string_free(id);
  id = nullptr;
  ASSERT_EQ(vxcore_workspace_create_preview(ctx, "Ordinary", &id), VXCORE_OK);
  ASSERT_NOT_NULL(id);
  const std::string preview_id(id);
  vxcore_string_free(id);
  ASSERT_EQ(vxcore_workspace_rename(ctx, preview_id.c_str(), "Renamed"), VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_set_metadata(ctx, preview_id.c_str(), R"({"isPreview":false})"),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_set_metadata(ctx, ordinary_id.c_str(), R"({"isPreview":true})"),
            VXCORE_OK);

  for (const auto &workspace_id : {ordinary_id, preview_id}) {
    char *out = nullptr;
    ASSERT_EQ(vxcore_workspace_get(ctx, workspace_id.c_str(), &out), VXCORE_OK);
    const auto workspace = nlohmann::json::parse(out);
    vxcore_string_free(out);
    ASSERT_TRUE(workspace.at("isPreview").is_boolean());
    ASSERT_EQ(workspace.at("isPreview").get<bool>(), workspace_id == preview_id);
  }
  char *out = nullptr;
  ASSERT_EQ(vxcore_workspace_list(ctx, &out), VXCORE_OK);
  const auto workspaces = nlohmann::json::parse(out);
  vxcore_string_free(out);
  ASSERT_EQ(workspaces.size(), 2u);
  for (const auto &workspace : workspaces) {
    ASSERT_EQ(workspace.at("isPreview").get<bool>(), workspace.at("id") == preview_id);
  }
  ASSERT_EQ(check_ws_session(read_ws_session(), {ordinary_id}, {}, ""), 0);
  vxcore_context_destroy(ctx);
  return 0;
}

int test_workspace_preview_runtime_persistence() {
  std::cout << "  Running test_workspace_preview_runtime_persistence..." << std::endl;
  vxcore_clear_test_directory();
  cleanup_ws_temp_files();
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  std::vector<std::string> buffers;
  ASSERT_EQ(open_ws_preview_test_buffers(ctx, buffers), 0);
  const auto &ordinary_buffer = buffers[0];
  const auto &preview_buffer = buffers[1];
  const auto &other_preview_buffer = buffers[2];
  const auto &shared_buffer = buffers[3];
  const auto &unowned_buffer = buffers[4];

  char *id = nullptr;
  ASSERT_EQ(vxcore_workspace_create(ctx, "Ordinary", &id), VXCORE_OK);
  const std::string ordinary_id(id);
  vxcore_string_free(id);
  ASSERT_EQ(vxcore_workspace_create(ctx, "Other ordinary", &id), VXCORE_OK);
  const std::string other_ordinary_id(id);
  vxcore_string_free(id);
  ASSERT_EQ(vxcore_workspace_create_preview(ctx, "Preview A", &id), VXCORE_OK);
  const std::string preview_id(id);
  vxcore_string_free(id);
  ASSERT_EQ(vxcore_workspace_create_preview(ctx, "Preview B", &id), VXCORE_OK);
  const std::string other_preview_id(id);
  vxcore_string_free(id);

  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, ordinary_id.c_str(), ordinary_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, ordinary_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, preview_id.c_str(), preview_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, preview_id.c_str(), shared_buffer.c_str()), VXCORE_OK);
  ASSERT_EQ(
      vxcore_workspace_add_buffer(ctx, other_preview_id.c_str(), other_preview_buffer.c_str()),
      VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, other_preview_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_set_current_buffer(ctx, ordinary_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_set_current_buffer(ctx, preview_id.c_str(), preview_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_set_current(ctx, preview_id.c_str()), VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_rename(ctx, ordinary_id.c_str(), "Saved while preview is live"),
            VXCORE_OK);

  char *out = nullptr;
  ASSERT_EQ(vxcore_workspace_list(ctx, &out), VXCORE_OK);
  const auto live_workspaces = nlohmann::json::parse(out);
  vxcore_string_free(out);
  ASSERT_EQ(live_workspaces.size(), 4u);
  std::string first_ordinary_id;
  for (const auto &workspace : live_workspaces) {
    if (!workspace.at("isPreview").get<bool>() && first_ordinary_id.empty()) {
      first_ordinary_id = workspace.at("id").get<std::string>();
    }
    if (workspace.at("isPreview").get<bool>()) {
      ASSERT_EQ(workspace.at("bufferIds").size(), 2u);
      ASSERT_TRUE(std::find(workspace.at("bufferIds").begin(), workspace.at("bufferIds").end(),
                            shared_buffer) != workspace.at("bufferIds").end());
    }
  }
  const std::vector<std::string> retained = {ordinary_buffer, shared_buffer, unowned_buffer};
  ASSERT_EQ(check_ws_session(read_ws_session(), {ordinary_id, other_ordinary_id}, retained,
                             first_ordinary_id),
            0);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, buffers), 0);
  out = nullptr;
  ASSERT_EQ(vxcore_workspace_get_current(ctx, &out), VXCORE_OK);
  ASSERT_EQ(std::string(out), preview_id);
  vxcore_string_free(out);

  // Runtime ownership must still see preview owners after the ordinary owner closes.
  ASSERT_EQ(vxcore_workspace_remove_buffer(ctx, ordinary_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_remove_buffer(ctx, preview_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, buffers), 0);
  ASSERT_EQ(check_ws_session(read_ws_session(), {ordinary_id, other_ordinary_id},
                             {ordinary_buffer, unowned_buffer}, first_ordinary_id),
            0);
  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, ordinary_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_remove_buffer(ctx, other_preview_id.c_str(), shared_buffer.c_str()),
            VXCORE_OK);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, buffers), 0);

  ASSERT_EQ(vxcore_prepare_shutdown(ctx), VXCORE_OK);
  const auto snapshot = read_ws_session();
  ASSERT_EQ(
      check_ws_session(snapshot, {ordinary_id, other_ordinary_id}, retained, first_ordinary_id), 0);
  // Closing live buffers after the shutdown snapshot must not publish previews or erase it.
  ASSERT_EQ(vxcore_workspace_remove_buffer(ctx, preview_id.c_str(), preview_buffer.c_str()),
            VXCORE_OK);
  vxcore_context_destroy(ctx);
  ASSERT_EQ(read_ws_session(), snapshot);

  ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, retained, {preview_buffer, other_preview_buffer}), 0);
  for (const auto &workspace_id : {preview_id, other_preview_id}) {
    out = nullptr;
    ASSERT_EQ(vxcore_workspace_get(ctx, workspace_id.c_str(), &out),
              VXCORE_ERR_WORKSPACE_NOT_FOUND);
    vxcore_string_free(out);
  }
  out = nullptr;
  ASSERT_EQ(vxcore_workspace_get(ctx, ordinary_id.c_str(), &out), VXCORE_OK);
  const auto ordinary = nlohmann::json::parse(out);
  vxcore_string_free(out);
  ASSERT_EQ(ordinary.at("name").get<std::string>(), std::string("Saved while preview is live"));
  ASSERT_EQ(ordinary.at("bufferIds"), nlohmann::json::array({ordinary_buffer, shared_buffer}));
  out = nullptr;
  ASSERT_EQ(vxcore_workspace_get_current(ctx, &out), VXCORE_OK);
  ASSERT_EQ(std::string(out), first_ordinary_id);
  vxcore_string_free(out);
  vxcore_context_destroy(ctx);
  cleanup_ws_temp_files();
  return 0;
}

int test_workspace_preview_only_current_is_empty() {
  std::cout << "  Running test_workspace_preview_only_current_is_empty..." << std::endl;
  vxcore_clear_test_directory();
  cleanup_ws_temp_files();
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *workspace_id = nullptr;
  ASSERT_EQ(vxcore_workspace_create_preview(ctx, "Only preview", &workspace_id), VXCORE_OK);
  const auto path = create_ws_temp_file("only-preview.md", "# Only preview\n");
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, nullptr, path.c_str(), &buffer_id), VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_add_buffer(ctx, workspace_id, buffer_id), VXCORE_OK);
  ASSERT_EQ(vxcore_workspace_set_current(ctx, workspace_id), VXCORE_OK);
  ASSERT_EQ(check_ws_session(read_ws_session(), {}, {}, ""), 0);
  char *current_id = nullptr;
  ASSERT_EQ(vxcore_workspace_get_current(ctx, &current_id), VXCORE_OK);
  ASSERT_EQ(std::string(current_id), std::string(workspace_id));
  vxcore_string_free(current_id);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, {buffer_id}), 0);
  ASSERT_EQ(vxcore_prepare_shutdown(ctx), VXCORE_OK);
  vxcore_string_free(workspace_id);
  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  ASSERT_EQ(check_ws_session(read_ws_session(), {}, {}, ""), 0);

  ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, {}), 0);
  current_id = nullptr;
  ASSERT_EQ(vxcore_workspace_get_current(ctx, &current_id), VXCORE_OK);
  ASSERT_EQ(std::string(current_id), std::string());
  vxcore_string_free(current_id);
  vxcore_context_destroy(ctx);
  cleanup_ws_temp_files();
  return 0;
}

int test_workspace_saved_preview_records_are_filtered() {
  std::cout << "  Running test_workspace_saved_preview_records_are_filtered..." << std::endl;
  // Exercise both ordinary fallback (in saved order, not map order) and no fallback.
  for (const bool with_ordinary : {true, false}) {
    vxcore_clear_test_directory();
    cleanup_ws_temp_files();
    VxCoreContextHandle ctx = nullptr;
    ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
    std::vector<std::string> buffers;
    ASSERT_EQ(open_ws_preview_test_buffers(ctx, buffers), 0);
    ASSERT_EQ(vxcore_prepare_shutdown(ctx), VXCORE_OK);
    vxcore_context_destroy(ctx);
    auto session = read_ws_session();
    session["workspaces"] = nlohmann::json::array({{{"id", "preview-a"},
                                                    {"name", "Preview A"},
                                                    {"isPreview", true},
                                                    {"bufferIds", {buffers[1], buffers[3]}},
                                                    {"currentBufferId", buffers[1]}},
                                                   {{"id", "preview-b"},
                                                    {"name", "Preview B"},
                                                    {"isPreview", true},
                                                    {"bufferIds", {buffers[2], buffers[3]}}}});
    std::vector<std::string> expected_workspaces;
    std::vector<std::string> retained = {buffers[0], buffers[4]};
    std::vector<std::string> excluded = {buffers[1], buffers[2]};
    std::string expected_current;
    if (with_ordinary) {
      session["workspaces"].push_back({{"id", "z-first"},
                                       {"name", "First ordinary"},
                                       {"isPreview", false},
                                       {"bufferIds", {buffers[0], buffers[3]}},
                                       {"currentBufferId", buffers[3]}});
      session["workspaces"].push_back({{"id", "a-second"}, {"name", "Second ordinary"}});
      expected_workspaces = {"z-first", "a-second"};
      expected_current = "z-first";
      retained.push_back(buffers[3]);
    } else {
      excluded.push_back(buffers[3]);
    }
    session["currentWorkspaceId"] = "preview-a";
    {
      std::ofstream file(ws_session_path());
      ASSERT_TRUE(file.is_open());
      file << session.dump(2);
    }

    ctx = nullptr;
    ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
    ASSERT_EQ(check_ws_runtime_buffers(ctx, retained, excluded), 0);
    char *out = nullptr;
    ASSERT_EQ(vxcore_workspace_list(ctx, &out), VXCORE_OK);
    const auto workspaces = nlohmann::json::parse(out);
    vxcore_string_free(out);
    ASSERT_EQ(workspaces.size(), expected_workspaces.size());
    for (const auto &id : expected_workspaces) {
      ASSERT_TRUE(ws_has_record(workspaces, id));
    }
    for (const char *id : {"preview-a", "preview-b"}) {
      out = nullptr;
      ASSERT_EQ(vxcore_workspace_get(ctx, id, &out), VXCORE_ERR_WORKSPACE_NOT_FOUND);
      vxcore_string_free(out);
    }
    out = nullptr;
    ASSERT_EQ(vxcore_workspace_get_current(ctx, &out), VXCORE_OK);
    ASSERT_EQ(std::string(out), expected_current);
    vxcore_string_free(out);
    if (with_ordinary) {
      out = nullptr;
      ASSERT_EQ(vxcore_workspace_get(ctx, "z-first", &out), VXCORE_OK);
      const auto ordinary = nlohmann::json::parse(out);
      vxcore_string_free(out);
      ASSERT_EQ(ordinary.at("bufferIds"), nlohmann::json::array({buffers[0], buffers[3]}));
      ASSERT_EQ(ordinary.at("currentBufferId").get<std::string>(), buffers[3]);
      ASSERT_EQ(vxcore_workspace_rename(ctx, "z-first", "Restored ordinary"), VXCORE_OK);
      ASSERT_EQ(
          check_ws_session(read_ws_session(), expected_workspaces, retained, expected_current), 0);
    }
    ASSERT_EQ(vxcore_prepare_shutdown(ctx), VXCORE_OK);
    ASSERT_EQ(check_ws_session(read_ws_session(), expected_workspaces, retained, expected_current),
              0);
    vxcore_context_destroy(ctx);
    cleanup_ws_temp_files();
  }
  return 0;
}

int test_workspace_legacy_and_malformed_preview_restore() {
  std::cout << "  Running test_workspace_legacy_and_malformed_preview_restore..." << std::endl;
  vxcore_clear_test_directory();
  cleanup_ws_temp_files();
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  const auto path = create_ws_temp_file("legacy-preview.md", "# Legacy workspace\n");
  char *id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, nullptr, path.c_str(), &id), VXCORE_OK);
  const std::string buffer_id(id);
  vxcore_string_free(id);
  ASSERT_EQ(vxcore_prepare_shutdown(ctx), VXCORE_OK);
  vxcore_context_destroy(ctx);
  auto session = read_ws_session();
  session["workspaces"] = nlohmann::json::array();
  // Missing, explicit false, null, strings, numbers, arrays and objects are ordinary.
  const auto values = nlohmann::json::array(
      {false, nullptr, "true", 1, 0, 1.5, nlohmann::json::array(), nlohmann::json::object()});
  std::vector<std::string> workspace_ids;
  for (size_t index = 0; index <= values.size(); ++index) {
    const auto workspace_id = "ordinary-" + std::to_string(index);
    nlohmann::json workspace = {{"id", workspace_id},
                                {"name", "Preview"},
                                {"bufferIds", {buffer_id}},
                                {"currentBufferId", buffer_id},
                                {"metadata", {{"isPreview", true}}}};
    if (index != 0) {
      workspace["isPreview"] = values[index - 1];
    }
    workspace_ids.push_back(workspace_id);
    session["workspaces"].push_back(std::move(workspace));
  }
  session["currentWorkspaceId"] = workspace_ids.back();
  {
    std::ofstream file(ws_session_path());
    ASSERT_TRUE(file.is_open());
    file << session.dump(2);
  }
  ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_EQ(check_ws_runtime_buffers(ctx, {buffer_id}), 0);
  for (const auto &workspace_id : workspace_ids) {
    char *out = nullptr;
    ASSERT_EQ(vxcore_workspace_get(ctx, workspace_id.c_str(), &out), VXCORE_OK);
    const auto workspace = nlohmann::json::parse(out);
    vxcore_string_free(out);
    ASSERT_TRUE(workspace.at("isPreview").is_boolean());
    ASSERT_FALSE(workspace.at("isPreview").get<bool>());
    ASSERT_EQ(workspace.at("bufferIds"), nlohmann::json::array({buffer_id}));
    ASSERT_EQ(workspace.at("currentBufferId").get<std::string>(), buffer_id);
  }
  char *current = nullptr;
  ASSERT_EQ(vxcore_workspace_get_current(ctx, &current), VXCORE_OK);
  ASSERT_EQ(std::string(current), workspace_ids.back());
  vxcore_string_free(current);
  ASSERT_EQ(vxcore_prepare_shutdown(ctx), VXCORE_OK);
  ASSERT_EQ(check_ws_session(read_ws_session(), workspace_ids, {buffer_id}, workspace_ids.back()),
            0);
  vxcore_context_destroy(ctx);
  cleanup_ws_temp_files();
  return 0;
}

int main() {
  std::cout << "Running workspace tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_workspace_empty_initially);
  RUN_TEST(test_workspace_create);
  RUN_TEST(test_workspace_get);
  RUN_TEST(test_workspace_list);
  RUN_TEST(test_workspace_rename);
  RUN_TEST(test_workspace_delete);
  RUN_TEST(test_workspace_current);
  RUN_TEST(test_workspace_buffer_add_remove);
  RUN_TEST(test_workspace_current_buffer);
  RUN_TEST(test_workspace_persistence);
  RUN_TEST(test_workspace_shutdown_saves_buffers);
  RUN_TEST(test_workspace_buffer_removal_then_shutdown);
  RUN_TEST(test_workspace_buffer_metadata_set_get);
  RUN_TEST(test_workspace_buffer_metadata_missing_buffer);
  RUN_TEST(test_workspace_buffer_metadata_cleanup_on_remove);
  RUN_TEST(test_workspace_buffer_metadata_persistence);
  RUN_TEST(test_workspace_buffer_metadata_in_workspace_json);
  RUN_TEST(test_workspace_preview_creation_contract);
  RUN_TEST(test_workspace_preview_runtime_persistence);
  RUN_TEST(test_workspace_preview_only_current_is_empty);
  RUN_TEST(test_workspace_saved_preview_records_are_filtered);
  RUN_TEST(test_workspace_legacy_and_malformed_preview_restore);

  std::cout << "All workspace tests passed!" << std::endl;
  return 0;
}
