#ifndef VXCORE_SIMPLE_SEARCH_BACKEND_H
#define VXCORE_SIMPLE_SEARCH_BACKEND_H

#include <string>
#include <vector>

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

 private:
  friend class ::SimpleSearchBackendTest;

  bool MatchesPattern(const std::string &line, const std::string &pattern, SearchOption options,
                      std::vector<SearchMatch> &out_matches);
};

}  // namespace vxcore

#endif
