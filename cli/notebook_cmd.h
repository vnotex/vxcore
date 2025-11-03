#ifndef VXCORE_CLI_NOTEBOOK_CMD_H
#define VXCORE_CLI_NOTEBOOK_CMD_H

#include "args.h"

namespace vxcore_cli {

class NotebookCommand {
public:
  static int execute(const ParsedArgs &args);

private:
  static int create(const ParsedArgs &args);
  static int open(const ParsedArgs &args);
  static int close(const ParsedArgs &args);
  static int list(const ParsedArgs &args);
  static int getProps(const ParsedArgs &args);
  static int setProps(const ParsedArgs &args);
};

} // namespace vxcore_cli

#endif
