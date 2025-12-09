#ifndef VXCORE_CLI_ARGS_H
#define VXCORE_CLI_ARGS_H

#include <map>
#include <string>
#include <vector>

namespace vxcore_cli {

struct ParsedArgs {
  std::string command;
  std::string subcommand;
  std::map<std::string, std::string> options;
  std::map<std::string, std::vector<std::string>> multi_options;
  std::vector<std::string> positional;
};

class ArgsParser {
 public:
  static ParsedArgs parse(int argc, char *argv[]);
  static void showHelp();

 private:
  static bool isOption(const std::string &arg);
  static std::string getOptionName(const std::string &arg);
};

}  // namespace vxcore_cli

#endif
