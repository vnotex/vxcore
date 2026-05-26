// Tests for the vxcore_log_set_handler C API (Task 6 of
// vxcore-log-handler-api plan). Verifies install / uninstall, userdata
// passthrough, log-level preservation, and thread-safety.
//
// Emission strategy:
//   We trigger logs by calling public C API functions that are documented to
//   emit VXCORE_LOG_* macros. Two paths reachable through public API:
//     - vxcore_notebook_open(ctx, "<missing>") -> emits INFO ("Opening
//       notebook: ...") then WARN ("Notebook root folder not found: ...").
//     - vxcore_notebook_close(ctx, "<bogus-id>") -> emits WARN
//       ("Notebook not found for closing: ...").
//
//   LIMITATION: TRACE / DEBUG / ERROR / FATAL levels are not easily reachable
//   from the public C API surface without provoking exceptions or constructing
//   elaborate fixtures, so the level-passthrough test asserts INFO and WARN.
//   That is sufficient to prove the C `VxCoreLogLevel` enum value is forwarded
//   unmolested through the handler.

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_log.h"

namespace {

struct CapturedRecord {
  VxCoreLogLevel level;
  std::string file;
  int line;
  std::string message;
};

std::mutex g_capture_mutex;
std::vector<CapturedRecord> g_captures;
// Userdata observed by the most recent invocation of g_capture_callback;
// captured under g_capture_mutex.
void *g_last_userdata = nullptr;

void g_capture_callback(VxCoreLogLevel level, const char *file, int line, const char *message,
                        void *userdata) {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  CapturedRecord rec;
  rec.level = level;
  rec.file = file ? file : "";
  rec.line = line;
  rec.message = message ? message : "";
  g_captures.push_back(std::move(rec));
  g_last_userdata = userdata;
}

void reset_captures() {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  g_captures.clear();
  g_last_userdata = nullptr;
}

size_t capture_count() {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  return g_captures.size();
}

// Emits exactly one WARN log via vxcore_notebook_close on an unknown id.
void emit_one_warn(VxCoreContextHandle ctx) {
  // notebook_manager.cpp line 195: VXCORE_LOG_WARN("Notebook not found for
  // closing: id=%s", ...) -- ignore return value (we expect NOT_FOUND).
  (void)vxcore_notebook_close(ctx, "nonexistent-notebook-id-xyz");
}

}  // namespace

int test_install_capture_uninstall() {
  std::cout << "  Running test_install_capture_uninstall..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  reset_captures();
  ASSERT_EQ(vxcore_log_set_level(VXCORE_LOG_LEVEL_TRACE), VXCORE_OK);
  ASSERT_EQ(vxcore_log_set_handler(g_capture_callback, nullptr), VXCORE_OK);

  emit_one_warn(ctx);
  size_t after_first = capture_count();
  ASSERT(after_first >= 1);

  // Uninstall: pass nullptr.
  ASSERT_EQ(vxcore_log_set_handler(nullptr, nullptr), VXCORE_OK);
  size_t before_more = capture_count();
  emit_one_warn(ctx);
  emit_one_warn(ctx);
  size_t after_more = capture_count();
  ASSERT_EQ(before_more, after_more);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_install_capture_uninstall passed" << std::endl;
  return 0;
}

int test_userdata_passthrough() {
  std::cout << "  Running test_userdata_passthrough..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  reset_captures();
  int my_marker = 4242;
  ASSERT_EQ(vxcore_log_set_level(VXCORE_LOG_LEVEL_TRACE), VXCORE_OK);
  ASSERT_EQ(vxcore_log_set_handler(g_capture_callback, &my_marker), VXCORE_OK);

  emit_one_warn(ctx);
  ASSERT(capture_count() >= 1);

  {
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    ASSERT_EQ(g_last_userdata, static_cast<void *>(&my_marker));
    int *as_int = static_cast<int *>(g_last_userdata);
    ASSERT_EQ(*as_int, 4242);
  }

  ASSERT_EQ(vxcore_log_set_handler(nullptr, nullptr), VXCORE_OK);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_userdata_passthrough passed" << std::endl;
  return 0;
}

