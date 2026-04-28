#ifndef VXCORE_FILE_TYPE_HANDLER_H
#define VXCORE_FILE_TYPE_HANDLER_H

#include <string>
#include <vector>

namespace vxcore {

struct LinkInfo {
  std::string url;           // The URL/path in the link
  size_t url_start_offset;   // Byte offset of URL start in original content
  size_t url_end_offset;     // Byte offset of URL end in original content
  bool is_image;             // true for ![](url), false for [](url)
};

class IFileTypeHandler {
 public:
  virtual ~IFileTypeHandler() = default;

  virtual std::vector<LinkInfo> DiscoverAssetLinks(
      const std::string &content,
      const std::string &assets_folder_prefix) const = 0;

  virtual std::string RewriteAssetLinks(
      const std::string &content,
      const std::string &old_assets_path,
      const std::string &new_assets_path) const = 0;

  virtual std::vector<std::string> DiscoverRelativeLinks(
      const std::string &content,
      const std::string &assets_folder_prefix) const = 0;
};

}  // namespace vxcore

#endif  // VXCORE_FILE_TYPE_HANDLER_H
