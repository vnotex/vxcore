#ifndef VXCORE_MARKDOWN_HANDLER_H
#define VXCORE_MARKDOWN_HANDLER_H

#include "core/content_processor/file_type_handler.h"

namespace vxcore {

class MarkdownHandler : public IFileTypeHandler {
 public:
  std::vector<LinkInfo> DiscoverAssetLinks(
      const std::string &content,
      const std::string &assets_folder_prefix) const override;

  std::string RewriteAssetLinks(
      const std::string &content,
      const std::string &old_assets_path,
      const std::string &new_assets_path) const override;

  std::vector<std::string> DiscoverRelativeLinks(
      const std::string &content,
      const std::string &assets_folder_prefix) const override;
};

}  // namespace vxcore

#endif  // VXCORE_MARKDOWN_HANDLER_H
