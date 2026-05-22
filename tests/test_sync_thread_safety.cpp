// Task 10.2 (sync-backend-phase4 F2.4 part 3): SyncManager thread-safety
// stress test.
//
// Goal: under heavy concurrent load (8 threads x 5 notebooks x 200 iterations
// = 1600 total operations) hammering EnableSync / TriggerSync / DisableSync /
// ClearDirty / GetSyncConfig / GetDirtyNotebooks / IsReady, no crash, no data
// corruption, and no deadlock occurs.
//
// Why this matters: Wave 10.1 introduced SyncManager::state_mutex_ guarding
// the three runtime maps (configs_cache_, states_, backends_) shared between
// the orchestrator thread and any event-driven mark-dirty path. Wave 10.1's
// reentrancy test proved no callbacks fire under the lock; this test proves
// the maps themselves stay coherent under concurrent producer/consumer load.
//
// Test strategy:
//   - Each test notebook lives in its own temp directory (no shared
//     filesystem state across notebooks).
//   - All notebooks use the "mock" backend which returns instantly, so the
//     test focuses on lock contention rather than backend latency.
//   - A watchdog thread aborts the process after 30 seconds if the stress
//     phase has not signaled completion — better than a silent ctest timeout
//     because the core dump points at the offending thread.
//   - Final state is non-deterministic (last-enable vs last-disable wins per
//     notebook). The assertion is "no crash / no garbage", not "all enabled".
//
// MUST NOT: real network, Qt, shared git repos, libgit2 (mock backend only).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/context.h"
#include "sync/sync_backend.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

namespace {

// Force-link the test_internals TU so its anonymous-namespace
// BackendRegistration token (for "mock") runs at static-init time. Without
// this anchor MSVC drops the registration TU because nothing else references
// it. Matches the pattern in test_event_manager.cpp,
// test_sync_manager_factory_override.cpp, test_sync_manager_reentrancy.cpp.
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();

using vxcore::SyncConfig;
using vxcore::SyncManager;

// Fixture: one VxCoreContext + N bundled notebooks, each rooted in its own
// isolated temp directory. Owns the notebook IDs so the stress threads can
// read them lock-free.
struct StressFixture {
  VxCoreContextHandle ctx = nullptr;
  std::vector<std::string> notebook_ids;
  std::vector<std::string> roots;

  explicit StressFixture(int num_notebooks) {
    auto err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) {
      std::cerr << "context_create failed: " << err << std::endl;
      std::exit(1);
    }
    notebook_ids.reserve(num_notebooks);
    roots.reserve(num_notebooks);
    for (int i = 0; i < num_notebooks; ++i) {
      std::string root =
          get_test_path("test_sync_thread_safety_nb_" + std::to_string(i));
      cleanup_test_dir(root);
      roots.push_back(root);

      char *id = nullptr;
      std::string cfg_json =
          "{\"name\":\"StressNB_" + std::to_string(i) + "\"}";
      err = vxcore_notebook_create(ctx, root.c_str(), cfg_json.c_str(),
                                   VXCORE_NOTEBOOK_BUNDLED, &id);
      if (err != VXCORE_OK || id == nullptr) {
        std::cerr << "notebook_create failed: " << err << std::endl;
        std::exit(1);
      }
      notebook_ids.emplace_back(id);
      vxcore_string_free(id);
    }
  }

  ~StressFixture() {
    if (ctx) vxcore_context_destroy(ctx);
    for (const auto &root : roots) {
      cleanup_test_dir(root);
    }
  }

  SyncManager &sync_manager() {
    auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
    return *vctx->sync_manager;
  }
};

// Build a SyncConfig pointing at the mock backend. The factory in
// test_internals self-registers "mock" with the registry, so EnableSync
// dispatches to MockSyncBackend without touching libgit2 or the network.
SyncConfig MakeMockConfig() {
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "test://stress";
  cfg.interval_seconds = 60;
  return cfg;
}

