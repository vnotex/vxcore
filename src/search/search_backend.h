#ifndef VXCORE_SEARCH_BACKEND_H
#define VXCORE_SEARCH_BACKEND_H

#include <functional>
#include <string>
#include <vector>

#include "search_query.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

struct SearchFileInfo;

// Default number of files per streaming chunk when the caller passes batch_size == 0 to the
// streaming search path. Chunking subsumes the legacy parallel-vs-sequential threshold:
// fileCount <= effective batch size runs inline as a single chunk.
constexpr int kDefaultSearchChunkSize = 64;

struct SearchMatch {
  int line_number = -1;
  int column_start = 0;
  int column_end = 0;
  std::string line_text;
};

struct ContentSearchMatchedFile {
  std::string path;
  std::string id;
  std::vector<SearchMatch> matches;
};

struct ContentSearchResult {
  std::vector<ContentSearchMatchedFile> matched_files;
  bool truncated = false;
};

// Emit callback for streaming content search. Invoked EXACTLY ONCE per completed chunk,
// including zero-match chunks (in which case batch_files is empty).
//   batch_index:   the chunk's position in input-file order (chunk 0 == files[0..batch_size)).
//                  Carried explicitly because chunks may complete out of order across drain
//                  threads; identity is the index, NOT arrival order.
//   total_batches: fixed up front (== number of chunks). Zero-file search yields no calls.
//   batch_files:   the matched files for THIS chunk only, in input-file order within the
//                  chunk. NO truncation is applied (the streaming primitive owns no
//                  truncation). Passed by mutable reference so an accumulating consumer may
//                  move out of it; valid only for the callback's duration.
// May be invoked concurrently from multiple drain threads; implementations MUST be
// thread-safe. Only the chunk's own thread ever touches its batch_files, so cross-chunk
// data does not race — but the callback body itself must guard any shared state it mutates.
using SearchBatchEmitFn = std::function<void(int batch_index, int total_batches,
                                             std::vector<ContentSearchMatchedFile> &batch_files)>;

class ISearchBackend {
 public:
  virtual ~ISearchBackend() = default;

  virtual VxCoreError Search(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                             SearchOption options,
                             const std::vector<std::string> &content_exclude_patterns,
                             int max_results, ContentSearchResult &out_result) = 0;

  // Streaming content search primitive. Scans |files| in chunks of |batch_size| files
  // (0 selects kDefaultSearchChunkSize) and invokes |emit_batch| once per chunk. Applies NO
  // truncation — every match is emitted; the caller owns any cap/reassembly. Blocks on the
  // calling thread (help-draining the work queue when one is configured) and returns when the
  // scan completes. Returns VXCORE_ERR_CANCELLED if a configured cancel flag was observed.
  virtual VxCoreError SearchStreaming(const std::vector<SearchFileInfo> &files,
                                      const std::string &pattern, SearchOption options,
                                      const std::vector<std::string> &content_exclude_patterns,
                                      int batch_size, const SearchBatchEmitFn &emit_batch) = 0;
};

}  // namespace vxcore

#endif
