#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "core/content_processor/content_processor.h"
#include "core/content_processor/markdown_handler.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

// ============================================================================
// DiscoverAssetLinks tests
// ============================================================================

static int test_discover_image_link() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](vx_assets/abc-123/pic.png)";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0].url, "vx_assets/abc-123/pic.png");
  ASSERT_TRUE(results[0].is_image);
  // Verify byte offsets: content is "![img](vx_assets/abc-123/pic.png)"
  // URL starts at index 7, ends at 32
  ASSERT_EQ(results[0].url_start_offset, (size_t)7);
  ASSERT_EQ(results[0].url_end_offset, (size_t)32);
  // Verify the offsets extract the URL correctly
  std::string extracted =
      content.substr(results[0].url_start_offset,
                     results[0].url_end_offset - results[0].url_start_offset);
  ASSERT_EQ(extracted, "vx_assets/abc-123/pic.png");
  return 0;
}

static int test_discover_regular_link() {
  vxcore::MarkdownHandler handler;
  std::string content = "[doc](vx_assets/abc-123/file.pdf)";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0].url, "vx_assets/abc-123/file.pdf");
  ASSERT_FALSE(results[0].is_image);
  // URL starts at index 6
  ASSERT_EQ(results[0].url_start_offset, (size_t)6);
  std::string extracted =
      content.substr(results[0].url_start_offset,
                     results[0].url_end_offset - results[0].url_start_offset);
  ASSERT_EQ(extracted, "vx_assets/abc-123/file.pdf");
  return 0;
}

static int test_discover_skip_code_block() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "```\n![img](vx_assets/abc-123/pic.png)\n```\n";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_discover_skip_inline_code() {
  vxcore::MarkdownHandler handler;
  std::string content = "`![img](vx_assets/abc-123/pic.png)`";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_discover_multiple_links() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "# Title\n\n"
      "![img1](vx_assets/abc-123/pic1.png)\n\n"
      "Some text [doc](vx_assets/abc-123/file.pdf) more text\n\n"
      "![img2](vx_assets/abc-123/pic2.jpg)\n";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)3);
  ASSERT_EQ(results[0].url, "vx_assets/abc-123/pic1.png");
  ASSERT_TRUE(results[0].is_image);
  ASSERT_EQ(results[1].url, "vx_assets/abc-123/file.pdf");
  ASSERT_FALSE(results[1].is_image);
  ASSERT_EQ(results[2].url, "vx_assets/abc-123/pic2.jpg");
  ASSERT_TRUE(results[2].is_image);

  // Verify offsets are in ascending order
  ASSERT_TRUE(results[0].url_start_offset < results[1].url_start_offset);
  ASSERT_TRUE(results[1].url_start_offset < results[2].url_start_offset);

  // Verify each offset extracts correctly
  for (const auto &link : results) {
    std::string extracted =
        content.substr(link.url_start_offset,
                       link.url_end_offset - link.url_start_offset);
    ASSERT_EQ(extracted, link.url);
  }
  return 0;
}

static int test_discover_non_asset_link() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](https://example.com/pic.png)";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_discover_empty_content() {
  vxcore::MarkdownHandler handler;
  std::string content;
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_discover_mixed_asset_and_external() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "![local](vx_assets/abc-123/pic.png)\n"
      "![remote](https://example.com/pic.png)\n"
      "[ref](vx_assets/abc-123/doc.pdf)\n";
  auto results = handler.DiscoverAssetLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)2);
  ASSERT_EQ(results[0].url, "vx_assets/abc-123/pic.png");
  ASSERT_EQ(results[1].url, "vx_assets/abc-123/doc.pdf");
  return 0;
}

// ============================================================================
// RewriteAssetLinks tests
// ============================================================================

static int test_rewrite_single_link() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](vx_assets/old-uuid/pic.png)";
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  ASSERT_EQ(result, "![img](vx_assets/new-uuid/pic.png)");
  return 0;
}

static int test_rewrite_multiple_links() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "![img1](vx_assets/old-uuid/pic1.png)\n"
      "Some text [doc](vx_assets/old-uuid/file.pdf)\n"
      "![img2](vx_assets/old-uuid/pic2.jpg)\n";
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  // All three links should be rewritten
  ASSERT_NE(result.find("vx_assets/new-uuid/pic1.png"), std::string::npos);
  ASSERT_NE(result.find("vx_assets/new-uuid/file.pdf"), std::string::npos);
  ASSERT_NE(result.find("vx_assets/new-uuid/pic2.jpg"), std::string::npos);
  // Old UUID should be gone
  ASSERT_EQ(result.find("vx_assets/old-uuid"), std::string::npos);
  return 0;
}

static int test_rewrite_skip_code_block() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "```\n![img](vx_assets/old-uuid/pic.png)\n```\n";
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  // Code block should be untouched
  ASSERT_EQ(result, content);
  return 0;
}

