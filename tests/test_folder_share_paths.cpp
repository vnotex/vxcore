// test_folder_share_paths: Step 1 of the share-bundled-folder plan.
//
// Covers vxcore_folder_get_share_paths — the synchronous, path-only query that
// resolves a bundled folder's notebook root, physical content root, and
// parallel metadata root (<root>/vx_notebook/contents/<path>).
//
// The critical property under test is FULL root-to-selected index
// reachability: an orphan physical/metadata subtree whose ancestor edge is
// missing from a parent's vx.json must be rejected, even though the selected
// folder's own vx.json is present and well-formed.
//
// All assertions go through the PUBLIC C API plus direct on-disk edits of the
// bundled metadata layout:
//   - content:  <notebook_root>/<relative_path>
//   - metadata: <notebook_root>/vx_notebook/contents/<relative_path>/vx.json

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

std::string root_config_path(const std::string &nb) {
  return nb + "/vx_notebook/contents/vx.json";
}

std::string folder_config_path(const std::string &nb, const std::string &relative) {
  return nb + "/vx_notebook/contents/" + relative + "/vx.json";
}

std::string metadata_dir(const std::string &nb, const std::string &relative) {
  return nb + "/vx_notebook/contents/" + relative;
}

nlohmann::json read_json(const std::string &path) {
  std::ifstream file(utf8_to_fs_path(path), std::ios::binary);
  nlohmann::json json;
  if (file.is_open()) {
    try {
      file >> json;
    } catch (const std::exception &) {
      json = nlohmann::json();
    }
  }
  return json;
}

void write_json(const std::string &path, const nlohmann::json &json) {
  write_file(path, json.dump(2));
}

// Creates a bundled notebook with Projects/Alpha/Beta and returns its id.
// Caller frees with vxcore_string_free().
char *make_notebook(VxCoreContextHandle ctx, const std::string &nb_path) {
  char *notebook_id = nullptr;
  if (vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Share NB\"}",
                             VXCORE_NOTEBOOK_BUNDLED, &notebook_id) != VXCORE_OK) {
    return nullptr;
  }
  return notebook_id;
}

bool make_folder(VxCoreContextHandle ctx, const char *nb_id, const char *path) {
  char *folder_id = nullptr;
  const VxCoreError err = vxcore_folder_create_path(ctx, nb_id, path, &folder_id);
  vxcore_string_free(folder_id);
  return err == VXCORE_OK;
}

struct SharePaths {
  char *notebook_root = nullptr;
  char *content_root = nullptr;
  char *metadata_root = nullptr;

  ~SharePaths() {
    vxcore_string_free(notebook_root);
    vxcore_string_free(content_root);
    vxcore_string_free(metadata_root);
  }
};

}  // namespace

// Happy path: nested folder resolves all three roots; strings are owned by the
// caller and freed with vxcore_string_free().
int test_share_paths_nested() {
  std::cout << "  Running test_share_paths_nested..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_nested_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects"));
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects/Alpha"));
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects/Alpha/Beta"));

  {
    SharePaths paths;
    ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Projects/Alpha", &paths.notebook_root,
                                            &paths.content_root, &paths.metadata_root),
              VXCORE_OK);
    ASSERT_NOT_NULL(paths.notebook_root);
    ASSERT_NOT_NULL(paths.content_root);
    ASSERT_NOT_NULL(paths.metadata_root);

    ASSERT_EQ(normalize_path(paths.notebook_root), normalize_path(nb));
    ASSERT_EQ(normalize_path(paths.content_root), normalize_path(nb + "/Projects/Alpha"));
    ASSERT_EQ(normalize_path(paths.metadata_root),
              normalize_path(metadata_dir(nb, "Projects/Alpha")));

    // metadata_root is the DIRECTORY containing vx.json, not the file.
    ASSERT_TRUE(std::filesystem::is_directory(utf8_to_fs_path(paths.metadata_root)));
    ASSERT_TRUE(std::filesystem::is_regular_file(
        utf8_to_fs_path(std::string(paths.metadata_root) + "/vx.json")));
    // The descendant metadata directory is under it.
    ASSERT_TRUE(std::filesystem::is_regular_file(
        utf8_to_fs_path(std::string(paths.metadata_root) + "/Beta/vx.json")));
  }

  // Deepest folder also resolves.
  {
    SharePaths paths;
    ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Projects/Alpha/Beta",
                                            &paths.notebook_root, &paths.content_root,
                                            &paths.metadata_root),
              VXCORE_OK);
    ASSERT_EQ(normalize_path(paths.content_root),
              normalize_path(nb + "/Projects/Alpha/Beta"));
  }

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_nested passed" << std::endl;
  return 0;
}

