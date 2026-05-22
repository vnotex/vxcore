// F5.9 / Task 12.1 of sync-backend-phase4: unit tests for SyncCancellation.
//
// Verifies:
//   - Cancel before any libgit2 op flips IsCancelled() to true.
//   - The libgit2 progress shim returns 0 before cancel, nonzero after.
//   - A worker thread polling Libgit2ProgressCheck observes a Cancel() set
//     from the main thread within < 100ms (lock-free atomic visibility).
//   - The shims are null-token-safe (defensive C-callback contract).
//   - The push-side shim behaves the same way.
//
// Direct-compile pattern (mirrors test_retry_policy / test_dirty_tracker):
// SyncCancellation symbols are not VXCORE_API-exported, so the test compiles
// sync_cancellation.cpp directly instead of linking vxcore.dll. No libgit2,
// no Qt deps -- the progress shims only dereference the `token` payload,
// not the libgit2 stats struct, so we pass nullptr for stats.

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "sync/sync_cancellation.h"
#include "test_utils.h"

// vxcore_set_test_mode shim -- same pattern as test_retry_policy.cpp.
namespace {
void vxcore_set_test_mode(int) {}
}  // namespace

using vxcore::SyncCancellation;
using vxcore::SyncCancellationPtr;

static int test_cancel_before_start_no_op() {
  SyncCancellation token;
  ASSERT_FALSE(token.IsCancelled());
  token.Cancel();
  ASSERT_TRUE(token.IsCancelled());
  // Cancel is idempotent.
  token.Cancel();
  ASSERT_TRUE(token.IsCancelled());
  std::cout << "  PASS test_cancel_before_start_no_op" << std::endl;
  return 0;
}

static int test_libgit2_progress_returns_zero_before_cancel() {
  SyncCancellation token;
  // Plan-mandated null-stats safety: the shim must never dereference stats.
  int rc = SyncCancellation::Libgit2ProgressCheck(/*stats=*/nullptr, &token);
  ASSERT_EQ(rc, 0);
  // Push-side equivalent.
  int prc = SyncCancellation::Libgit2PushProgressCheck(0, 0, 0, &token);
  ASSERT_EQ(prc, 0);
  std::cout << "  PASS test_libgit2_progress_returns_zero_before_cancel" << std::endl;
  return 0;
}

static int test_libgit2_progress_returns_nonzero_after_cancel() {
  SyncCancellation token;
  token.Cancel();
  int rc = SyncCancellation::Libgit2ProgressCheck(/*stats=*/nullptr, &token);
  ASSERT_TRUE(rc != 0);
  int prc = SyncCancellation::Libgit2PushProgressCheck(10, 20, 1024, &token);
  ASSERT_TRUE(prc != 0);
  std::cout << "  PASS test_libgit2_progress_returns_nonzero_after_cancel" << std::endl;
  return 0;
}

static int test_null_token_returns_zero() {
  // Both shims must be safe with a null payload (libgit2 will sometimes route
  // a null payload through when callbacks are wired without a token).
  ASSERT_EQ(SyncCancellation::Libgit2ProgressCheck(/*stats=*/nullptr,
                                                    /*token=*/nullptr),
            0);
  ASSERT_EQ(SyncCancellation::Libgit2PushProgressCheck(0, 0, 0,
                                                        /*payload=*/nullptr),
            0);
  std::cout << "  PASS test_null_token_returns_zero" << std::endl;
  return 0;
}

static int test_cancel_during_fetch_within_100ms() {
  // Spawn a worker that loops calling the progress shim like libgit2 would.
  // Main thread cancels after a short delay; assert the worker observes the
  // nonzero return within 100ms (lock-free atomic store -> load latency
  // across cores is bounded in microseconds on any platform we support).
  SyncCancellation token;
  std::atomic<bool> worker_observed_cancel{false};
  std::atomic<long long> observed_us{-1};
  std::atomic<bool> stop_worker{false};

  auto start = std::chrono::steady_clock::now();
  std::thread worker([&] {
    for (;;) {
      if (stop_worker.load(std::memory_order_relaxed)) return;
      int rc = SyncCancellation::Libgit2ProgressCheck(/*stats=*/nullptr, &token);
      if (rc != 0) {
        auto now = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        observed_us.store(us, std::memory_order_seq_cst);
        worker_observed_cancel.store(true, std::memory_order_seq_cst);
        return;
      }
      // Tight busy loop matches libgit2's progress-callback cadence during
      // a fast local fetch; the small sleep keeps the test thread polite
      // without masking lock-free visibility delays.
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  });

  // Let the worker spin for a bit, then cancel from the main thread.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  token.Cancel();

  // Wait up to 100ms for the worker to observe the cancel.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (!worker_observed_cancel.load(std::memory_order_seq_cst) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  stop_worker.store(true, std::memory_order_relaxed);
  worker.join();

  ASSERT_TRUE(worker_observed_cancel.load(std::memory_order_seq_cst));
  // Sanity: observation latency from Cancel() (~5ms after start) should be
  // well under the 100ms budget. Print for diagnostics.
  long long us = observed_us.load(std::memory_order_seq_cst);
  std::cout << "  PASS test_cancel_during_fetch_within_100ms (observed at "
            << us << "us from start)" << std::endl;
  return 0;
}

static int test_shared_ptr_alias() {
  // SyncCancellationPtr is a std::shared_ptr<SyncCancellation>. Verify the
  // alias compiles and round-trips correctly.
  SyncCancellationPtr p = std::make_shared<SyncCancellation>();
  ASSERT_FALSE(p->IsCancelled());
  p->Cancel();
  ASSERT_TRUE(p->IsCancelled());
  // Raw pointer extraction (mirrors how SyncManager will pass the token to
  // libgit2 via GitCredentialPayload::cancellation).
  SyncCancellation *raw = p.get();
  ASSERT_NOT_NULL(raw);
  ASSERT_TRUE(raw->IsCancelled());
  ASSERT_TRUE(SyncCancellation::Libgit2ProgressCheck(nullptr, raw) != 0);
  std::cout << "  PASS test_shared_ptr_alias" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncCancellation tests..." << std::endl;

  RUN_TEST(test_cancel_before_start_no_op);
  RUN_TEST(test_libgit2_progress_returns_zero_before_cancel);
  RUN_TEST(test_libgit2_progress_returns_nonzero_after_cancel);
  RUN_TEST(test_null_token_returns_zero);
  RUN_TEST(test_cancel_during_fetch_within_100ms);
  RUN_TEST(test_shared_ptr_alias);

  std::cout << "All SyncCancellation tests passed." << std::endl;
  return 0;
}
