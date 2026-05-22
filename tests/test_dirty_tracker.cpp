// Standalone unit test for vxcore::DirtyTracker (Task 9.1 / F2.4).
//
// DirtyTracker is direct-compiled into this test executable rather than
// linked via vxcore.dll because its symbols are not VXCORE_API-exported.

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "sync/dirty_tracker.h"
#include "test_utils.h"

// vxcore_set_test_mode is exported by vxcore.dll, but this test does not link
// vxcore.dll (we direct-compile dirty_tracker.cpp only). Provide a no-op shim
// so the call in main() links cleanly. Same pattern used by test_gitkeep_basic.
namespace {
void vxcore_set_test_mode(int) {}
}  // namespace

using vxcore::DirtyTracker;

static int test_mark_and_has() {
  DirtyTracker dt;
  dt.MarkDirty("A", "p1");
  dt.MarkDirty("A", "p2");
  dt.MarkDirty("B", "q1");

  ASSERT_TRUE(dt.HasDirty("A"));
  ASSERT_TRUE(dt.HasDirty("B"));
  ASSERT_FALSE(dt.HasDirty("C"));

  // Idempotent insert (set semantics): re-marking same path doesn't add.
  dt.MarkDirty("A", "p1");
  ASSERT_TRUE(dt.HasDirty("A"));

  std::cout << "  ✓ test_mark_and_has" << std::endl;
  return 0;
}

static int test_take_returns_and_clears() {
  DirtyTracker dt;
  dt.MarkDirty("A", "p1");
  dt.MarkDirty("A", "p2");

  auto paths = dt.TakeDirty("A");
  ASSERT_EQ(paths.size(), static_cast<size_t>(2));

  std::sort(paths.begin(), paths.end());
  ASSERT_EQ(paths[0], std::string("p1"));
  ASSERT_EQ(paths[1], std::string("p2"));

  ASSERT_FALSE(dt.HasDirty("A"));

  auto second = dt.TakeDirty("A");
  ASSERT_TRUE(second.empty());

  // Unknown notebook returns empty.
  auto unknown = dt.TakeDirty("does-not-exist");
  ASSERT_TRUE(unknown.empty());

  std::cout << "  ✓ test_take_returns_and_clears" << std::endl;
  return 0;
}

static int test_clear() {
  DirtyTracker dt;
  dt.MarkDirty("A", "p1");
  dt.Clear("A");
  ASSERT_FALSE(dt.HasDirty("A"));
  ASSERT_TRUE(dt.TakeDirty("A").empty());

  // Clear of unknown notebook is a no-op.
  dt.Clear("nonexistent");

  std::cout << "  ✓ test_clear" << std::endl;
  return 0;
}

static int test_clear_all() {
  DirtyTracker dt;
  dt.MarkDirty("A", "p1");
  dt.MarkDirty("B", "p2");
  dt.MarkDirty("C", "p3");

  dt.ClearAll();

  ASSERT_FALSE(dt.HasDirty("A"));
  ASSERT_FALSE(dt.HasDirty("B"));
  ASSERT_FALSE(dt.HasDirty("C"));

  std::cout << "  ✓ test_clear_all" << std::endl;
  return 0;
}

static int test_concurrent_mark_take_1000_iter() {
  DirtyTracker dt;
  constexpr int kNotebooks = 10;
  constexpr int kIters = 1000;
  constexpr int kMarkers = 4;
  constexpr int kTakers = 4;

  std::atomic<int> issued{0};
  std::atomic<int> taken{0};

  auto marker = [&]() {
    for (int i = 0; i < kIters; ++i) {
      int nb = i % kNotebooks;
      std::string nb_id = "nb-" + std::to_string(nb);
      std::string path = "p-" + std::to_string(i);
      dt.MarkDirty(nb_id, path);
      issued.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto taker = [&]() {
    for (int i = 0; i < kIters; ++i) {
      int nb = i % kNotebooks;
      std::string nb_id = "nb-" + std::to_string(nb);
      auto v = dt.TakeDirty(nb_id);
      taken.fetch_add(static_cast<int>(v.size()), std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kMarkers + kTakers);
  for (int i = 0; i < kMarkers; ++i) threads.emplace_back(marker);
  for (int i = 0; i < kTakers; ++i) threads.emplace_back(taker);
  for (auto &t : threads) t.join();

  // Drain whatever's left so we can check the conservation invariant.
  int remaining = 0;
  for (int nb = 0; nb < kNotebooks; ++nb) {
    auto v = dt.TakeDirty("nb-" + std::to_string(nb));
    remaining += static_cast<int>(v.size());
  }

  // Because MarkDirty uses set semantics, the same (nb, path) issued multiple
  // times by different marker threads collapses into one entry. Therefore:
  //   taken + remaining <= issued
  // and the tracker must be empty after draining.
  const int total_consumed = taken.load() + remaining;
  ASSERT_TRUE(total_consumed <= issued.load());
  for (int nb = 0; nb < kNotebooks; ++nb) {
    ASSERT_FALSE(dt.HasDirty("nb-" + std::to_string(nb)));
  }

  std::cout << "  ✓ test_concurrent_mark_take_1000_iter (issued=" << issued.load()
            << " taken=" << taken.load() << " remaining=" << remaining << ")" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running DirtyTracker tests..." << std::endl;

  RUN_TEST(test_mark_and_has);
  RUN_TEST(test_take_returns_and_clears);
  RUN_TEST(test_clear);
  RUN_TEST(test_clear_all);
  RUN_TEST(test_concurrent_mark_take_1000_iter);

  std::cout << "All DirtyTracker tests passed." << std::endl;
  return 0;
}