// Unicode folder names round-trip through the UTF-8 path boundary.
int test_share_paths_unicode() {
  std::cout << "  Running test_share_paths_unicode..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_unicode_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  const std::string parent = u8"\u9879\u76ee";      // 项目
  const std::string child = u8"\u30ce\u30fc\u30c8";  // ノート
  ASSERT_TRUE(make_folder(ctx, nb_id, parent.c_str()));
  ASSERT_TRUE(make_folder(ctx, nb_id, (parent + "/" + child).c_str()));

  SharePaths paths;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, (parent + "/" + child).c_str(),
                                          &paths.notebook_root, &paths.content_root,
                                          &paths.metadata_root),
            VXCORE_OK);
  ASSERT_EQ(normalize_path(paths.content_root), normalize_path(nb + "/" + parent + "/" + child));
  ASSERT_TRUE(std::filesystem::is_directory(utf8_to_fs_path(paths.content_root)));
  ASSERT_TRUE(std::filesystem::is_directory(utf8_to_fs_path(paths.metadata_root)));

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_unicode passed" << std::endl;
  return 0;
}

// Argument hygiene: null args, root, absolute, "." / "..", escaping paths.
int test_share_paths_rejects_invalid_paths() {
  std::cout << "  Running test_share_paths_rejects_invalid_paths..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_invalid_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;

  // Null arguments.
  ASSERT_EQ(vxcore_folder_get_share_paths(nullptr, nb_id, "Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nullptr, "Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, nullptr, &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha", nullptr, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha", &a, nullptr, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha", &a, &b, nullptr),
            VXCORE_ERR_INVALID_PARAM);
  // Outputs stay null on every failure branch.
  ASSERT_NULL(a);
  ASSERT_NULL(b);
  ASSERT_NULL(c);

  // Root forms.
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "", &a, &b, &c), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, ".", &a, &b, &c), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "./", &a, &b, &c), VXCORE_ERR_INVALID_PARAM);

  // Escaping / absolute forms.
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "..", &a, &b, &c), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "../Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha/../..", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  // "." / ".." must be rejected AS WRITTEN, not silently normalized away.
  // Without the raw-component check these would collapse to a valid "Alpha".
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "./Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Projects/../Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha/./Sub", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "/Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
#ifdef _WIN32
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "C:/Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_PARAM);
#endif
  ASSERT_NULL(a);
  ASSERT_NULL(b);
  ASSERT_NULL(c);

  // Unknown notebook.
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, "no-such-notebook", "Alpha", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_invalid_paths passed" << std::endl;
  return 0;
}

// Raw notebooks have no parallel metadata tree at all.
int test_share_paths_rejects_raw_notebook() {
  std::cout << "  Running test_share_paths_rejects_raw_notebook..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_raw_nb");
  cleanup_test_dir(nb);
  create_directory(nb + "/Alpha");

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *nb_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb.c_str(), "{\"name\":\"Raw NB\"}", VXCORE_NOTEBOOK_RAW,
                                   &nb_id),
            VXCORE_OK);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha", &a, &b, &c),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(a);
  ASSERT_NULL(b);
  ASSERT_NULL(c);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_raw_notebook passed" << std::endl;
  return 0;
}

