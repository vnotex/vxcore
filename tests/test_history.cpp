#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "core/context.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_history_get_empty() {
  std::cout << "  Running test_history_get_empty..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_empty"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_empty").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(history_json);

  auto arr = nlohmann::json::parse(history_json);
  ASSERT_TRUE(arr.is_array());
  ASSERT_EQ(arr.size(), static_cast<size_t>(0));

  vxcore_string_free(history_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_empty"));
  std::cout << "  ✓ test_history_get_empty passed" << std::endl;
  return 0;
}

int test_history_record_and_get() {
  std::cout << "  Running test_history_record_and_get..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_record"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_record").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer (triggers history recording)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Check history
  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);

  auto arr = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr.size(), static_cast<size_t>(1));
  ASSERT_EQ(arr[0]["fileId"].get<std::string>(), std::string(file_id));
  ASSERT_TRUE(arr[0]["openedUtc"].get<int64_t>() > 0);

  vxcore_string_free(history_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_record"));
  std::cout << "  ✓ test_history_record_and_get passed" << std::endl;
  return 0;
}

int test_history_dedup() {
  std::cout << "  Running test_history_dedup..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_dedup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_dedup").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get timestamp of first open
  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr1 = nlohmann::json::parse(history_json);
  int64_t first_ts = arr1[0]["openedUtc"].get<int64_t>();
  vxcore_string_free(history_json);

  // Close buffer, sleep, re-open
  err = vxcore_buffer_close(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Check: still 1 entry, timestamp updated
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr2 = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr2.size(), static_cast<size_t>(1));
  ASSERT_TRUE(arr2[0]["openedUtc"].get<int64_t>() >= first_ts);

  vxcore_string_free(history_json);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_dedup"));
  std::cout << "  ✓ test_history_dedup passed" << std::endl;
  return 0;
}

int test_history_ordering() {
  std::cout << "  Running test_history_ordering..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_order"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_order").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create 3 files
  char *file_a = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "a.md", &file_a);
  ASSERT_EQ(err, VXCORE_OK);
  char *file_b = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "b.md", &file_b);
  ASSERT_EQ(err, VXCORE_OK);
  char *file_c = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "c.md", &file_c);
  ASSERT_EQ(err, VXCORE_OK);

  // Open A, B, C
  char *buf_a = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "a.md", &buf_a);
  ASSERT_EQ(err, VXCORE_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  char *buf_b = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "b.md", &buf_b);
  ASSERT_EQ(err, VXCORE_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  char *buf_c = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "c.md", &buf_c);
  ASSERT_EQ(err, VXCORE_OK);

  // Order should be [C, B, A]
  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr.size(), static_cast<size_t>(3));
  ASSERT_EQ(arr[0]["fileId"].get<std::string>(), std::string(file_c));
  ASSERT_EQ(arr[1]["fileId"].get<std::string>(), std::string(file_b));
  ASSERT_EQ(arr[2]["fileId"].get<std::string>(), std::string(file_a));
  vxcore_string_free(history_json);

  // Close B, re-open B → order should be [B, C, A]
  err = vxcore_buffer_close(ctx, buf_b);
  ASSERT_EQ(err, VXCORE_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  char *buf_b2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "b.md", &buf_b2);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr2 = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr2.size(), static_cast<size_t>(3));
  ASSERT_EQ(arr2[0]["fileId"].get<std::string>(), std::string(file_b));
  ASSERT_EQ(arr2[1]["fileId"].get<std::string>(), std::string(file_c));
  ASSERT_EQ(arr2[2]["fileId"].get<std::string>(), std::string(file_a));
  vxcore_string_free(history_json);

  vxcore_string_free(buf_b2);
  vxcore_string_free(buf_c);
  vxcore_string_free(buf_b);
  vxcore_string_free(buf_a);
  vxcore_string_free(file_c);
  vxcore_string_free(file_b);
  vxcore_string_free(file_a);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_order"));
  std::cout << "  ✓ test_history_ordering passed" << std::endl;
  return 0;
}

