#include "args.h"

#include <iostream>

namespace vxcore_cli {

bool ArgsParser::isOption(const std::string &arg) { return arg.size() >= 2 && arg[0] == '-'; }

std::string ArgsParser::getOptionName(const std::string &arg) {
  if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
    return arg.substr(2);
  }
  if (arg.size() >= 2 && arg[0] == '-') {
    return arg.substr(1);
  }
  return arg;
}

ParsedArgs ArgsParser::parse(int argc, char *argv[]) {
  ParsedArgs result;

  if (argc < 2) {
    return result;
  }

  result.command = argv[1];

  int i = 2;
  if (argc > 2 && !isOption(argv[2])) {
    result.subcommand = argv[2];
    i = 3;
  }

  while (i < argc) {
    std::string arg = argv[i];

    if (isOption(arg)) {
      std::string optName = getOptionName(arg);

      if (i + 1 < argc && !isOption(argv[i + 1])) {
        std::string value = argv[i + 1];

        if (optName == "prop") {
          result.multi_options[optName].push_back(value);
        } else {
          result.options[optName] = value;
        }
        i += 2;
      } else {
        result.options[optName] = "true";
        i++;
      }
    } else {
      result.positional.push_back(arg);
      i++;
    }
  }

  return result;
}

void ArgsParser::showHelp() {
  std::cout << "VxCore CLI - Core library for note-taking applications\n\n";
  std::cout << "Usage: vxcli <command> [options]\n\n";
  std::cout << "Commands:\n";
  std::cout << "  version              Show version information\n";
  std::cout << "  notebook             Notebook management commands\n";
  std::cout << "  tag                  Tag management commands\n";
  std::cout << "  config               Configuration management commands\n";
  std::cout << "  help                 Show this help message\n\n";
  std::cout << "Run 'vxcli <command> --help' for more information on a command.\n";
}

}  // namespace vxcore_cli
