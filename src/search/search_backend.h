#ifndef VXCORE_SEARCH_BACKEND_H
#define VXCORE_SEARCH_BACKEND_H

#include <string>
#include <vector>

#include "search_query.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

struct SearchFileInfo;

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

class ISearchBackend {
 public:
  virtual ~ISearchBackend() = default;

  virtual VxCoreError Search(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                             SearchOption options,
                             const std::vector<std::string> &content_exclude_patterns,
                             int max_results, ContentSearchResult &out_result) = 0;
};

}  // namespace vxcore

#endif
