#include <iostream>

#include "args.h"
#include "config_cmd.h"
#include "notebook_cmd.h"
#include "vxcore/vxcore.h"

int main(int argc, char *argv[]) {
  vxcore_cli::ParsedArgs args = vxcore_cli::ArgsParser::parse(argc, argv);

  if (args.command.empty() || args.command == "help" || args.options.count("help")) {
    vxcore_cli::ArgsParser::showHelp();
    return 0;
  }

  if (args.command == "version") {
    VxCoreVersion version = vxcore_get_version();
    std::cout << "VxCore v" << version.major << "." << version.minor << "." << version.patch
              << std::endl;
    return 0;
  }

  if (args.command == "notebook") {
    return vxcore_cli::NotebookCommand::execute(args);
  }

  if (args.command == "config") {
    return vxcore_cli::ConfigCommand::execute(args);
  }

  std::cerr << "Unknown command: " << args.command << std::endl;
  vxcore_cli::ArgsParser::showHelp();
  return 1;
}
