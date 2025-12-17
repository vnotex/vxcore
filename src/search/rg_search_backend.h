#ifndef VXCORE_RG_SEARCH_BACKEND_H
#define VXCORE_RG_SEARCH_BACKEND_H

#include "search_backend.h"

namespace vxcore {

class RgSearchBackend : public ISearchBackend {
 public:
  RgSearchBackend();
  ~RgSearchBackend() override;

  bool Search(const std::string &root_path, const std::string &pattern, bool case_sensitive,
              bool whole_word, bool regex, const std::vector<std::string> &path_patterns,
              const std::vector<std::string> &exclude_path_patterns,
              const std::vector<std::string> &content_exclude_patterns,
              std::vector<ContentSearchResult> &out_results) override;

 private:
  bool IsAvailable();
  std::string BuildCommand(const std::string &root_path, const std::string &pattern,
                           bool case_sensitive, bool whole_word, bool regex,
                           const std::vector<std::string> &path_patterns,
                           const std::vector<std::string> &exclude_path_patterns);
  void ParseOutput(const std::string &output, std::vector<ContentSearchResult> &out_results);
};

}  // namespace vxcore

#endif
