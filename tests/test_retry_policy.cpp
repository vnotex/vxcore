// F5.10 / Task 11.1 of sync-backend-phase4: unit tests for RetryPolicy /
// compute_delay / is_retryable_network_error.
//
// Direct-compile pattern: retry_policy.cpp is exported through vxcore.dll
// via `VXCORE_API`-free free functions, but we direct-compile it here to
// stay independent of the DLL boundary (mirrors test_dirty_tracker). No
// libgit2 / Qt deps.

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <vector>

#include "sync/retry_policy.h"
#include "test_utils.h"

// vxcore_set_test_mode shim — same pattern as test_dirty_tracker.cpp.
namespace {
void vxcore_set_test_mode(int) {}
}  // namespace

using vxcore::RetryPolicy;
using vxcore::compute_delay;
using vxcore::is_retryable_network_error;

// libgit2 error code constants re-derived locally (matches retry_policy.cpp).
// Keeps the test independent of <git2.h>.
static constexpr int kGitOk = 0;
static constexpr int kGitError = -1;
static constexpr int kGitENotFound = -3;
static constexpr int kGitEUnmerged = -10;
static constexpr int kGitETimeout = -12;
static constexpr int kGitEInvalidSpec = -15;
static constexpr int kGitEAuth = -16;
static constexpr int kGitECertificate = -17;

static int test_attempt_one_is_zero() {
  RetryPolicy policy;
  std::mt19937 rng(0xdeadbeef);
  auto d = compute_delay(1, policy, rng);
  ASSERT_EQ(d.count(), static_cast<long long>(0));

  // Also: zero / negative attempt clamps to zero (function is total).
  ASSERT_EQ(compute_delay(0, policy, rng).count(), static_cast<long long>(0));
  ASSERT_EQ(compute_delay(-5, policy, rng).count(), static_cast<long long>(0));

  std::cout << "  ✓ test_attempt_one_is_zero" << std::endl;
  return 0;
}

static int test_schedule_3_attempts() {
  // With a fixed seed the compute_delay outputs are bit-exact reproducible.
  // We assert the bracket they fall into (jitter band around the un-jittered
  // base) for each attempt — gives determinism without baking the
  // implementation-specific rng draw into the test.
  RetryPolicy policy;  // default: initial=500, mult=2.0, jitter=0.2
  std::mt19937 rng(0xc0ffee);

  auto d1 = compute_delay(1, policy, rng);
  auto d2 = compute_delay(2, policy, rng);
  auto d3 = compute_delay(3, policy, rng);

  // attempt 1: always 0.
  ASSERT_EQ(d1.count(), static_cast<long long>(0));

  // attempt 2: base = 500 * 2^0 = 500. Band: [400, 600].
  ASSERT_TRUE(d2.count() >= 400 && d2.count() <= 600);

  // attempt 3: base = 500 * 2^1 = 1000. Band: [800, 1200].
  ASSERT_TRUE(d3.count() >= 800 && d3.count() <= 1200);

  // Re-run with the SAME seed — bit-exact reproduction proves the function
  // is pure over its inputs.
  std::mt19937 rng2(0xc0ffee);
  auto d2b = compute_delay(2, policy, rng2);
  auto d3b = compute_delay(3, policy, rng2);
  ASSERT_EQ(d2.count(), d2b.count());
  ASSERT_EQ(d3.count(), d3b.count());

  std::cout << "  ✓ test_schedule_3_attempts (d2=" << d2.count()
            << "ms d3=" << d3.count() << "ms)" << std::endl;
  return 0;
}

static int test_jitter_within_20pct() {
  RetryPolicy policy;
  std::mt19937 rng(42);
  // 1000 runs of attempt=2 — base=500ms, jitter band [400, 600].
  // Allow +/-1ms slop for the rounding-to-nearest in compute_delay().
  long long lo_seen = 1'000'000;
  long long hi_seen = -1;
  for (int i = 0; i < 1000; ++i) {
    auto d = compute_delay(2, policy, rng);
    ASSERT_TRUE(d.count() >= 399);
    ASSERT_TRUE(d.count() <= 601);
    if (d.count() < lo_seen) lo_seen = d.count();
    if (d.count() > hi_seen) hi_seen = d.count();
  }
  // Sanity: 1000 draws across uniform [0.8, 1.2] should populate near both
  // extremes. Verifies the jitter dist actually varies (not stuck at 1.0).
  ASSERT_TRUE(lo_seen < 450);
  ASSERT_TRUE(hi_seen > 550);

  std::cout << "  ✓ test_jitter_within_20pct (lo=" << lo_seen
            << "ms hi=" << hi_seen << "ms)" << std::endl;
  return 0;
}

