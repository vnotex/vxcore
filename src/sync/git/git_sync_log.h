#ifndef VXCORE_SYNC_GIT_GIT_SYNC_LOG_H
#define VXCORE_SYNC_GIT_GIT_SYNC_LOG_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>

#include "utils/logger.h"

namespace vxcore {

// Phase names are fixed literals; context is an opaque handle, never user data.
class GitSyncLog {
 public:
  GitSyncLog(const char *phase, const void *context)
      : phase_(phase),
        context_(const_cast<void *>(context)),
        worker_(WorkerId()),
        started_(std::chrono::steady_clock::now()) {
    VXCORE_LOG_INFO("GitSync worker=%llu context=%p phase=%s event=start",
                    worker_, context_, phase_);
  }

  ~GitSyncLog() {
    if (!finished_) {
      VXCORE_LOG_WARN("GitSync worker=%llu context=%p phase=%s event=unfinished elapsed_ms=%lld",
                      worker_, context_, phase_, ElapsedMs());
    }
  }

  GitSyncLog(const GitSyncLog &) = delete;
  GitSyncLog &operator=(const GitSyncLog &) = delete;

  template <typename Result>
  Result Finish(Result code) {
    finished_ = true;
    Logger::GetInstance().Log(
        code == 0 ? LogLevel::kInfo : LogLevel::kWarn, __FILE__, __LINE__,
        "GitSync worker=%llu context=%p phase=%s event=finish elapsed_ms=%lld code=%d",
        worker_, context_, phase_, ElapsedMs(), static_cast<int>(code));
    return code;
  }

  bool FinishProbe(int code, bool has_refs) {
    Logger::GetInstance().Log(
        code == 0 ? LogLevel::kInfo : LogLevel::kWarn, __FILE__, __LINE__,
        "GitSync worker=%llu context=%p phase=%s event=remote_refs has_refs=%d code=%d",
        worker_, context_, phase_, has_refs ? 1 : 0, code);
    Finish(code);
    return has_refs;
  }

  void CredentialSummary(std::size_t attempts, int code) const {
    Logger::GetInstance().Log(
        code == 0 ? LogLevel::kInfo : LogLevel::kWarn, __FILE__, __LINE__,
        "GitSync worker=%llu context=%p phase=%s event=credential_summary "
        "callback_attempts=%zu code=%d",
        worker_, context_, phase_, attempts, code);
  }

  static unsigned long long WorkerId() {
    return static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
  }

 private:
  long long ElapsedMs() const {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_).count());
  }

  const char *const phase_;
  void *const context_;
  const unsigned long long worker_;
  const std::chrono::steady_clock::time_point started_;
  bool finished_ = false;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_LOG_H