static int test_rewrite_non_asset_unchanged() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](https://example.com/pic.png)";
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  ASSERT_EQ(result, content);
  return 0;
}

static int test_rewrite_formatting_preserved() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "# Heading\n\n"
      "Some **bold** text with ![img](vx_assets/old-uuid/pic.png) inline.\n\n"
      "> Blockquote with [link](vx_assets/old-uuid/doc.pdf)\n\n"
      "- List item\n";
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  // Formatting preserved — only the UUID portion changed
  ASSERT_NE(result.find("# Heading"), std::string::npos);
  ASSERT_NE(result.find("Some **bold** text"), std::string::npos);
  ASSERT_NE(result.find("> Blockquote"), std::string::npos);
  ASSERT_NE(result.find("- List item"), std::string::npos);
  ASSERT_NE(result.find("vx_assets/new-uuid/pic.png"), std::string::npos);
  ASSERT_NE(result.find("vx_assets/new-uuid/doc.pdf"), std::string::npos);
  ASSERT_EQ(result.find("vx_assets/old-uuid"), std::string::npos);
  return 0;
}

static int test_rewrite_inline_code_unchanged() {
  vxcore::MarkdownHandler handler;
  std::string content = "`![img](vx_assets/old-uuid/pic.png)`";
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  // Inline code should be untouched
  ASSERT_EQ(result, content);
  return 0;
}

static int test_rewrite_empty_content() {
  vxcore::MarkdownHandler handler;
  std::string content;
  std::string result =
      handler.RewriteAssetLinks(content, "vx_assets/old-uuid", "vx_assets/new-uuid");

  ASSERT_EQ(result, "");
  return 0;
}

// ============================================================================
// DiscoverRelativeLinks tests
// ============================================================================

static int test_relative_basic_image() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](images/pic.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "images/pic.png");
  return 0;
}

static int test_relative_basic_link() {
  vxcore::MarkdownHandler handler;
  std::string content = "[doc](docs/report.pdf)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "docs/report.pdf");
  return 0;
}

static int test_relative_nested_path() {
  vxcore::MarkdownHandler handler;
  std::string content = "[f](a/b/c/file.txt)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "a/b/c/file.txt");
  return 0;
}

static int test_relative_skip_parent_prefix() {
  vxcore::MarkdownHandler handler;
  std::string content = "[up](../other/file.md)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_after_normalization() {
  vxcore::MarkdownHandler handler;
  // sub/../../other.md normalizes to ../other.md
  std::string content = "[tricky](sub/../../other.md)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_asset_prefix() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](vx_assets/abc-123/pic.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_absolute_path() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](/absolute/path.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_http_url() {
  vxcore::MarkdownHandler handler;
  std::string content = "![img](https://example.com/pic.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_ftp_url() {
  vxcore::MarkdownHandler handler;
  std::string content = "[ftp](ftp://server/file)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_code_block() {
  vxcore::MarkdownHandler handler;
  std::string content = "```\n![img](images/pic.png)\n```\n";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_skip_inline_code() {
  vxcore::MarkdownHandler handler;
  std::string content = "`[link](file.md)`";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_deduplicate() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "[link1](docs/file.md)\n"
      "[link2](docs/file.md)\n";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "docs/file.md");
  return 0;
}

static int test_relative_strip_fragment() {
  vxcore::MarkdownHandler handler;
  std::string content = "[ref](file.md#section)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "file.md");
  return 0;
}

static int test_relative_strip_query() {
  vxcore::MarkdownHandler handler;
  std::string content = "[ref](file.md?v=2)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "file.md");
  return 0;
}

static int test_relative_mixed_content() {
  vxcore::MarkdownHandler handler;
  std::string content =
      "![asset](vx_assets/abc-123/pic.png)\n"
      "[relative](docs/file.md)\n"
      "![external](https://example.com/pic.png)\n"
      "[another](notes/todo.txt)\n";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)2);
  ASSERT_EQ(results[0], "docs/file.md");
  ASSERT_EQ(results[1], "notes/todo.txt");
  return 0;
}