// Stress subtest: 8 threads x 5 notebooks x 200 iterations.
//
// Important scope note: per `libs/vxcore/AGENTS.md` § Thread Safety,
// `NotebookManager` itself is NOT thread-safe — concurrent writers into the
// notebook config (e.g. TriggerSync calling Notebook::SetLastSyncUtc while
// another thread runs GetSyncConfig reading the same Notebook) is documented
// as undefined behaviour. Wave 10.1's state_mutex_ guards SyncManager's
// internal maps (configs_cache_/states_/backends_) but NOT the Notebook*
// objects owned by NotebookManager. To stay within the contract we
// serialize Enable/Disable/Trigger ops (the writers into per-Notebook state)
// behind a single test-owned mutex, while running the SyncManager
// read-side accessors (GetSyncConfig / IsReady / GetDirtyNotebooks /
// ClearDirty) fully concurrently. This still exercises state_mutex_'s
// read-side contention pattern — which is what Wave 10's discipline must
// guarantee — without conflating with the orthogonal NotebookManager
// thread-safety bug tracked elsewhere.
int test_concurrent_enable_trigger_disable_mark_getconfig_8threads_5notebooks_200iter() {
  std::cout
      << "  Running test_concurrent_enable_trigger_disable_mark_getconfig_"
         "8threads_5notebooks_200iter..."
      << std::endl;

  constexpr int kThreads = 8;
  constexpr int kNotebooks = 5;
  constexpr int kIterations = 200;

  StressFixture fix(kNotebooks);
  SyncManager &mgr = fix.sync_manager();

  // Watchdog: if stress phase hasn't completed in 30s, abort. Use a polled
  // loop so we can exit cleanly when done_ flips true before the budget
  // elapses (avoids leaving a stray thread alive past the test).
  std::atomic<bool> done{false};
  std::thread watchdog([&done]() {
    for (int i = 0; i < 300; ++i) {  // 300 * 100ms = 30s
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (done.load(std::memory_order_acquire)) return;
    }
    std::cerr << "✗ Watchdog tripped: stress phase exceeded 30s — likely "
                 "deadlock in SyncManager::state_mutex_ discipline."
              << std::endl;
    std::abort();
  });

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  std::atomic<int> total_ops{0};

  // Single mutex serializing the Enable/Disable/Trigger writer ops. See
  // scope-note in the function-level comment for rationale. Read-side ops
  // (cases 3-5) run fully concurrently against this mutex.
  std::mutex writer_mu;

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t]() {
      // Each thread has its own RNG seeded deterministically by thread id for
      // reproducibility. Threads still race against each other so the global
      // op interleaving is non-deterministic.
      std::mt19937 rng(static_cast<uint32_t>(0xC0FFEE ^ t));
      std::uniform_int_distribution<int> op_dist(0, 5);
      std::uniform_int_distribution<int> nb_dist(0, kNotebooks - 1);

      for (int k = 0; k < kIterations; ++k) {
        int op = op_dist(rng);
        int nb_idx = nb_dist(rng);
        const std::string &nb_id = fix.notebook_ids[nb_idx];

        switch (op) {
          case 0: {
            // EnableSync — writer. Serialized via writer_mu.
            std::lock_guard<std::mutex> lk(writer_mu);
            (void)mgr.EnableSync(nb_id, MakeMockConfig());
            break;
          }
          case 1: {
            // TriggerSync — writer (updates Notebook::SetLastSyncUtc).
            // Serialized via writer_mu.
            std::lock_guard<std::mutex> lk(writer_mu);
            (void)mgr.TriggerSync(nb_id);
            break;
          }
          case 2: {
            // DisableSync — writer. Serialized via writer_mu.
            std::lock_guard<std::mutex> lk(writer_mu);
            (void)mgr.DisableSync(nb_id);
            break;
          }
          case 3: {
            // ClearDirty + GetDirtyNotebooks — DirtyTracker is self-locking
            // and SyncManager::state_mutex_ guards its own internals; safe
            // to run concurrently with writers (and with each other).
            mgr.ClearDirty(nb_id);
            auto dirty = mgr.GetDirtyNotebooks();
            (void)dirty;
            break;
          }
          case 4: {
            // GetSyncConfig — read accessor. Exercises configs_cache_ slow
            // path under state_mutex_ read side. Concurrent with writers.
            SyncConfig out;
            (void)mgr.GetSyncConfig(nb_id, out);
            break;
          }
          case 5: {
            // IsReady — const accessor, takes state_mutex_ briefly to read
            // map. Concurrent with writers.
            (void)mgr.IsReady(nb_id);
            break;
          }
        }
        total_ops.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto &w : workers) w.join();

  // Stress phase complete — disarm the watchdog before the post-phase
  // assertions (which mustn't be falsely killed by a 30s overrun in a slow
  // CI environment).
  done.store(true, std::memory_order_release);
  watchdog.join();

  std::cout << "    Total operations executed: "
            << total_ops.load(std::memory_order_relaxed) << std::endl;
  ASSERT_EQ(total_ops.load(std::memory_order_relaxed), kThreads * kIterations);

  // Final-state invariants: for each notebook, the public accessors must
  // return consistent (non-crashing, non-garbage) values regardless of who
  // won the last enable/disable race.
  for (int i = 0; i < kNotebooks; ++i) {
    const std::string &nb_id = fix.notebook_ids[i];

    // IsReady reads notebook JSON only — must not throw or crash.
    bool ready = mgr.IsReady(nb_id);
    (void)ready;  // value is non-deterministic; both true/false legal.

    // GetSyncConfig must either return OK with a valid cfg or a documented
    // error code (NOT_FOUND/SYNC_NOT_ENABLED). It must not segfault or
    // return undefined memory.
    SyncConfig out;
    VxCoreError err = mgr.GetSyncConfig(nb_id, out);
    bool err_ok = (err == VXCORE_OK || err == VXCORE_ERR_SYNC_NOT_ENABLED ||
                   err == VXCORE_ERR_NOT_FOUND);
    if (!err_ok) {
      std::cerr << "    Unexpected GetSyncConfig error for nb " << i << ": "
                << err << std::endl;
    }
    ASSERT_TRUE(err_ok);
    if (err == VXCORE_OK) {
      // backend string is either "mock" (last EnableSync won) or empty
      // (last DisableSync won and cleared the on-disk sync fields, so the
      // read-through cache returned empty). Both are legal under race;
      // assert only that it's not undefined memory.
      bool backend_ok = (out.backend == "mock" || out.backend.empty());
      if (!backend_ok) {
        std::cerr << "    Unexpected backend string for nb " << i << ": '"
                  << out.backend << "'" << std::endl;
      }
      ASSERT_TRUE(backend_ok);
    }

    // GetDirtyNotebooks must complete without crash.
    auto dirty = mgr.GetDirtyNotebooks();
    (void)dirty;
  }

  // Final cleanup: best-effort disable everything to drain backends_ before
  // context teardown (avoids destructor running a backend dtor against a
  // half-mutated state).
  for (int i = 0; i < kNotebooks; ++i) {
    (void)mgr.DisableSync(fix.notebook_ids[i]);
  }

  std::cout
      << "  ✓ test_concurrent_enable_trigger_disable_mark_getconfig_"
         "8threads_5notebooks_200iter passed"
      << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncManager thread-safety stress tests..." << std::endl;
  RUN_TEST(test_concurrent_enable_trigger_disable_mark_getconfig_8threads_5notebooks_200iter);
  std::cout << "All SyncManager thread-safety stress tests passed." << std::endl;
  return 0;
}