int test_level_passthrough() {
  std::cout << "  Running test_level_passthrough..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  reset_captures();
  ASSERT_EQ(vxcore_log_set_level(VXCORE_LOG_LEVEL_TRACE), VXCORE_OK);
  ASSERT_EQ(vxcore_log_set_handler(g_capture_callback, nullptr), VXCORE_OK);

  // vxcore_notebook_open on a missing path emits:
  //   - INFO  "Opening notebook: root_folder=..." (notebook_manager.cpp:147)
  //   - WARN  "Notebook root folder not found: ..." (notebook_manager.cpp:157)
  // Two distinct levels in a single call -- sufficient to prove the C
  // enum value is preserved through the handler boundary.
  char *out_id = nullptr;
  (void)vxcore_notebook_open(ctx, "Z:/__vxcore_log_handler_test_missing_path__", &out_id);
  if (out_id) {
    vxcore_string_free(out_id);
  }

  bool saw_info = false;
  bool saw_warn = false;
  {
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    for (const auto &rec : g_captures) {
      if (rec.level == VXCORE_LOG_LEVEL_INFO) saw_info = true;
      if (rec.level == VXCORE_LOG_LEVEL_WARN) saw_warn = true;
    }
  }
  ASSERT_TRUE(saw_info);
  ASSERT_TRUE(saw_warn);

  ASSERT_EQ(vxcore_log_set_handler(nullptr, nullptr), VXCORE_OK);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_level_passthrough passed" << std::endl;
  return 0;
}

int test_thread_safety() {
  std::cout << "  Running test_thread_safety..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  reset_captures();
  // Filter to WARN+: emit_one_warn issues an INFO ("Closing notebook")
  // followed by a WARN ("Notebook not found for closing"). Setting the
  // level to WARN restricts captures to exactly one entry per call so the
  // assertion below can pin the total to kThreads * kIters.
  ASSERT_EQ(vxcore_log_set_level(VXCORE_LOG_LEVEL_WARN), VXCORE_OK);
  ASSERT_EQ(vxcore_log_set_handler(g_capture_callback, nullptr), VXCORE_OK);
  // 4 threads x 100 iterations -- with the level at WARN, each
  // vxcore_notebook_close call on a missing id captures exactly one
  // WARN ("Notebook not found for closing"). Total expected: 400.
  // Logger serializes Log calls with its own mutex, so torn messages are
  // structurally impossible; we still verify each captured message starts
  // with the expected stable prefix to make any future regression loud.
  constexpr int kThreads = 4;
  constexpr int kIters = 100;
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([ctx, kIters]() {
      for (int i = 0; i < kIters; ++i) {
        emit_one_warn(ctx);
      }
    });
  }
  for (auto &w : workers) {
    w.join();
  }

  std::lock_guard<std::mutex> lock(g_capture_mutex);
  ASSERT_EQ(g_captures.size(), static_cast<size_t>(kThreads * kIters));
  const std::string kExpectedPrefix = "Notebook not found for closing: id=";
  for (const auto &rec : g_captures) {
    ASSERT_EQ(rec.level, VXCORE_LOG_LEVEL_WARN);
    ASSERT(rec.message.size() >= kExpectedPrefix.size());
    ASSERT_EQ(rec.message.compare(0, kExpectedPrefix.size(), kExpectedPrefix), 0);
  }

  ASSERT_EQ(vxcore_log_set_handler(nullptr, nullptr), VXCORE_OK);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_thread_safety passed" << std::endl;
  return 0;
}

int test_null_uninstall_restores_default() {
  std::cout << "  Running test_null_uninstall_restores_default..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  reset_captures();
  ASSERT_EQ(vxcore_log_set_level(VXCORE_LOG_LEVEL_TRACE), VXCORE_OK);
  ASSERT_EQ(vxcore_log_set_handler(g_capture_callback, nullptr), VXCORE_OK);

  emit_one_warn(ctx);
  size_t baseline = capture_count();
  ASSERT(baseline >= 1);

  ASSERT_EQ(vxcore_log_set_handler(nullptr, nullptr), VXCORE_OK);
  for (int i = 0; i < 5; ++i) {
    emit_one_warn(ctx);
  }
  ASSERT_EQ(capture_count(), baseline);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_null_uninstall_restores_default passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running log handler tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_install_capture_uninstall);
  RUN_TEST(test_userdata_passthrough);
  RUN_TEST(test_level_passthrough);
  RUN_TEST(test_thread_safety);
  RUN_TEST(test_null_uninstall_restores_default);

  // Defensive: leave the global logger in default state.
  vxcore_log_set_handler(nullptr, nullptr);

  std::cout << "✓ All log handler tests passed" << std::endl;
  return 0;
}
