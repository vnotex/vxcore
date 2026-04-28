#include "core/content_processor/markdown_handler.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <cmark.h>

#include "utils/file_utils.h"

namespace vxcore {

std::vector<LinkInfo> MarkdownHandler::DiscoverAssetLinks(
    const std::string &content,
    const std::string &assets_folder_prefix) const {
  std::vector<LinkInfo> results;
  if (content.empty() || assets_folder_prefix.empty()) {
    return results;
  }

  cmark_node *document =
      cmark_parse_document(content.c_str(), content.size(), CMARK_OPT_DEFAULT);
  if (!document) {
    return results;
  }

  // Collect matching URLs from AST (cmark skips code blocks/inline code).
  struct AstLink {
    std::string url;
    bool is_image;
  };
  std::vector<AstLink> ast_links;

  cmark_iter *iter = cmark_iter_new(document);
  cmark_event_type ev;
  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    if (ev != CMARK_EVENT_ENTER) {
      continue;
    }
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);
    if (type != CMARK_NODE_LINK && type != CMARK_NODE_IMAGE) {
      continue;
    }
    const char *url = cmark_node_get_url(node);
    if (!url) {
      continue;
    }
    std::string url_str(url);
    if (url_str.rfind(assets_folder_prefix, 0) == 0) {
      ast_links.push_back({url_str, type == CMARK_NODE_IMAGE});
    }
  }
  cmark_iter_free(iter);
  cmark_node_free(document);

  // Find byte offsets by scanning the original content for each URL.
  // URLs appear in order, so we advance search_start to avoid duplicates.
  size_t search_start = 0;
  for (const auto &link : ast_links) {
    while (search_start < content.size()) {
      size_t pos = content.find(link.url, search_start);
      if (pos == std::string::npos) {
        break;
      }
      // Verify this URL is inside markdown link syntax: preceded by '('
      // and followed by ')' or space/quote (for title).
      bool valid = false;
      if (pos > 0 && content[pos - 1] == '(') {
        size_t end = pos + link.url.size();
        if (end <= content.size()) {
          if (end == content.size() || content[end] == ')' ||
              content[end] == ' ' || content[end] == '"' ||
              content[end] == '\'') {
            valid = true;
          }
        }
      }
      if (valid) {
        LinkInfo info;
        info.url = link.url;
        info.url_start_offset = pos;
        info.url_end_offset = pos + link.url.size();
        info.is_image = link.is_image;
        results.push_back(info);
        search_start = pos + link.url.size();
        break;
      }
      search_start = pos + 1;
    }
  }

  return results;
}

std::string MarkdownHandler::RewriteAssetLinks(
    const std::string &content,
    const std::string &old_assets_path,
    const std::string &new_assets_path) const {
  if (content.empty()) {
    return content;
  }

  auto links = DiscoverAssetLinks(content, old_assets_path);
  if (links.empty()) {
    return content;
  }

  // Sort by offset descending so replacements don't shift earlier positions.
  std::sort(links.begin(), links.end(),
            [](const LinkInfo &a, const LinkInfo &b) {
              return a.url_start_offset > b.url_start_offset;
            });

  std::string result = content;
  for (const auto &link : links) {
    // Build new URL by replacing old_assets_path with new_assets_path.
    std::string new_url = link.url;
    size_t prefix_pos = new_url.find(old_assets_path);
    if (prefix_pos != std::string::npos) {
      new_url.replace(prefix_pos, old_assets_path.size(), new_assets_path);
    }
    result.replace(link.url_start_offset,
                   link.url_end_offset - link.url_start_offset, new_url);
  }

  return result;
}

std::vector<std::string> MarkdownHandler::DiscoverRelativeLinks(
    const std::string &content,
    const std::string &assets_folder_prefix) const {
  std::vector<std::string> results;
  if (content.empty()) {
    return results;
  }

  cmark_node *document =
      cmark_parse_document(content.c_str(), content.size(), CMARK_OPT_DEFAULT);
  if (!document) {
    return results;
  }

  std::string asset_prefix = assets_folder_prefix.empty()
                                 ? ""
                                 : assets_folder_prefix + "/";

  std::set<std::string> seen;

  cmark_iter *iter = cmark_iter_new(document);
  cmark_event_type ev;
  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    if (ev != CMARK_EVENT_ENTER) {
      continue;
    }
    cmark_node *node = cmark_iter_get_node(iter);
    cmark_node_type type = cmark_node_get_type(node);
    if (type != CMARK_NODE_LINK && type != CMARK_NODE_IMAGE) {
      continue;
    }
    const char *url = cmark_node_get_url(node);
    if (!url) {
      continue;
    }
    std::string url_str(url);

    // Skip empty or "."
    if (url_str.empty() || url_str == ".") {
      continue;
    }

    // Skip URLs with "://"
    if (url_str.find("://") != std::string::npos) {
      continue;
    }

    // Skip absolute paths
    if (url_str[0] == '/') {
      continue;
    }

    // Strip fragment (#)
    auto hash_pos = url_str.find('#');
    if (hash_pos != std::string::npos) {
      url_str = url_str.substr(0, hash_pos);
    }

    // Strip query (?)
    auto query_pos = url_str.find('?');
    if (query_pos != std::string::npos) {
      url_str = url_str.substr(0, query_pos);
    }

    // Skip if empty after stripping
    if (url_str.empty()) {
      continue;
    }

    // Normalize via CleanPath
    std::string normalized = CleanPath(url_str);

    // CleanPath("") returns "." — skip
    if (normalized == ".") {
      continue;
    }

    // Skip if starts with "../"
    if (normalized.rfind("../", 0) == 0) {
      continue;
    }

    // Skip if starts with assets_folder_prefix + "/"
    if (!asset_prefix.empty() && normalized.rfind(asset_prefix, 0) == 0) {
      continue;
    }

    // Deduplicate
    if (seen.insert(normalized).second) {
      results.push_back(normalized);
    }
  }
  cmark_iter_free(iter);
  cmark_node_free(document);

  return results;
}

}  // namespace vxcore