// Orphan selected metadata: the folder's own vx.json exists and its physical
// directory exists, but its parent no longer lists it in "folders".
int test_share_paths_rejects_orphan_selected() {
  std::cout << "  Running test_share_paths_rejects_orphan_selected..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_orphan_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);

  // Drop "Alpha" from the root config's folders[] while leaving the child's
  // vx.json + physical directory intact.
  nlohmann::json root = read_json(root_config_path(nb));
  ASSERT_TRUE(root.is_object());
  root["folders"] = nlohmann::json::array();
  write_json(root_config_path(nb), root);

  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, reopened, "Alpha", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);
  ASSERT_NULL(b);
  ASSERT_NULL(c);

  vxcore_string_free(reopened);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_orphan_selected passed" << std::endl;
  return 0;
}

// Nested orphan: Projects/vx.json lists Alpha, but the ROOT vx.json omits
// Projects. Checking only the immediate parent edge would wrongly accept this.
int test_share_paths_rejects_nested_orphan_ancestor() {
  std::cout << "  Running test_share_paths_rejects_nested_orphan_ancestor..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_nested_orphan_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects"));
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects/Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);

  // Root forgets Projects; Projects still lists Alpha.
  nlohmann::json root = read_json(root_config_path(nb));
  ASSERT_TRUE(root.is_object());
  root["folders"] = nlohmann::json::array();
  write_json(root_config_path(nb), root);

  nlohmann::json projects = read_json(folder_config_path(nb, "Projects"));
  ASSERT_TRUE(projects.is_object());
  ASSERT_TRUE(projects["folders"].is_array());
  ASSERT_EQ(projects["folders"].size(), static_cast<size_t>(1));

  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, reopened, "Projects/Alpha", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);

  vxcore_string_free(reopened);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_nested_orphan_ancestor passed" << std::endl;
  return 0;
}

// The selected folder's vx.json "name" must match its path component.
int test_share_paths_rejects_name_mismatch() {
  std::cout << "  Running test_share_paths_rejects_name_mismatch..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_name_mismatch_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);

  nlohmann::json alpha = read_json(folder_config_path(nb, "Alpha"));
  ASSERT_TRUE(alpha.is_object());
  alpha["name"] = "NotAlpha";
  write_json(folder_config_path(nb, "Alpha"), alpha);

  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, reopened, "Alpha", &a, &b, &c),
            VXCORE_ERR_INVALID_STATE);
  ASSERT_NULL(a);

  vxcore_string_free(reopened);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_name_mismatch passed" << std::endl;
  return 0;
}

// Missing physical content -> VXCORE_ERR_NODE_NOT_EXISTS (metadata intact).
int test_share_paths_rejects_missing_content() {
  std::cout << "  Running test_share_paths_rejects_missing_content..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_missing_content_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));

  std::error_code ec;
  std::filesystem::remove_all(utf8_to_fs_path(nb + "/Alpha"), ec);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha", &a, &b, &c),
            VXCORE_ERR_NODE_NOT_EXISTS);
  ASSERT_NULL(a);
  ASSERT_NULL(b);
  ASSERT_NULL(c);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_missing_content passed" << std::endl;
  return 0;
}

// Missing / malformed metadata surface as NOT_FOUND / JSON_PARSE.
int test_share_paths_rejects_bad_metadata() {
  std::cout << "  Running test_share_paths_rejects_bad_metadata..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_bad_metadata_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);

  // 1. Malformed selected vx.json.
  write_file(folder_config_path(nb, "Alpha"), "{ not json");

  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, reopened, "Alpha", &a, &b, &c),
            VXCORE_ERR_JSON_PARSE);
  ASSERT_NULL(a);
  ASSERT_EQ(vxcore_notebook_close(ctx, reopened), VXCORE_OK);
  vxcore_string_free(reopened);
  reopened = nullptr;

  // 2. Missing selected vx.json (content + parent edge intact).
  std::error_code ec;
  std::filesystem::remove(utf8_to_fs_path(folder_config_path(nb, "Alpha")), ec);

  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, reopened, "Alpha", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);

  vxcore_string_free(reopened);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_bad_metadata passed" << std::endl;
  return 0;
}

