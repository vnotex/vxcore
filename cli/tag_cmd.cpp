#include "tag_cmd.h"

#include <iostream>

#include "json_helpers.h"
#include "vxcore/vxcore.h"

namespace vxcore_cli {

void showTagHelp() {
  std::cout << "VxCore Tag Management\n\n";
  std::cout << "Usage: vxcli tag <subcommand> [options]\n\n";
  std::cout << "Subcommands:\n";
  std::cout << "  create               Create a new tag in a notebook\n";
  std::cout << "  delete               Delete a tag from a notebook\n";
  std::cout << "  list                 List all tags in a notebook\n";
  std::cout << "  add                  Add a tag to a file\n";
  std::cout << "  remove               Remove a tag from a file\n\n";
  std::cout << "Create options:\n";
  std::cout << "  --notebook ID        Notebook ID (required)\n";
  std::cout << "  --name NAME          Tag name (required)\n\n";
  std::cout << "Delete options:\n";
  std::cout << "  --notebook ID        Notebook ID (required)\n";
  std::cout << "  --name NAME          Tag name (required)\n\n";
  std::cout << "List options:\n";
  std::cout << "  --notebook ID        Notebook ID (required)\n";
  std::cout << "  --json               Output as JSON\n\n";
  std::cout << "Add options:\n";
  std::cout << "  --notebook ID        Notebook ID (required)\n";
  std::cout << "  --file PATH          File path relative to notebook (required)\n";
  std::cout << "  --name NAME          Tag name (required)\n\n";
  std::cout << "Remove options:\n";
  std::cout << "  --notebook ID        Notebook ID (required)\n";
  std::cout << "  --file PATH          File path relative to notebook (required)\n";
  std::cout << "  --name NAME          Tag name (required)\n\n";
  std::cout << "Examples:\n";
  std::cout << "  vxcli tag create --notebook <uuid> --name work\n";
  std::cout << "  vxcli tag list --notebook <uuid>\n";
  std::cout << "  vxcli tag add --notebook <uuid> --file notes.md --name work\n";
  std::cout << "  vxcli tag remove --notebook <uuid> --file notes.md --name work\n";
  std::cout << "  vxcli tag delete --notebook <uuid> --name work\n";
}

int TagCommand::execute(const ParsedArgs &args) {
  if (args.subcommand.empty() || args.options.count("help")) {
    showTagHelp();
    return 0;
  }

  if (args.subcommand == "create") {
    return create(args);
  } else if (args.subcommand == "delete") {
    return deleteTag(args);
  } else if (args.subcommand == "list") {
    return list(args);
  } else if (args.subcommand == "add") {
    return addToFile(args);
  } else if (args.subcommand == "remove") {
    return removeFromFile(args);
  } else {
    std::cerr << "Unknown subcommand: " << args.subcommand << std::endl;
    showTagHelp();
    return 1;
  }
}

int TagCommand::create(const ParsedArgs &args) {
  if (args.options.count("notebook") == 0) {
    std::cerr << "Error: --notebook is required" << std::endl;
    return 1;
  }
  if (args.options.count("name") == 0) {
    std::cerr << "Error: --name is required" << std::endl;
    return 1;
  }

  std::string notebook_id = args.options.at("notebook");
  std::string tag_name = args.options.at("name");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  err = vxcore_tag_create(ctx, notebook_id.c_str(), tag_name.c_str());

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << "Tag created: " << tag_name << std::endl;
  vxcore_context_destroy(ctx);
  return 0;
}

int TagCommand::deleteTag(const ParsedArgs &args) {
  if (args.options.count("notebook") == 0) {
    std::cerr << "Error: --notebook is required" << std::endl;
    return 1;
  }
  if (args.options.count("name") == 0) {
    std::cerr << "Error: --name is required" << std::endl;
    return 1;
  }

  std::string notebook_id = args.options.at("notebook");
  std::string tag_name = args.options.at("name");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  err = vxcore_tag_delete(ctx, notebook_id.c_str(), tag_name.c_str());

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << "Tag deleted: " << tag_name << std::endl;
  vxcore_context_destroy(ctx);
  return 0;
}

int TagCommand::list(const ParsedArgs &args) {
  if (args.options.count("notebook") == 0) {
    std::cerr << "Error: --notebook is required" << std::endl;
    return 1;
  }

  std::string notebook_id = args.options.at("notebook");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id.c_str(), &tags_json);

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  if (args.options.count("json")) {
    std::cout << tags_json << std::endl;
  } else {
    try {
      nlohmann::json tags = nlohmann::json::parse(tags_json);
      if (tags.empty()) {
        std::cout << "No tags in notebook" << std::endl;
      } else {
        std::cout << "Tags:" << std::endl;
        for (const auto &tag : tags) {
          std::cout << "  " << tag.get<std::string>() << std::endl;
        }
      }
    } catch (const std::exception &e) {
      std::cerr << "Error parsing tags: " << e.what() << std::endl;
      vxcore_string_free(tags_json);
      vxcore_context_destroy(ctx);
      return 1;
    }
  }

  vxcore_string_free(tags_json);
  vxcore_context_destroy(ctx);
  return 0;
}

int TagCommand::addToFile(const ParsedArgs &args) {
  if (args.options.count("notebook") == 0) {
    std::cerr << "Error: --notebook is required" << std::endl;
    return 1;
  }
  if (args.options.count("file") == 0) {
    std::cerr << "Error: --file is required" << std::endl;
    return 1;
  }
  if (args.options.count("name") == 0) {
    std::cerr << "Error: --name is required" << std::endl;
    return 1;
  }

  std::string notebook_id = args.options.at("notebook");
  std::string file_path = args.options.at("file");
  std::string tag_name = args.options.at("name");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  err = vxcore_file_tag(ctx, notebook_id.c_str(), file_path.c_str(), tag_name.c_str());

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << "Tag added to file: " << tag_name << std::endl;
  vxcore_context_destroy(ctx);
  return 0;
}

int TagCommand::removeFromFile(const ParsedArgs &args) {
  if (args.options.count("notebook") == 0) {
    std::cerr << "Error: --notebook is required" << std::endl;
    return 1;
  }
  if (args.options.count("file") == 0) {
    std::cerr << "Error: --file is required" << std::endl;
    return 1;
  }
  if (args.options.count("name") == 0) {
    std::cerr << "Error: --name is required" << std::endl;
    return 1;
  }

  std::string notebook_id = args.options.at("notebook");
  std::string file_path = args.options.at("file");
  std::string tag_name = args.options.at("name");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    return 1;
  }

  err = vxcore_file_untag(ctx, notebook_id.c_str(), file_path.c_str(), tag_name.c_str());

  if (err != VXCORE_OK) {
    std::cerr << "Error: " << vxcore_error_message(err) << std::endl;
    vxcore_context_destroy(ctx);
    return 1;
  }

  std::cout << "Tag removed from file: " << tag_name << std::endl;
  vxcore_context_destroy(ctx);
  return 0;
}

}  // namespace vxcore_cli
