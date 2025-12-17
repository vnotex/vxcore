#ifndef VXCORE_SEARCH_BACKEND_H
#define VXCORE_SEARCH_BACKEND_H

#include <string>
#include <vector>

namespace vxcore {

struct SearchMatch {
  int line_number;
  int column_start;
  int column_end;
  std::string line_text;
  std::string match_text;
};

struct ContentSearchResult {
  std::string file_path;
  std::vector<SearchMatch> matches;
};

class ISearchBackend {
 public:
  virtual ~ISearchBackend() = default;

  virtual bool Search(const std::string &root_path, const std::string &pattern, bool case_sensitive,
                      bool whole_word, bool regex, const std::vector<std::string> &path_patterns,
                      const std::vector<std::string> &exclude_path_patterns,
                      const std::vector<std::string> &content_exclude_patterns,
                      std::vector<ContentSearchResult> &out_results) = 0;
};

}  // namespace vxcore

#endif
