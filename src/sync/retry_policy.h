// F5.10 / Task 11.1 of sync-backend-phase4: retry policy with exponential
// backoff + jitter for git sync network operations.
//
// Design notes:
//   - `RetryPolicy` is a POD struct of policy knobs. NOT a class. NOT exported
//     across the C ABI. Internal to the C++ sync subsystem.
//   - `compute_delay()` is a PURE function of (attempt, policy, rng-state).
//     No global state, no system time, no random_device. Determinism is the
//     whole point — unit tests seed an `std::mt19937` and assert exact ms.
//   - `is_retryable_network_error()` is a WHITELIST classifier — unknown
//     libgit2 return codes are NOT retried (safer default; auth failures and
//     validation errors must never be retried per the plan's MUST NOT list).
//
// See libs/vxcore/src/sync/AGENTS.md for the broader sync architecture.

#ifndef VXCORE_SYNC_RETRY_POLICY_H
#define VXCORE_SYNC_RETRY_POLICY_H

#include <chrono>
#include <random>

namespace vxcore {

// Policy knobs controlling the retry schedule. Defaults are tuned for git
// network operations (fetch / push) against typical hosted git remotes.
struct RetryPolicy {
  int max_attempts = 3;
  std::chrono::milliseconds initial_delay{500};
  double backoff_multiplier = 2.0;
  std::chrono::milliseconds max_delay{30'000};
  double jitter_ratio = 0.2;  // +/-20%
};

// Compute the delay to sleep BEFORE the given attempt.
//
// `attempt` is 1-based: 1, 2, 3, ...
//   attempt == 1                       -> 0 ms (first attempt; no delay)
//   attempt >= 2                       -> min(initial_delay * pow(mult, attempt-2),
//                                              max_delay)
//                                          multiplicatively jittered by
//                                          uniform [1 - jitter_ratio,
//                                                   1 + jitter_ratio].
//
// `rng` is mutated by the call. Pass a seeded `std::mt19937` to keep the
// schedule deterministic in tests. Production callers seed once per pipeline
// from `std::random_device`.
//
// Negative or zero `attempt` is treated as `attempt == 1` (returns 0 ms) so
// the function is total over int inputs.
std::chrono::milliseconds compute_delay(int attempt, const RetryPolicy& policy,
                                        std::mt19937& rng);

// Whitelist classifier: returns true iff `libgit2_err_code` is a transient
// network/transport error worth retrying. Takes a raw int (the libgit2 return
// code) instead of an enum so this header does NOT need `<git2.h>` — keeps
// the retry_policy translation unit independent of libgit2.
//
// True for: GIT_ETIMEOUT (-12), and additionally for any caller-detected
// network/transport class condition that the caller maps onto a libgit2 rc
// from {GIT_ERROR (-1), GIT_ENETWORK reserved range}.
//
// False for: GIT_EAUTH (-16), GIT_EINVALIDSPEC (-15), GIT_ENOTFOUND (-3),
// GIT_EUNMERGED (-10), and every code not explicitly whitelisted.
//
// IMPORTANT: non-fast-forward push rejections in libgit2 v1.7 surface as
// generic GIT_ERROR (-1) with klass GIT_ERROR_REFERENCE, which is NOT
// classified as a network error here. Callers that want to retry non-ff
// pushes must detect that separately (e.g. by message inspection) and
// invoke `compute_delay()` directly.
bool is_retryable_network_error(int libgit2_err_code);

}  // namespace vxcore

#endif  // VXCORE_SYNC_RETRY_POLICY_H
