#ifndef VXCORE_CLI_CONFIG_CMD_H
#define VXCORE_CLI_CONFIG_CMD_H

#include "args.h"

namespace vxcore_cli {

class ConfigCommand {
 public:
  static int execute(const ParsedArgs &args);

 private:
  static int dump(const ParsedArgs &args);
};

}  // namespace vxcore_cli

#endif