static int test_max_delay_capped() {
  RetryPolicy policy;  // initial=500, mult=2.0, max=30000, jitter=0.2
  std::mt19937 rng(7);
  // attempt=20: 500 * 2^18 = 131M ms uncapped — well above max_delay.
  // After cap: 30000ms +/-20% = [24000, 36000].
  auto d = compute_delay(20, policy, rng);
  ASSERT_TRUE(d.count() >= 23999);  // 1ms rounding slop
  ASSERT_TRUE(d.count() <= 36001);

  // Sanity: across many draws, all stay within the capped jitter band.
  for (int i = 0; i < 200; ++i) {
    auto dd = compute_delay(20, policy, rng);
    ASSERT_TRUE(dd.count() >= 23999);
    ASSERT_TRUE(dd.count() <= 36001);
  }

  std::cout << "  ✓ test_max_delay_capped (sample=" << d.count() << "ms)"
            << std::endl;
  return 0;
}

static int test_no_retry_on_auth_error() {
  ASSERT_FALSE(is_retryable_network_error(kGitEAuth));
  ASSERT_FALSE(is_retryable_network_error(kGitECertificate));
  ASSERT_FALSE(is_retryable_network_error(kGitEInvalidSpec));
  ASSERT_FALSE(is_retryable_network_error(kGitENotFound));
  ASSERT_FALSE(is_retryable_network_error(kGitEUnmerged));

  // Generic GIT_ERROR is NOT classified network here — translator decides.
  ASSERT_FALSE(is_retryable_network_error(kGitError));

  // Success is not retryable (nothing to retry).
  ASSERT_FALSE(is_retryable_network_error(kGitOk));

  // Unknown code (random negative) — whitelist policy, NOT retried.
  ASSERT_FALSE(is_retryable_network_error(-999));

  // Whitelist member: timeout.
  ASSERT_TRUE(is_retryable_network_error(kGitETimeout));

  std::cout << "  ✓ test_no_retry_on_auth_error" << std::endl;
  return 0;
}

static int test_custom_policy_overrides() {
  // Custom policy: zero jitter -> deterministic exact ms.
  RetryPolicy policy;
  policy.initial_delay = std::chrono::milliseconds(100);
  policy.backoff_multiplier = 3.0;
  policy.max_delay = std::chrono::milliseconds(10'000);
  policy.jitter_ratio = 0.0;

  std::mt19937 rng(1);
  // attempt=2: 100 * 3^0 = 100ms exact.
  ASSERT_EQ(compute_delay(2, policy, rng).count(), static_cast<long long>(100));
  // attempt=3: 100 * 3^1 = 300ms exact.
  ASSERT_EQ(compute_delay(3, policy, rng).count(), static_cast<long long>(300));
  // attempt=4: 100 * 3^2 = 900ms exact.
  ASSERT_EQ(compute_delay(4, policy, rng).count(), static_cast<long long>(900));
  // attempt=10: 100 * 3^8 = 656_100ms uncapped -> capped at 10000ms.
  ASSERT_EQ(compute_delay(10, policy, rng).count(),
            static_cast<long long>(10'000));

  std::cout << "  ✓ test_custom_policy_overrides" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running RetryPolicy tests..." << std::endl;

  RUN_TEST(test_attempt_one_is_zero);
  RUN_TEST(test_schedule_3_attempts);
  RUN_TEST(test_jitter_within_20pct);
  RUN_TEST(test_max_delay_capped);
  RUN_TEST(test_no_retry_on_auth_error);
  RUN_TEST(test_custom_policy_overrides);

  std::cout << "All RetryPolicy tests passed." << std::endl;
  return 0;
}