int test_history_max_items() {
  std::cout << "  Running test_history_max_items..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_max"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_max").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create 51 files and open each
  std::vector<std::string> file_ids;
  for (int i = 0; i < 51; i++) {
    std::string name = "file_" + std::to_string(i) + ".md";
    char *fid = nullptr;
    err = vxcore_file_create(ctx, notebook_id, ".", name.c_str(), &fid);
    ASSERT_EQ(err, VXCORE_OK);
    file_ids.push_back(fid);
    vxcore_string_free(fid);

    char *bid = nullptr;
    err = vxcore_buffer_open(ctx, notebook_id, name.c_str(), &bid);
    ASSERT_EQ(err, VXCORE_OK);
    // Close each buffer so next open creates fresh buffer
    err = vxcore_buffer_close(ctx, bid);
    ASSERT_EQ(err, VXCORE_OK);
    vxcore_string_free(bid);
  }

  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr.size(), static_cast<size_t>(50));

  // First file (file_0) should be evicted
  for (const auto &entry : arr) {
    ASSERT_NE(entry["fileId"].get<std::string>(), file_ids[0]);
  }

  vxcore_string_free(history_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_max"));
  std::cout << "  ✓ test_history_max_items passed" << std::endl;
  return 0;
}

int test_history_clear() {
  std::cout << "  Running test_history_clear..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_clear"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_clear").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Clear history
  err = vxcore_notebook_history_clear(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // History should be empty
  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr.size(), static_cast<size_t>(0));

  vxcore_string_free(history_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_clear"));
  std::cout << "  ✓ test_history_clear passed" << std::endl;
  return 0;
}

int test_history_persists() {
  std::cout << "  Running test_history_persists..." << std::endl;
  std::string nb_path = get_test_path("test_history_persist");
  cleanup_test_dir(nb_path);

  std::string saved_notebook_id;
  std::string saved_file_id;

  // First session: create notebook, file, open buffer
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *notebook_id = nullptr;
    err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"History Test\"}",
                                 VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
    ASSERT_EQ(err, VXCORE_OK);
    saved_notebook_id = notebook_id;

    char *file_id = nullptr;
    err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
    ASSERT_EQ(err, VXCORE_OK);
    saved_file_id = file_id;

    char *buffer_id = nullptr;
    err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
    ASSERT_EQ(err, VXCORE_OK);

    vxcore_string_free(buffer_id);
    vxcore_string_free(file_id);
    vxcore_string_free(notebook_id);
    vxcore_context_destroy(ctx);
  }

  // Second session: re-open notebook, check history persists
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *notebook_id = nullptr;
    err = vxcore_notebook_open(ctx, nb_path.c_str(), &notebook_id);
    ASSERT_EQ(err, VXCORE_OK);

    char *history_json = nullptr;
    err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
    ASSERT_EQ(err, VXCORE_OK);

    auto arr = nlohmann::json::parse(history_json);
    ASSERT_EQ(arr.size(), static_cast<size_t>(1));
    ASSERT_EQ(arr[0]["fileId"].get<std::string>(), saved_file_id);

    vxcore_string_free(history_json);
    vxcore_string_free(notebook_id);
    vxcore_context_destroy(ctx);
  }

  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_history_persists passed" << std::endl;
  return 0;
}

int test_history_skip_external() {
  std::cout << "  Running test_history_skip_external..." << std::endl;
  cleanup_test_dir(get_test_path("test_history_external"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_history_external").c_str(),
                               "{\"name\":\"History Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create an external file outside the notebook
  std::string ext_dir = get_test_path("test_history_ext_files");
  create_directory(ext_dir);
  std::string ext_file = ext_dir + "/external.md";
  write_file(ext_file, "external content");

  // Open external file (NULL notebook_id → empty string in C API)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, ext_file.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Notebook history should remain empty
  char *history_json = nullptr;
  err = vxcore_notebook_history_get(ctx, notebook_id, &history_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto arr = nlohmann::json::parse(history_json);
  ASSERT_EQ(arr.size(), static_cast<size_t>(0));

  vxcore_string_free(history_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_history_external"));
  cleanup_test_dir(ext_dir);
  std::cout << "  ✓ test_history_skip_external passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running history tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_history_get_empty);
  RUN_TEST(test_history_record_and_get);
  RUN_TEST(test_history_dedup);
  RUN_TEST(test_history_ordering);
  RUN_TEST(test_history_max_items);
  RUN_TEST(test_history_clear);
  RUN_TEST(test_history_persists);
  RUN_TEST(test_history_skip_external);

  std::cout << "All history tests passed!" << std::endl;
  return 0;
}
