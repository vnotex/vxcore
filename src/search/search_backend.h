#ifndef VXCORE_SEARCH_BACKEND_H
#define VXCORE_SEARCH_BACKEND_H

#include <functional>
#include <string>
#include <vector>

namespace vxcore {

// Default number of files per streaming chunk when the caller passes batch_size == 0 to the
// streaming search path. Chunking subsumes the legacy parallel-vs-sequential threshold:
// fileCount <= effective batch size runs inline as a single chunk.
constexpr int kDefaultSearchChunkSize = 64;

// One-based line numbers and zero-based, end-exclusive UTF-16 columns.
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
//                  chunk. No result cap is applied; kFirstMatchPerFile selects only the first
//                  qualifying occurrence per file. Passed by mutable reference so an
//                  accumulating consumer may move out of it; valid only for the callback's
//                  duration.
// May be invoked concurrently from multiple drain threads; implementations MUST be
// thread-safe. Only the chunk's own thread ever touches its batch_files, so cross-chunk
// data does not race — but the callback body itself must guard any shared state it mutates.
using SearchBatchEmitFn = std::function<void(int batch_index, int total_batches,
                                             std::vector<ContentSearchMatchedFile> &batch_files)>;

}  // namespace vxcore

#endif
