#include "config_cmd.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "vxcore/vxcore.h"

namespace vxcore_cli {

int ConfigCommand::execute(const ParsedArgs &args) {
  if (args.subcommand.empty() || args.options.count("help")) {
    std::cout << "VxCore Configuration Management\n\n";
    std::cout << "Usage: vxcore config <subcommand> [options]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  dump                 Dump all config information\n\n";
    std::cout << "Dump options:\n";
    std::cout << "  --show-contents      Show file contents (default: true)\n";
    std::cout << "  --paths-only         Show only file paths\n\n";
    std::cout << "Examples:\n";
    std::cout << "  vxcore config dump\n";
    std::cout << "  vxcore config dump --paths-only\n";
    return 0;
  }

  if (args.subcommand == "dump") {
    return ConfigCommand::dump(args);
  } else {
    std::cerr << "Unknown subcommand: " << args.subcommand << std::endl;
    return 1;
  }
}

int ConfigCommand::dump(const ParsedArgs &args) {
  bool paths_only = args.options.count("paths-only") > 0;
  bool show_contents = !paths_only;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  char *config_path = nullptr;
  char *session_config_path = nullptr;
  char *config_json = nullptr;
  char *session_config_json = nullptr;

  err = vxcore_context_get_config_path(ctx, &config_path);
  if (err != VXCORE_OK) {
    std::cerr << "Error getting config path: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  err = vxcore_context_get_session_config_path(ctx, &session_config_path);
  if (err != VXCORE_OK) {
    std::cerr << "Error getting session config path: " << vxcore_error_message(err) << std::endl;
    vxcore_string_free(config_path);
    vxcore_context_destroy(ctx);
    return 1;
  }

  if (show_contents) {
    err = vxcore_context_get_config(ctx, &config_json);
    if (err != VXCORE_OK) {
      std::cerr << "Error getting config: " << vxcore_error_message(err) << std::endl;
      vxcore_string_free(config_path);
      vxcore_string_free(session_config_path);
      vxcore_context_destroy(ctx);
      return 1;
    }

    err = vxcore_context_get_session_config(ctx, &session_config_json);
    if (err != VXCORE_OK) {
      std::cerr << "Error getting session config: " << vxcore_error_message(err) << std::endl;
      vxcore_string_free(config_path);
      vxcore_string_free(session_config_path);
      vxcore_string_free(config_json);
      vxcore_context_destroy(ctx);
      return 1;
    }
  }

  std::cout << "=== VxCore Configuration ===\n\n";

  std::cout << "App Config Path:\n  " << config_path << "\n";
  bool config_exists = std::filesystem::exists(config_path);
  std::cout << "  Exists: " << (config_exists ? "yes" : "no") << "\n\n";

  if (show_contents && config_exists) {
    std::cout << "App Config Contents:\n" << config_json << "\n\n";
  }

  std::cout << "Session Config Path:\n  " << session_config_path << "\n";
  bool session_exists = std::filesystem::exists(session_config_path);
  std::cout << "  Exists: " << (session_exists ? "yes" : "no") << "\n\n";

  if (show_contents && session_exists) {
    std::cout << "Session Config Contents:\n" << session_config_json << "\n\n";
  }

  vxcore_string_free(config_path);
  vxcore_string_free(session_config_path);
  if (config_json) {
    vxcore_string_free(config_json);
  }
  if (session_config_json) {
    vxcore_string_free(session_config_json);
  }
  vxcore_context_destroy(ctx);

  return 0;
}

}  // namespace vxcore_cli
