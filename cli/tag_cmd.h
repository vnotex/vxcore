#ifndef VXCORE_CLI_TAG_CMD_H
#define VXCORE_CLI_TAG_CMD_H

#include "args.h"

namespace vxcore_cli {

class TagCommand {
 public:
  static int execute(const ParsedArgs &args);

 private:
  static int create(const ParsedArgs &args);
  static int deleteTag(const ParsedArgs &args);
  static int list(const ParsedArgs &args);
  static int addToFile(const ParsedArgs &args);
  static int removeFromFile(const ParsedArgs &args);
};

}  // namespace vxcore_cli

#endif
