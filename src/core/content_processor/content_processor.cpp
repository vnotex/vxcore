#include "core/content_processor/content_processor.h"

#include <algorithm>

#include "core/content_processor/markdown_handler.h"

namespace vxcore {

ContentProcessor::ContentProcessor() {
  handlers_["md"] = std::make_unique<MarkdownHandler>();
  handlers_["mkd"] = std::make_unique<MarkdownHandler>();
  handlers_["rmd"] = std::make_unique<MarkdownHandler>();
  handlers_["markdown"] = std::make_unique<MarkdownHandler>();
}

IFileTypeHandler *ContentProcessor::GetHandler(
    const std::string &file_extension) const {
  std::string ext = file_extension;
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  auto it = handlers_.find(ext);
  if (it != handlers_.end()) {
    return it->second.get();
  }
  return nullptr;
}

bool ContentProcessor::HasHandler(const std::string &file_extension) const {
  return GetHandler(file_extension) != nullptr;
}

}  // namespace vxcore
