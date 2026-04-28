#ifndef VXCORE_ASSET_UTILS_H
#define VXCORE_ASSET_UTILS_H

#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

VxCoreError CopyAssetsDirectory(const std::string &source_assets_dir,
                                const std::string &dest_assets_dir);

VxCoreError MoveAssetsDirectory(const std::string &source_assets_dir,
                                const std::string &dest_assets_dir);

int CopyRelativeLinkedFiles(const std::vector<std::string> &relative_paths,
                            const std::string &src_dir,
                            const std::string &dest_dir,
                            const std::string &notebook_root,
                            const std::string &source_file_path);

int MoveRelativeLinkedFiles(const std::vector<std::string> &relative_paths,
                            const std::string &src_dir,
                            const std::string &dest_dir,
                            const std::string &notebook_root,
                            const std::string &source_file_path);

}  // namespace vxcore

#endif  // VXCORE_ASSET_UTILS_H
