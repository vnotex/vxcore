#ifndef VXCORE_MOBILE_FILE_WRITER_H
#define VXCORE_MOBILE_FILE_WRITER_H

#include <cstdio>
#include <cstdint>
#include <filesystem>

#include "vxcore/vxcore_types.h"

namespace vxcore {

// Single-use POSIX writer for mobile ordinary buffers; the parent must exist.
// Rejects readonly, nonregular, symbolic and hard-linked destinations; preserves
// existing mode bits. Write failures prevent publication, and destruction closes
// and removes an uncommitted exclusive temporary sibling. Identity and mode are
// rechecked before rename, but this is not protection against all concurrent edits.
// Parent-directory fsync can fail after publication: an error does not always
// mean that the old bytes remain. No additional metadata preservation is promised.
class MobileFileWriter final {
 public:
  explicit MobileFileWriter(const std::filesystem::path &path);
  ~MobileFileWriter();

  MobileFileWriter(const MobileFileWriter &) = delete;
  MobileFileWriter &operator=(const MobileFileWriter &) = delete;

  VxCoreError Open();
  VxCoreError Write(const void *data, size_t size);
  VxCoreError Commit();

 private:
  VxCoreError InspectDestination(bool initial);

  std::filesystem::path path_;
  std::filesystem::path temp_path_;
  std::FILE *file_ = nullptr;
  VxCoreError error_ = VXCORE_OK;
  bool opened_ = false;
  bool owns_temp_ = false;
  bool destination_exists_ = false;
  uint64_t destination_device_ = 0;
  uint64_t destination_id_ = 0;
  unsigned int destination_mode_ = 0;
  int parent_fd_ = -1;
};

}  // namespace vxcore

#endif
