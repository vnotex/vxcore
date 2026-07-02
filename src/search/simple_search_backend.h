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

  // Blob content search. Reimplemented as a thin accumulating wrapper over SearchStreaming:
  // it collects the streamed chunk slices by batch_index, reassembles them in input-file
  // order, then applies the deterministic file-boundary max_results truncation. Output is
  // byte-identical to the pre-streaming implementation.
  VxCoreError Search(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                     SearchOption options, const std::vector<std::string> &content_exclude_patterns,
                     int max_results, ContentSearchResult &out_result) override;

  // Streaming content search primitive. Scans |files| in chunks of |batch_size| files (0 ->
  // kDefaultSearchChunkSize) and fires |emit_batch| exactly once per chunk (including
  // zero-match chunks). Applies NO truncation. When a work queue is configured and there is
  // more than one chunk, chunks are enqueued to the "vxcore.search" queue and the initiating
  // thread help-drains; otherwise chunks run inline on the calling thread. Honors the cancel
  // flag (returns VXCORE_ERR_CANCELLED). May fire callbacks concurrently across drain threads.
  VxCoreError SearchStreaming(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                              SearchOption options,
                              const std::vector<std::string> &content_exclude_patterns,
                              int batch_size, const SearchBatchEmitFn &emit_batch) override;

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

  // Compiled matcher + preprocessed exclude patterns shared (read-only) across every chunk
  // scan of a single SearchStreaming call. Held on the SearchStreaming stack frame, which
  // outlives all enqueued chunk work items (the initiator blocks until they complete), so
  // references into it never dangle.
  struct MatchContext {
    bool regex = false;
    bool case_sensitive = false;
    bool whole_word = false;
    std::regex pattern_regex;
    std::string lowercased_pattern;  // used when !regex && !case_sensitive
    std::string literal_pattern;     // used when !regex && case_sensitive
    std::vector<std::string> content_exclude_patterns;
    std::vector<std::string> lowercased_exclude_patterns;
    std::vector<std::regex> exclude_regexes;
  };

  // Compiles |pattern| and preprocesses the exclude patterns into |out_ctx|. Returns
  // VXCORE_ERR_INVALID_PARAM on an invalid pattern or exclude regex.
  VxCoreError BuildMatchContext(const std::string &pattern, SearchOption options,
                                const std::vector<std::string> &content_exclude_patterns,
                                MatchContext &out_ctx);

  // Applies the compiled matcher of |ctx| to a single line.
  static bool RunMatch(const MatchContext &ctx, const std::string &line, int line_number,
                       std::vector<SearchMatch> &out_matches);

  // Scans files[begin, end) with NO truncation, appending matched files (in input order) to
  // |out_files|. Fires the test probe hook once at entry, honors the cancel flag (returns
  // early leaving |out_files| partial), and skips unreadable files.
  void ScanChunk(const std::vector<SearchFileInfo> &files, size_t begin, size_t end,
                 const MatchContext &ctx, std::vector<ContentSearchMatchedFile> &out_files);

  bool MatchesPattern(const std::string &line, const std::string &pattern, SearchOption options,
                      std::vector<SearchMatch> &out_matches);

  WorkQueue *work_queue_ = nullptr;
  const volatile int *cancel_flag_ = nullptr;
};

}  // namespace vxcore

#endif
