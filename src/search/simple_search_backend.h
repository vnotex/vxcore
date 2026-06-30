#ifndef VXCORE_SIMPLE_SEARCH_BACKEND_H
#define VXCORE_SIMPLE_SEARCH_BACKEND_H

#include <functional>
#include <regex>
#include <string>
#include <vector>

#include "core/work_queue.h"
#include "search_backend.h"

class SimpleSearchBackendTest;

namespace vxcore {

class SimpleSearchBackend : public ISearchBackend {
 public:
  SimpleSearchBackend() = default;
  ~SimpleSearchBackend() override = default;

  VxCoreError Search(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                     SearchOption options, const std::vector<std::string> &content_exclude_patterns,
                     int max_results, ContentSearchResult &out_result) override;

  void SetWorkQueue(WorkQueue *queue);
  void SetCancelFlag(const volatile int *flag);

  // ---- Test-only seams (C++ exports; NOT part of the stable C ABI) ----
  // These exist solely so the concurrency/exception test-suite (T6) can prove
  // the caller-helps-drain rewrite empirically. They are complete no-ops in
  // production: every per-work-item hook is gated behind a relaxed atomic flag
  // that only a test arms, so the hot path pays at most one predict-not-taken
  // branch when unarmed.

  // Arm a file-scope rendezvous barrier of `expectedParticipants` arrivals and
  // reset the distinct-worker-thread set. Once armed, each work item records
  // its std::thread::id and rendezvouses on the barrier (2s timeout, so a
  // shortfall degrades to a bounded wait, never a deadlock). Pass 0 to disarm
  // and clear all probe state.
  VXCORE_API static void TestArmParallelismProbe(size_t expectedParticipants);

  // Number of distinct worker threads that have executed a work item since the
  // probe was last armed.
  VXCORE_API static size_t TestDistinctWorkerThreadCount();

  // Arm a one-shot scan-path throw: the next work item to run throws a
  // std::exception (then auto-disarms). Proves the work-item catch -> first_exc
  // -> initiator rethrow -> SearchContent -> VXCORE_ERR_UNKNOWN path on stdlibs
  // (e.g. libstdc++) whose std::regex executor cannot throw at match time.
  VXCORE_API static void TestArmScanThrowOnce();

 private:
  friend class ::SimpleSearchBackendTest;

  VxCoreError SearchSequential(
      const std::vector<SearchFileInfo> &files,
      const std::function<bool(const std::string &, int, std::vector<SearchMatch> &)> &do_match,
      const std::vector<std::string> &content_exclude_patterns,
      const std::vector<std::string> &lowercased_exclude_patterns,
      const std::vector<std::regex> &exclude_regexes, int max_results,
      ContentSearchResult &out_result);
  VxCoreError SearchParallel(
      const std::vector<SearchFileInfo> &files,
      const std::function<bool(const std::string &, int, std::vector<SearchMatch> &)> &do_match,
      const std::vector<std::string> &content_exclude_patterns,
      const std::vector<std::string> &lowercased_exclude_patterns,
      const std::vector<std::regex> &exclude_regexes, int max_results,
      ContentSearchResult &out_result);

  bool MatchesPattern(const std::string &line, const std::string &pattern, SearchOption options,
                      std::vector<SearchMatch> &out_matches);

  WorkQueue *work_queue_ = nullptr;
  const volatile int *cancel_flag_ = nullptr;
};

}  // namespace vxcore

#endif
