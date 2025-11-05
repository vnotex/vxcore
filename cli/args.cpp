#include "args.h"
#include <iostream>

namespace vxcore_cli {

bool ArgsParser::isOption(const std::string &arg) {
  return arg.size() >= 2 && arg[0] == '-';
}

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
  std::cout << "Usage: vxcore <command> [options]\n\n";
  std::cout << "Commands:\n";
  std::cout << "  version              Show version information\n";
  std::cout << "  notebook             Notebook management commands\n";
  std::cout << "  help                 Show this help message\n\n";
  std::cout << "Run 'vxcore <command> --help' for more information on a command.\n";
}

void ArgsParser::showNotebookHelp() {
  std::cout << "VxCore Notebook Management\n\n";
  std::cout << "Usage: vxcore notebook <subcommand> [options]\n\n";
  std::cout << "Subcommands:\n";
  std::cout << "  create               Create a new notebook\n";
  std::cout << "  open                 Open an existing notebook\n";
  std::cout << "  close                Close a notebook\n";
  std::cout << "  list                 List all opened notebooks\n";
  std::cout << "  get-props            Get notebook properties\n";
  std::cout << "  set-props            Set notebook properties\n\n";
  std::cout << "Create options:\n";
  std::cout << "  --path PATH          Path to notebook directory (required)\n";
  std::cout << "  --type TYPE          Notebook type: bundled (default) or raw\n";
  std::cout << "  --props @FILE        Load JSON properties from file\n";
  std::cout << "  --props -            Read JSON properties from STDIN\n";
  std::cout << "  --props-json JSON    Inline JSON string\n";
  std::cout << "  --prop key=value     Set a property (repeatable)\n\n";
  std::cout << "Open options:\n";
  std::cout << "  --path PATH          Path to notebook directory (required)\n\n";
  std::cout << "Close options:\n";
  std::cout << "  --id ID              Notebook ID (required)\n\n";
  std::cout << "List options:\n";
  std::cout << "  --json               Output as JSON\n\n";
  std::cout << "Get-props options:\n";
  std::cout << "  --id ID              Notebook ID (required)\n";
  std::cout << "  --json               Output as JSON (default)\n\n";
  std::cout << "Set-props options:\n";
  std::cout << "  --id ID              Notebook ID (required)\n";
  std::cout << "  --props @FILE        Load JSON properties from file\n";
  std::cout << "  --props -            Read JSON properties from STDIN\n";
  std::cout << "  --props-json JSON    Inline JSON string\n";
  std::cout << "  --prop key=value     Set a property (repeatable)\n\n";
  std::cout << "Examples:\n";
  std::cout << "  vxcore notebook create --path ./notes --prop name=\"My Notes\"\n";
  std::cout << "  vxcore notebook create --path ./notes --props @config.json\n";
  std::cout << "  vxcore notebook list --json\n";
  std::cout << "  vxcore notebook get-props --id <uuid>\n";
}

} // namespace vxcore_cli
