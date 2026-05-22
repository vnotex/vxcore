#include "sync/retry_policy.h"

#include <algorithm>
#include <cmath>

namespace vxcore {

namespace {

// libgit2 v1.7 return codes we care about. Re-derived locally so this
// translation unit does NOT depend on <git2.h> (the retry classifier is
// caller-agnostic by design — git_error_translator.cpp owns the libgit2
// include).
//
// Source: third_party/libgit2/include/git2/errors.h (v1.7.x).
constexpr int kGitOk = 0;
constexpr int kGitError = -1;        // generic catch-all (klass disambiguates)
constexpr int kGitENotFound = -3;
constexpr int kGitEUnmerged = -10;
constexpr int kGitETimeout = -12;
constexpr int kGitEInvalidSpec = -15;
constexpr int kGitEAuth = -16;
constexpr int kGitECertificate = -17;
constexpr int kGitEApplied = -18;
constexpr int kGitEUncommitted = -22;
constexpr int kGitEDirectory = -23;
constexpr int kGitEMergeConflict = -24;
constexpr int kGitEMismatch = -33;

}  // namespace

std::chrono::milliseconds compute_delay(int attempt, const RetryPolicy& policy,
                                        std::mt19937& rng) {
  if (attempt <= 1) {
    return std::chrono::milliseconds(0);
  }

  // Base delay: initial * mult^(attempt-2). Computed in double for the
  // pow(); then capped against max_delay BEFORE jitter so the jitter band
  // is centered on the cap (caller's spec: "max_delay (within jitter band)").
  const double base_ms = static_cast<double>(policy.initial_delay.count()) *
                         std::pow(policy.backoff_multiplier,
                                  static_cast<double>(attempt - 2));
  const double max_ms = static_cast<double>(policy.max_delay.count());
  const double capped_ms = std::min(base_ms, max_ms);

  // Apply +/-jitter_ratio multiplicative noise.
  const double jr = policy.jitter_ratio;
  std::uniform_real_distribution<double> dist(1.0 - jr, 1.0 + jr);
  const double jitter_factor = dist(rng);
  const double jittered_ms = capped_ms * jitter_factor;

  // Round to nearest ms. Clamp at zero (jitter_ratio > 1.0 would otherwise
  // allow negatives).
  long long out_ms = static_cast<long long>(jittered_ms + 0.5);
  if (out_ms < 0) {
    out_ms = 0;
  }
  return std::chrono::milliseconds(out_ms);
}

bool is_retryable_network_error(int libgit2_err_code) {
  // Success and obviously non-retryable codes short-circuit.
  if (libgit2_err_code == kGitOk) {
    return false;  // nothing to retry
  }

  switch (libgit2_err_code) {
    // Transient network conditions — retry.
    case kGitETimeout:
      return true;

    // Hard failures — NEVER retry. Listed explicitly to document intent.
    case kGitEAuth:
    case kGitECertificate:
    case kGitEInvalidSpec:
    case kGitENotFound:
    case kGitEUnmerged:
    case kGitEApplied:
    case kGitEUncommitted:
    case kGitEDirectory:
    case kGitEMergeConflict:
    case kGitEMismatch:
      return false;

    // Generic GIT_ERROR (-1) is deliberately NOT retried here. Its klass
    // (HTTP / NET / SSL / ...) is what determines retryability, and that
    // mapping lives in git_error_translator.cpp. Callers that need to retry
    // by klass must consult `TranslateGitError`'s result (e.g. on
    // VXCORE_ERR_SYNC_NETWORK) and call `compute_delay` directly.
    case kGitError:
      return false;

    default:
      // Whitelist policy: unknown codes are NOT retried.
      return false;
  }
}

}  // namespace vxcore