// A parent listing the same child twice is corrupt metadata, not a valid edge.
int test_share_paths_rejects_duplicate_parent_entry() {
  std::cout << "  Running test_share_paths_rejects_duplicate_parent_entry..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_dup_entry_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);

  nlohmann::json root = read_json(root_config_path(nb));
  ASSERT_TRUE(root.is_object());
  root["folders"] = nlohmann::json::array({"Alpha", "Alpha"});
  write_json(root_config_path(nb), root);

  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, reopened, "Alpha", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);

  vxcore_string_free(reopened);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_duplicate_parent_entry passed" << std::endl;
  return 0;
}

// A FILE path must not resolve: files are never listed in "folders".
int test_share_paths_rejects_file() {
  std::cout << "  Running test_share_paths_rejects_file..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_file_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, nb_id, ".", "note.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "note.md", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_file passed" << std::endl;
  return 0;
}

// An unindexed (external) physical folder with no metadata at all is rejected.
int test_share_paths_rejects_unindexed_folder() {
  std::cout << "  Running test_share_paths_rejects_unindexed_folder..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_unindexed_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  create_directory(nb + "/External");

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "External", &a, &b, &c),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(a);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_share_paths_rejects_unindexed_folder passed" << std::endl;
  return 0;
}

// The indexed folder's physical directory is a symlink / junction. is_directory
// follows it, so without an explicit symlink_status check the caller would be
// handed a path that resolves OUTSIDE the notebook entirely.
int test_share_paths_rejects_symlinked_content() {
  std::cout << "  Running test_share_paths_rejects_symlinked_content..." << std::endl;
  const std::string nb = get_test_path("test_share_paths_symlink_nb");
  const std::string outside = get_test_path("test_share_paths_symlink_outside");
  cleanup_test_dir(nb);
  cleanup_test_dir(outside);
  create_directory(outside);
  write_file(outside + "/planted.md", "planted");

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));

  // Replace the real directory with a link to an external tree.
  std::error_code ec;
  std::filesystem::remove_all(utf8_to_fs_path(nb + "/Alpha"), ec);
  std::filesystem::create_directory_symlink(utf8_to_fs_path(outside),
                                            utf8_to_fs_path(nb + "/Alpha"), ec);
  if (ec) {
    std::cout << "  ~ skipped (cannot create symlinks on this platform/user)" << std::endl;
    vxcore_string_free(nb_id);
    vxcore_context_destroy(ctx);
    cleanup_test_dir(nb);
    cleanup_test_dir(outside);
    return 0;
  }

  char *a = nullptr;
  char *b = nullptr;
  char *c = nullptr;
  ASSERT_EQ(vxcore_folder_get_share_paths(ctx, nb_id, "Alpha", &a, &b, &c),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(a);
  ASSERT_NULL(b);
  ASSERT_NULL(c);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  std::filesystem::remove(utf8_to_fs_path(nb + "/Alpha"), ec);
  cleanup_test_dir(nb);
  cleanup_test_dir(outside);
  std::cout << "  ✓ test_share_paths_rejects_symlinked_content passed" << std::endl;
  return 0;
}

int main() {  vxcore_set_test_mode(1);

  std::cout << "Running test_folder_share_paths..." << std::endl;

  RUN_TEST(test_share_paths_nested);
  RUN_TEST(test_share_paths_unicode);
  RUN_TEST(test_share_paths_rejects_invalid_paths);
  RUN_TEST(test_share_paths_rejects_raw_notebook);
  RUN_TEST(test_share_paths_rejects_orphan_selected);
  RUN_TEST(test_share_paths_rejects_nested_orphan_ancestor);
  RUN_TEST(test_share_paths_rejects_name_mismatch);
  RUN_TEST(test_share_paths_rejects_missing_content);
  RUN_TEST(test_share_paths_rejects_bad_metadata);
  RUN_TEST(test_share_paths_rejects_duplicate_parent_entry);
  RUN_TEST(test_share_paths_rejects_file);
  RUN_TEST(test_share_paths_rejects_unindexed_folder);
  RUN_TEST(test_share_paths_rejects_symlinked_content);

  std::cout << "All test_folder_share_paths tests passed!" << std::endl;
  return 0;
}
