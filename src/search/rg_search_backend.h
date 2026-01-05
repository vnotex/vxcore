#ifndef VXCORE_RG_SEARCH_BACKEND_H
#define VXCORE_RG_SEARCH_BACKEND_H

#include <unordered_map>

#include "search_backend.h"

class RgSearchBackendTest;

namespace vxcore {

class RgSearchBackend : public ISearchBackend {
 public:
  RgSearchBackend();
  ~RgSearchBackend() override;

  VxCoreError Search(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                     SearchOption options, const std::vector<std::string> &content_exclude_patterns,
                     int max_results, ContentSearchResult &out_result) override;

  static bool IsAvailable();

 private:
  friend class ::RgSearchBackendTest;
  std::string BuildCommand(const std::vector<SearchFileInfo> &files, const std::string &pattern,
                           SearchOption options,
                           const std::vector<std::string> &content_exclude_patterns,
                           int max_results);
  void ParseOutput(const std::string &output,
                   const std::unordered_map<std::string, const SearchFileInfo *> &abs_to_file_info,
                   std::vector<ContentSearchMatchedFile> &out_results);
};

}  // namespace vxcore

#endif
