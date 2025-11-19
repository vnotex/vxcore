#include "notebook_cmd.h"

#include <iostream>

#include "json_helpers.h"
#include "vxcore/vxcore.h"

namespace vxcore_cli {

int NotebookCommand::execute(const ParsedArgs &args) {
  if (args.subcommand.empty() || args.options.count("help")) {
    ArgsParser::showNotebookHelp();
    return 0;
  }

  if (args.subcommand == "create") {
    return create(args);
  } else if (args.subcommand == "open") {
    return open(args);
  } else if (args.subcommand == "close") {
    return close(args);
  } else if (args.subcommand == "list") {
    return list(args);
  } else if (args.subcommand == "get-props") {
    return getProps(args);
  } else if (args.subcommand == "set-props") {
    return setProps(args);
  } else {
    std::cerr << "Unknown subcommand: " << args.subcommand << std::endl;
    ArgsParser::showNotebookHelp();
    return 1;
  }
}

int NotebookCommand::create(const ParsedArgs &args) {
  if (args.options.count("path") == 0) {
    std::cerr << "Error: --path is required" << std::endl;
    return 1;
  }

  std::string path = args.options.at("path");
  std::string typeStr = args.options.count("type") ? args.options.at("type") : "bundled";

  VxCoreNotebookType type = VXCORE_NOTEBOOK_BUNDLED;
  if (typeStr == "raw") {
    type = VXCORE_NOTEBOOK_RAW;
  } else if (typeStr != "bundled") {
    std::cerr << "Error: Invalid type '" << typeStr << "'. Use 'bundled' or 'raw'." << std::endl;
    return 1;
  }

  try {
    std::string propsOption = args.options.count("props") ? args.options.at("props") : "";
    std::string propsJsonOption =
        args.options.count("props-json") ? args.options.at("props-json") : "";
    std::vector<std::string> propsList;
    if (args.multi_options.count("prop")) {
      propsList = args.multi_options.at("prop");
    }

    nlohmann::json properties =
        JsonHelpers::buildProperties(propsOption, propsJsonOption, propsList);
    std::string propertiesJson = properties.dump();

    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) {
      std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
      return 1;
    }

    char *notebook_id = nullptr;
    err = vxcore_notebook_create(ctx, path.c_str(), propertiesJson.c_str(), type, &notebook_id);

    if (err != VXCORE_OK) {
      std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
      vxcore_context_destroy(ctx);
      return 1;
    }

    std::cout << notebook_id << std::endl;
    vxcore_string_free(notebook_id);
    vxcore_context_destroy(ctx);
    return 0;

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}

int NotebookCommand::open(const ParsedArgs &args) {
  if (args.options.count("path") == 0) {
    std::cerr << "Error: --path is required" << std::endl;
    return 1;
  }

  std::string path = args.options.at("path");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  char *notebook_id = nullptr;
  err = vxcore_notebook_open(ctx, path.c_str(), &notebook_id);

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << notebook_id << std::endl;
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  return 0;
}

int NotebookCommand::close(const ParsedArgs &args) {
  if (args.options.count("id") == 0) {
    std::cerr << "Error: --id is required" << std::endl;
    return 1;
  }

  std::string id = args.options.at("id");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  err = vxcore_notebook_close(ctx, id.c_str());

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << "Notebook closed" << std::endl;
  vxcore_context_destroy(ctx);
  return 0;
}

int NotebookCommand::list(const ParsedArgs &args) {
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  char *notebooks_json = nullptr;
  err = vxcore_notebook_list(ctx, &notebooks_json);

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  if (args.options.count("json")) {
    std::cout << notebooks_json << std::endl;
  } else {
    try {
      nlohmann::json notebooks = nlohmann::json::parse(notebooks_json);
      if (notebooks.empty()) {
        std::cout << "No notebooks opened" << std::endl;
      } else {
        std::cout << "Opened notebooks:" << std::endl;
        for (const auto &nb : notebooks) {
          std::cout << "  " << nb["id"].get<std::string>() << " - "
                    << nb["rootFolder"].get<std::string>() << " (" << nb["type"].get<std::string>()
                    << ")" << std::endl;
        }
      }
    } catch (const std::exception &e) {
      std::cerr << "Error parsing notebooks: " << e.what() << std::endl;
      vxcore_string_free(notebooks_json);
      vxcore_context_destroy(ctx);
      return 1;
    }
  }

  vxcore_string_free(notebooks_json);
  vxcore_context_destroy(ctx);
  return 0;
}

int NotebookCommand::getProps(const ParsedArgs &args) {
  if (args.options.count("id") == 0) {
    std::cerr << "Error: --id is required" << std::endl;
    return 1;
  }

  std::string id = args.options.at("id");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: {" << id << "} " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  char *properties = nullptr;
  err = vxcore_notebook_get_properties(ctx, id.c_str(), &properties);

  if (err != VXCORE_OK) {
    std::cerr << "Error: {" << id << "} " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << properties << std::endl;
  vxcore_string_free(properties);
  vxcore_context_destroy(ctx);
  return 0;
}

int NotebookCommand::setProps(const ParsedArgs &args) {
  if (args.options.count("id") == 0) {
    std::cerr << "Error: --id is required" << std::endl;
    return 1;
  }

  std::string id = args.options.at("id");

  try {
    std::string propsOption = args.options.count("props") ? args.options.at("props") : "";
    std::string propsJsonOption =
        args.options.count("props-json") ? args.options.at("props-json") : "";
    std::vector<std::string> propsList;
    if (args.multi_options.count("prop")) {
      propsList = args.multi_options.at("prop");
    }

    nlohmann::json properties =
        JsonHelpers::buildProperties(propsOption, propsJsonOption, propsList);
    std::string propertiesJson = properties.dump();

    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) {
      std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
      return 1;
    }

    err = vxcore_notebook_set_properties(ctx, id.c_str(), propertiesJson.c_str());

    if (err != VXCORE_OK) {
      std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
      vxcore_context_destroy(ctx);
      return 1;
    }

    std::cout << "Properties updated" << std::endl;
    vxcore_context_destroy(ctx);
    return 0;

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}

}  // namespace vxcore_cli
