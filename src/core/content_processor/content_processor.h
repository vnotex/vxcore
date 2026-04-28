#ifndef VXCORE_CONTENT_PROCESSOR_H
#define VXCORE_CONTENT_PROCESSOR_H

#include <memory>
#include <string>
#include <unordered_map>

#include "core/content_processor/file_type_handler.h"

namespace vxcore {

class ContentProcessor {
 public:
  ContentProcessor();

  IFileTypeHandler *GetHandler(const std::string &file_extension) const;
  bool HasHandler(const std::string &file_extension) const;

 private:
  std::unordered_map<std::string, std::unique_ptr<IFileTypeHandler>> handlers_;
};

}  // namespace vxcore

#endif  // VXCORE_CONTENT_PROCESSOR_H