static int test_relative_empty_content() {
  vxcore::MarkdownHandler handler;
  std::string content;
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

static int test_relative_bare_filename() {
  vxcore::MarkdownHandler handler;
  std::string content = "[f](notes.md)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "notes.md");
  return 0;
}

static int test_relative_dot_slash_prefix() {
  vxcore::MarkdownHandler handler;
  // ./images/pic.png normalizes to images/pic.png
  std::string content = "[f](./images/pic.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "images/pic.png");
  return 0;
}

static int test_relative_legacy_folder_paths() {
  vxcore::MarkdownHandler handler;
  // Legacy folders are just relative paths, not excluded by asset prefix
  std::string content = "![img](_v_images/pic.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)1);
  ASSERT_EQ(results[0], "_v_images/pic.png");
  return 0;
}

static int test_relative_percent_encoding_spike() {
  // Spike test: determine what cmark returns for %20-encoded URLs
  vxcore::MarkdownHandler handler;
  std::string content = "![img](my%20image.png)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  // cmark returns raw URL string as-is (with %20)
  // This test documents the actual behavior
  ASSERT_EQ(results.size(), (size_t)1);
  // Print what we got for debugging
  std::cout << "  [spike] cmark returned: \"" << results[0] << "\"" << std::endl;
  // We expect cmark preserves %20 as-is (does NOT decode)
  ASSERT_EQ(results[0], "my%20image.png");
  return 0;
}

static int test_relative_skip_empty_fragment_only() {
  // [ref](#section) -> after stripping fragment, path is empty -> skip
  vxcore::MarkdownHandler handler;
  std::string content = "[ref](#section)";
  auto results = handler.DiscoverRelativeLinks(content, "vx_assets/abc-123");

  ASSERT_EQ(results.size(), (size_t)0);
  return 0;
}

// ============================================================================
// ContentProcessor dispatcher tests
// ============================================================================

static int test_dispatcher_md() {
  vxcore::ContentProcessor processor;
  auto *handler = processor.GetHandler("md");
  ASSERT_NOT_NULL(handler);
  return 0;
}

static int test_dispatcher_markdown() {
  vxcore::ContentProcessor processor;
  ASSERT_NOT_NULL(processor.GetHandler("markdown"));
  return 0;
}

static int test_dispatcher_mkd() {
  vxcore::ContentProcessor processor;
  ASSERT_NOT_NULL(processor.GetHandler("mkd"));
  return 0;
}

static int test_dispatcher_rmd() {
  vxcore::ContentProcessor processor;
  ASSERT_NOT_NULL(processor.GetHandler("rmd"));
  return 0;
}

static int test_dispatcher_unknown() {
  vxcore::ContentProcessor processor;
  ASSERT_NULL(processor.GetHandler("txt"));
  return 0;
}

static int test_dispatcher_empty() {
  vxcore::ContentProcessor processor;
  ASSERT_NULL(processor.GetHandler(""));
  return 0;
}

static int test_dispatcher_has_handler() {
  vxcore::ContentProcessor processor;
  ASSERT_TRUE(processor.HasHandler("md"));
  ASSERT_FALSE(processor.HasHandler("unknown"));
  return 0;
}

// ============================================================================
// main
// ============================================================================

int main() {
  vxcore_set_test_mode(1);

  // DiscoverAssetLinks
  RUN_TEST(test_discover_image_link);
  RUN_TEST(test_discover_regular_link);
  RUN_TEST(test_discover_skip_code_block);
  RUN_TEST(test_discover_skip_inline_code);
  RUN_TEST(test_discover_multiple_links);
  RUN_TEST(test_discover_non_asset_link);
  RUN_TEST(test_discover_empty_content);
  RUN_TEST(test_discover_mixed_asset_and_external);

  // RewriteAssetLinks
  RUN_TEST(test_rewrite_single_link);
  RUN_TEST(test_rewrite_multiple_links);
  RUN_TEST(test_rewrite_skip_code_block);
  RUN_TEST(test_rewrite_non_asset_unchanged);
  RUN_TEST(test_rewrite_formatting_preserved);
  RUN_TEST(test_rewrite_inline_code_unchanged);
  RUN_TEST(test_rewrite_empty_content);

  // DiscoverRelativeLinks
  RUN_TEST(test_relative_basic_image);
  RUN_TEST(test_relative_basic_link);
  RUN_TEST(test_relative_nested_path);
  RUN_TEST(test_relative_skip_parent_prefix);
  RUN_TEST(test_relative_skip_after_normalization);
  RUN_TEST(test_relative_skip_asset_prefix);
  RUN_TEST(test_relative_skip_absolute_path);
  RUN_TEST(test_relative_skip_http_url);
  RUN_TEST(test_relative_skip_ftp_url);
  RUN_TEST(test_relative_skip_code_block);
  RUN_TEST(test_relative_skip_inline_code);
  RUN_TEST(test_relative_deduplicate);
  RUN_TEST(test_relative_strip_fragment);
  RUN_TEST(test_relative_strip_query);
  RUN_TEST(test_relative_mixed_content);
  RUN_TEST(test_relative_empty_content);
  RUN_TEST(test_relative_bare_filename);
  RUN_TEST(test_relative_dot_slash_prefix);
  RUN_TEST(test_relative_legacy_folder_paths);
  RUN_TEST(test_relative_percent_encoding_spike);
  RUN_TEST(test_relative_skip_empty_fragment_only);

  // ContentProcessor dispatcher
  RUN_TEST(test_dispatcher_md);
  RUN_TEST(test_dispatcher_markdown);
  RUN_TEST(test_dispatcher_mkd);
  RUN_TEST(test_dispatcher_rmd);
  RUN_TEST(test_dispatcher_unknown);
  RUN_TEST(test_dispatcher_empty);
  RUN_TEST(test_dispatcher_has_handler);

  std::cout << "All content_processor tests passed!" << std::endl;
  return 0;
}
