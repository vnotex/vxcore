// test_folder_import_bundle: Tasks 2-5 of the import-shared-folder-bundle plan.
//
// Covers the three new vxcore entry points that make "Import Folder → Shared
// folder from VNote" safe:
//
//   vxcore_folder_get_import_paths     destination resolution (ROOT allowed,
//                                      unlike the share variant)
//   vxcore_notebook_collect_node_ids   the AUTHORITATIVE id oracle — an on-disk
//                                      vx.json walk, root included, folder and
//                                      file ids in ONE namespace. SQLite is a
//                                      lazily-populated, INCOMPLETE index and
//                                      therefore cannot prove an id's absence.
//   vxcore_folder_attach_imported      the journaled commit: publish, insert-only
//                                      store transaction, atomic parent vx.json
//                                      replace, one folder.created
//
// plus vxcore_notebook_recover_imports, exercised by CONSTRUCTING CRASH STATES
// BY HAND on disk (a journal at each phase with the matching half-published
// tree) — the only way to test a crash without crashing.
//
// The load-bearing property throughout: ids and timestamps survive VERBATIM,
// and any collision fails CLOSED, leaving the notebook untouched. The old
// vxcore_folder_import regenerates both and must not be confused with this.
//
// All assertions go through the PUBLIC C API plus direct on-disk edits of the
// bundled metadata layout:
//   - content:  <notebook_root>/<relative_path>
//   - metadata: <notebook_root>/vx_notebook/contents/<relative_path>/vx.json

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "test_utils.h"
#include "vxcore/vxcore.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

std::string metadata_dir(const std::string &nb, const std::string &relative) {
  return relative.empty() ? (nb + "/vx_notebook/contents")
                          : (nb + "/vx_notebook/contents/" + relative);
}

std::string folder_config_path(const std::string &nb, const std::string &relative) {
  return metadata_dir(nb, relative) + "/vx.json";
}

std::string staging_root(const std::string &nb) { return nb + "/vx_notebook/vx_import"; }

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

std::string binary_bytes(const std::string &path) {
  std::ifstream input(utf8_to_fs_path(path), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Capture the real directory identity in the journal's platform-neutral shape.
// The tests link only the public API; no private DLL exports are required.
nlohmann::json directory_identity(const std::string &path) {
  uint64_t device = 0, low = 0, high = 0, links = 0;
#ifdef _WIN32
  HANDLE handle = CreateFileW(utf8_to_fs_path(path).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return nullptr;
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO id{};
  const bool valid = GetFileInformationByHandle(handle, &info) &&
      GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
      !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
  CloseHandle(handle);
  if (!valid) return nullptr;
  device = id.VolumeSerialNumber;
  std::memcpy(&low, id.FileId.Identifier, sizeof(low));
  std::memcpy(&high, id.FileId.Identifier + sizeof(low), sizeof(high));
  links = info.nNumberOfLinks;
#else
  struct stat info{};
  if (::lstat(utf8_to_fs_path(path).c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return nullptr;
  device = static_cast<uint64_t>(info.st_dev);
  low = static_cast<uint64_t>(info.st_ino);
  links = static_cast<uint64_t>(info.st_nlink);
#endif
  return {{"device", device}, {"low", low}, {"high", high}, {"links", links}, {"directory", true}};
}

char *make_notebook(VxCoreContextHandle ctx, const std::string &nb_path,
                    VxCoreNotebookType type = VXCORE_NOTEBOOK_BUNDLED) {
  char *notebook_id = nullptr;
  if (vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Import NB\"}", type,
                             &notebook_id) != VXCORE_OK) {
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

struct ImportPaths {
  char *notebook_root = nullptr;
  char *content_root = nullptr;
  char *metadata_root = nullptr;

  ~ImportPaths() {
    vxcore_string_free(notebook_root);
    vxcore_string_free(content_root);
    vxcore_string_free(metadata_root);
  }
};

// Describes a synthetic bundle staged for attach. Timestamps and ids are fixed
// literals so "verbatim" is checkable by equality, not by "looks plausible".
struct StagedBundle {
  std::string dir;  // <nb>/vx_notebook/vx_import/<token>
  std::string root_folder_id;
  std::string child_folder_id;
  std::string file_id;
  std::string nested_file_id;
  int64_t created_utc = 1000000001;
  int64_t modified_utc = 1000000002;
};

// Builds a staged bundle equivalent to what FolderBundleImporter produces:
//   <staging>/content/{note.md,Sub/deep.md}
//   <staging>/metadata/{vx.json,Sub/vx.json}
// The metadata carries FINAL ids, tags, metadata objects and attachments.
StagedBundle stage_bundle(const std::string &nb, const std::string &token,
                          const std::string &name, const std::string &id_prefix) {
  StagedBundle bundle;
  bundle.dir = staging_root(nb) + "/" + token;
  bundle.root_folder_id = id_prefix + "-folder-root";
  bundle.child_folder_id = id_prefix + "-folder-sub";
  bundle.file_id = id_prefix + "-file-note";
  bundle.nested_file_id = id_prefix + "-file-deep";

  create_directory(bundle.dir + "/content/Sub");
  create_directory(bundle.dir + "/metadata/Sub");
  write_file(bundle.dir + "/content/note.md", "# note\n");
  write_file(bundle.dir + "/content/Sub/deep.md", "# deep\n");

  nlohmann::json file_record;
  file_record["id"] = bundle.file_id;
  file_record["name"] = "note.md";
  file_record["createdUtc"] = bundle.created_utc;
  file_record["modifiedUtc"] = bundle.modified_utc;
  file_record["metadata"] = nlohmann::json{{"origin", "bundle"}};
  file_record["tags"] = nlohmann::json::array({"research", "imported"});
  file_record["attachments"] = nlohmann::json::array({"spec.pdf"});

  nlohmann::json root_config;
  root_config["id"] = bundle.root_folder_id;
  root_config["name"] = name;
  root_config["createdUtc"] = bundle.created_utc;
  root_config["modifiedUtc"] = bundle.modified_utc;
  root_config["metadata"] = nlohmann::json{{"color", "blue"}};
  root_config["files"] = nlohmann::json::array({file_record});
  root_config["folders"] = nlohmann::json::array({"Sub"});
  write_json(bundle.dir + "/metadata/vx.json", root_config);

  nlohmann::json nested_file;
  nested_file["id"] = bundle.nested_file_id;
  nested_file["name"] = "deep.md";
  nested_file["createdUtc"] = bundle.created_utc;
  nested_file["modifiedUtc"] = bundle.modified_utc;
  nested_file["metadata"] = nlohmann::json::object();
  nested_file["tags"] = nlohmann::json::array();
  nested_file["attachments"] = nlohmann::json::array();

  nlohmann::json sub_config;
  sub_config["id"] = bundle.child_folder_id;
  sub_config["name"] = "Sub";
  sub_config["createdUtc"] = bundle.created_utc;
  sub_config["modifiedUtc"] = bundle.modified_utc;
  sub_config["metadata"] = nlohmann::json::object();
  sub_config["files"] = nlohmann::json::array({nested_file});
  sub_config["folders"] = nlohmann::json::array();
  write_json(bundle.dir + "/metadata/Sub/vx.json", sub_config);

  return bundle;
}

nlohmann::json schema2_journal(const std::string &notebook_id, const StagedBundle &bundle,
                                const std::string &before, const std::string &phase = "init") {
  auto after = nlohmann::json::parse(before);
  after["folders"].push_back("Alpha");
  after["modifiedUtc"] = 2000000000;
  return {{"schema", 2}, {"notebookId", notebook_id}, {"phase", phase},
      {"destPath", "."}, {"finalName", "Alpha"}, {"parentId", after["id"]},
      {"rootFolderId", bundle.root_folder_id},
      {"ids", {bundle.root_folder_id, bundle.child_folder_id, bundle.file_id, bundle.nested_file_id}},
      {"parentConfigBytes", before}, {"parentAfterBytes", after.dump(2)},
      {"stagingIdentity", directory_identity(bundle.dir)},
      {"contentIdentity", directory_identity(bundle.dir + "/content")},
      {"metadataIdentity", directory_identity(bundle.dir + "/metadata")}};
}

std::vector<std::string> collect_ids(VxCoreContextHandle ctx, const char *nb_id) {
  std::vector<std::string> ids;
  char *json_str = nullptr;
  if (vxcore_notebook_collect_node_ids(ctx, nb_id, &json_str) != VXCORE_OK || !json_str) {
    return ids;
  }
  try {
    const nlohmann::json json = nlohmann::json::parse(json_str);
    for (const auto &entry : json) {
      ids.push_back(entry.get<std::string>());
    }
  } catch (const std::exception &) {
  }
  vxcore_string_free(json_str);
  return ids;
}

bool contains_id(const std::vector<std::string> &ids, const std::string &id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Reads the "folders" array of a folder's vx.json straight off disk.
std::vector<std::string> disk_child_folders(const std::string &nb, const std::string &relative) {
  std::vector<std::string> names;
  const nlohmann::json json = read_json(folder_config_path(nb, relative));
  if (json.contains("folders") && json["folders"].is_array()) {
    for (const auto &entry : json["folders"]) {
      if (entry.is_string()) {
        names.push_back(entry.get<std::string>());
      }
    }
  }
  return names;
}

}  // namespace

// --- vxcore_folder_get_import_paths ----------------------------------------

// The notebook ROOT is a legal import destination; a nested folder resolves to
// the container that will hold the import. This is the deliberate difference
// from vxcore_folder_get_share_paths, which rejects the root.
int test_import_paths_root_and_nested() {
  std::cout << "  Running test_import_paths_root_and_nested..." << std::endl;
  const std::string nb = get_test_path("test_import_paths_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects"));

  // Root, spelled three ways.
  for (const char *root_spelling : {"", ".", "."}) {
    ImportPaths paths;
    ASSERT_EQ(vxcore_folder_get_import_paths(ctx, nb_id, root_spelling, &paths.notebook_root,
                                             &paths.content_root, &paths.metadata_root),
              VXCORE_OK);
    ASSERT_EQ(normalize_path(paths.notebook_root), normalize_path(nb));
    ASSERT_EQ(normalize_path(paths.content_root), normalize_path(nb));
    ASSERT_EQ(normalize_path(paths.metadata_root), normalize_path(metadata_dir(nb, "")));
  }

  {
    ImportPaths paths;
    ASSERT_EQ(vxcore_folder_get_import_paths(ctx, nb_id, "Projects", &paths.notebook_root,
                                             &paths.content_root, &paths.metadata_root),
              VXCORE_OK);
    ASSERT_EQ(normalize_path(paths.content_root), normalize_path(nb + "/Projects"));
    ASSERT_EQ(normalize_path(paths.metadata_root), normalize_path(metadata_dir(nb, "Projects")));
  }

  // Unindexed / traversing / absolute destinations are rejected, and the out
  // params stay NULL so the caller never frees garbage.
  {
    ImportPaths paths;
    ASSERT_EQ(vxcore_folder_get_import_paths(ctx, nb_id, "Nope", &paths.notebook_root,
                                             &paths.content_root, &paths.metadata_root),
              VXCORE_ERR_NOT_FOUND);
    ASSERT_NULL(paths.notebook_root);
    ASSERT_NULL(paths.content_root);
    ASSERT_NULL(paths.metadata_root);
  }
  {
    ImportPaths paths;
    ASSERT_EQ(vxcore_folder_get_import_paths(ctx, nb_id, "Projects/../Projects",
                                             &paths.notebook_root, &paths.content_root,
                                             &paths.metadata_root),
              VXCORE_ERR_INVALID_PARAM);
  }

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_import_paths_root_and_nested passed" << std::endl;
  return 0;
}

// A read-only notebook cannot be an import destination, and a raw notebook is
// structurally unsupported (it has no vx_notebook/contents/ tree at all).
int test_import_paths_rejects_readonly_and_raw() {
  std::cout << "  Running test_import_paths_rejects_readonly_and_raw..." << std::endl;
  const std::string nb = get_test_path("test_import_paths_ro_nb");
  const std::string raw_nb = get_test_path("test_import_paths_raw_nb");
  cleanup_test_dir(nb);
  cleanup_test_dir(raw_nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_EQ(vxcore_notebook_set_read_only(ctx, nb_id, 1), VXCORE_OK);
  {
    ImportPaths paths;
    ASSERT_EQ(vxcore_folder_get_import_paths(ctx, nb_id, ".", &paths.notebook_root,
                                             &paths.content_root, &paths.metadata_root),
              VXCORE_ERR_READ_ONLY);
  }

  char *raw_id = make_notebook(ctx, raw_nb, VXCORE_NOTEBOOK_RAW);
  ASSERT_NOT_NULL(raw_id);
  {
    ImportPaths paths;
    ASSERT_EQ(vxcore_folder_get_import_paths(ctx, raw_id, ".", &paths.notebook_root,
                                             &paths.content_root, &paths.metadata_root),
              VXCORE_ERR_UNSUPPORTED);
  }
  {
    char *ids_json = nullptr;
    ASSERT_EQ(vxcore_notebook_collect_node_ids(ctx, raw_id, &ids_json), VXCORE_ERR_UNSUPPORTED);
    ASSERT_NULL(ids_json);
  }

  vxcore_string_free(nb_id);
  vxcore_string_free(raw_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  cleanup_test_dir(raw_nb);
  std::cout << "  ✓ test_import_paths_rejects_readonly_and_raw passed" << std::endl;
  return 0;
}

// --- vxcore_notebook_collect_node_ids --------------------------------------

// The oracle must include the ROOT folder's own id and every FILE id, because
// vxcore_node_get_path_by_id reports the root as "not found" (its path is the
// empty string) and because a folder id colliding with a file id is just as
// destructive as a same-kind collision.
int test_collect_node_ids_is_exhaustive() {
  std::cout << "  Running test_collect_node_ids_is_exhaustive..." << std::endl;
  const std::string nb = get_test_path("test_collect_ids_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects"));
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects/Alpha"));

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, nb_id, "Projects/Alpha", "note.md", &file_id), VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  const std::vector<std::string> ids = collect_ids(ctx, nb_id);

  const std::string root_id = read_json(folder_config_path(nb, "")).value("id", std::string());
  const std::string projects_id =
      read_json(folder_config_path(nb, "Projects")).value("id", std::string());
  const std::string alpha_id =
      read_json(folder_config_path(nb, "Projects/Alpha")).value("id", std::string());

  ASSERT_FALSE(root_id.empty());
  ASSERT_TRUE(contains_id(ids, root_id));
  ASSERT_TRUE(contains_id(ids, projects_id));
  ASSERT_TRUE(contains_id(ids, alpha_id));
  ASSERT_TRUE(contains_id(ids, file_id));

  vxcore_string_free(file_id);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_collect_node_ids_is_exhaustive passed" << std::endl;
  return 0;
}

// --- vxcore_folder_attach_imported -----------------------------------------

// Happy path into the notebook ROOT: ids, timestamps, metadata, tags and
// attachments all survive VERBATIM, and they are still there after the
// notebook is CLOSED AND REOPENED (which rebuilds the store from vx.json).
int test_attach_into_root_preserves_everything() {
  std::cout << "  Running test_attach_into_root_preserves_everything..." << std::endl;
  const std::string nb = get_test_path("test_attach_root_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  const StagedBundle bundle = stage_bundle(nb, "tok1", "Alpha", "imp");

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);
  ASSERT_EQ(std::string(folder_id), bundle.root_folder_id);

  // Published on disk, staging gone (journal deleted with it).
  ASSERT_TRUE(path_exists(nb + "/Alpha/note.md"));
  ASSERT_TRUE(path_exists(nb + "/Alpha/Sub/deep.md"));
  ASSERT_TRUE(path_exists(folder_config_path(nb, "Alpha")));
  ASSERT_FALSE(path_exists(bundle.dir));

  // The parent index names the import exactly once.
  const std::vector<std::string> root_children = disk_child_folders(nb, "");
  ASSERT_EQ(std::count(root_children.begin(), root_children.end(), std::string("Alpha")),
            static_cast<ptrdiff_t>(1));

  vxcore_string_free(folder_id);
  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);

  // --- Reopen: the store is rebuilt from vx.json, so this is the real proof.
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *reopened_id = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened_id), VXCORE_OK);
  ASSERT_NOT_NULL(reopened_id);
  ASSERT_EQ(vxcore_notebook_rebuild_cache(ctx, reopened_id), VXCORE_OK);

  char *config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, reopened_id, "Alpha", &config_json), VXCORE_OK);
  const nlohmann::json config = nlohmann::json::parse(config_json);
  vxcore_string_free(config_json);

  ASSERT_EQ(config.value("id", std::string()), bundle.root_folder_id);
  ASSERT_EQ(config.value("createdUtc", int64_t(0)), bundle.created_utc);
  ASSERT_EQ(config.value("modifiedUtc", int64_t(0)), bundle.modified_utc);

  ASSERT_TRUE(config.contains("files"));
  ASSERT_EQ(config["files"].size(), static_cast<size_t>(1));
  const nlohmann::json &file = config["files"][0];
  ASSERT_EQ(file.value("id", std::string()), bundle.file_id);
  ASSERT_EQ(file.value("createdUtc", int64_t(0)), bundle.created_utc);
  ASSERT_EQ(file["tags"].size(), static_cast<size_t>(2));
  ASSERT_EQ(file["metadata"].value("origin", std::string()), "bundle");

  // Attachments survive: they are dropped by the store-record converter and
  // must be written through the dedicated attachments path instead.
  char *attachments_json = nullptr;
  ASSERT_EQ(vxcore_node_list_attachments(ctx, reopened_id, "Alpha/note.md", &attachments_json),
            VXCORE_OK);
  const nlohmann::json attachments = nlohmann::json::parse(attachments_json);
  vxcore_string_free(attachments_json);
  ASSERT_EQ(attachments.size(), static_cast<size_t>(1));
  ASSERT_EQ(attachments[0].get<std::string>(), "spec.pdf");

  // The imported ids are now part of the notebook's id namespace.
  const std::vector<std::string> ids = collect_ids(ctx, reopened_id);
  ASSERT_TRUE(contains_id(ids, bundle.root_folder_id));
  ASSERT_TRUE(contains_id(ids, bundle.child_folder_id));
  ASSERT_TRUE(contains_id(ids, bundle.file_id));
  ASSERT_TRUE(contains_id(ids, bundle.nested_file_id));

  vxcore_string_free(reopened_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_attach_into_root_preserves_everything passed" << std::endl;
  return 0;
}

// Attaching into a nested destination lands under that folder, not the root.
int test_attach_into_nested_destination() {
  std::cout << "  Running test_attach_into_nested_destination..." << std::endl;
  const std::string nb = get_test_path("test_attach_nested_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Projects"));

  const StagedBundle bundle = stage_bundle(nb, "tok2", "Alpha", "nested");

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, "Projects", "Alpha", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_OK);
  vxcore_string_free(folder_id);

  ASSERT_TRUE(path_exists(nb + "/Projects/Alpha/note.md"));
  ASSERT_TRUE(path_exists(folder_config_path(nb, "Projects/Alpha")));
  ASSERT_FALSE(path_exists(nb + "/Alpha"));

  const std::vector<std::string> children = disk_child_folders(nb, "Projects");
  ASSERT_TRUE(std::find(children.begin(), children.end(), std::string("Alpha")) != children.end());
  const std::vector<std::string> root_children = disk_child_folders(nb, "");
  ASSERT_TRUE(std::find(root_children.begin(), root_children.end(), std::string("Alpha")) ==
              root_children.end());

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_attach_into_nested_destination passed" << std::endl;
  return 0;
}

// A name already listed by the destination parent is refused, and nothing is
// published — the caller is expected to have uniquified the name first.
int test_attach_rejects_existing_name() {
  std::cout << "  Running test_attach_rejects_existing_name..." << std::endl;
  const std::string nb = get_test_path("test_attach_name_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  ASSERT_TRUE(make_folder(ctx, nb_id, "Alpha"));

  const std::string existing_id =
      read_json(folder_config_path(nb, "Alpha")).value("id", std::string());

  const StagedBundle bundle = stage_bundle(nb, "tok3", "Alpha", "clash");
  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_NULL(folder_id);

  // The pre-existing folder is untouched and the staged tree was not published.
  ASSERT_EQ(read_json(folder_config_path(nb, "Alpha")).value("id", std::string()), existing_id);
  ASSERT_FALSE(path_exists(nb + "/Alpha/note.md"));
  ASSERT_TRUE(path_exists(bundle.dir + "/content/note.md"));

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_attach_rejects_existing_name passed" << std::endl;
  return 0;
}

// Re-attaching the SAME bundle under a different name must fail on the id
// collision and leave the notebook byte-identical. This is the regression that
// insert-only store primitives exist for: with INSERT OR REPLACE the second
// attach would silently REPLACE the first import's rows and cascade-delete its
// tag associations, reporting success.
int test_attach_rejects_id_collision_without_replacing() {
  std::cout << "  Running test_attach_rejects_id_collision_without_replacing..." << std::endl;
  const std::string nb = get_test_path("test_attach_collision_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  const StagedBundle first = stage_bundle(nb, "tokA", "Alpha", "dup");
  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", first.dir.c_str(),
                                          &folder_id),
            VXCORE_OK);
  vxcore_string_free(folder_id);
  folder_id = nullptr;

  // Same ids, different name.
  const StagedBundle second = stage_bundle(nb, "tokB", "Alpha (2)", "dup");
  const VxCoreError err = vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha (2)",
                                                        second.dir.c_str(), &folder_id);
  ASSERT_NE(err, VXCORE_OK);
  ASSERT_NULL(folder_id);

  // Nothing of the second attach survives, and the first import is intact.
  ASSERT_FALSE(path_exists(nb + "/Alpha (2)"));
  ASSERT_FALSE(path_exists(metadata_dir(nb, "Alpha (2)")));
  const std::vector<std::string> children = disk_child_folders(nb, "");
  ASSERT_TRUE(std::find(children.begin(), children.end(), std::string("Alpha (2)")) ==
              children.end());
  ASSERT_EQ(read_json(folder_config_path(nb, "Alpha")).value("id", std::string()),
            first.root_folder_id);
  ASSERT_TRUE(path_exists(nb + "/Alpha/note.md"));

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_attach_rejects_id_collision_without_replacing passed" << std::endl;
  return 0;
}

// A staged FOLDER id equal to an existing FILE id must be rejected: folder and
// file ids share one namespace as far as import is concerned, even though the
// SQLite `uuid UNIQUE` constraint is per-table and would happily allow it.
int test_attach_rejects_cross_kind_id_collision() {
  std::cout << "  Running test_attach_rejects_cross_kind_id_collision..." << std::endl;
  const std::string nb = get_test_path("test_attach_crosskind_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  char *existing_file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, nb_id, ".", "existing.md", &existing_file_id), VXCORE_OK);
  ASSERT_NOT_NULL(existing_file_id);
  const std::string reused_id(existing_file_id);
  vxcore_string_free(existing_file_id);

  // The oracle reports the file id, so the caller can refuse before staging.
  const std::vector<std::string> ids = collect_ids(ctx, nb_id);
  ASSERT_TRUE(contains_id(ids, reused_id));

  // Stage a bundle whose ROOT FOLDER id is that file id.
  StagedBundle bundle = stage_bundle(nb, "tokX", "Alpha", "cross");
  nlohmann::json root_config = read_json(bundle.dir + "/metadata/vx.json");
  root_config["id"] = reused_id;
  write_json(bundle.dir + "/metadata/vx.json", root_config);

  char *folder_id = nullptr;
  const VxCoreError err =
      vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", bundle.dir.c_str(), &folder_id);
  ASSERT_NE(err, VXCORE_OK);
  ASSERT_NULL(folder_id);

  // The existing file keeps its id and its row was not replaced.
  char *path = nullptr;
  ASSERT_EQ(vxcore_node_get_path_by_id(ctx, nb_id, reused_id.c_str(), &path), VXCORE_OK);
  ASSERT_EQ(std::string(path), "existing.md");
  vxcore_string_free(path);
  ASSERT_FALSE(path_exists(nb + "/Alpha"));

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_attach_rejects_cross_kind_id_collision passed" << std::endl;
  return 0;
}

// Read-only and raw notebooks refuse the attach outright.
int test_attach_rejects_readonly_and_raw() {
  std::cout << "  Running test_attach_rejects_readonly_and_raw..." << std::endl;
  const std::string nb = get_test_path("test_attach_ro_nb");
  const std::string raw_nb = get_test_path("test_attach_raw_nb");
  cleanup_test_dir(nb);
  cleanup_test_dir(raw_nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);
  const StagedBundle bundle = stage_bundle(nb, "tokRO", "Alpha", "ro");
  ASSERT_EQ(vxcore_notebook_set_read_only(ctx, nb_id, 1), VXCORE_OK);

  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(folder_id);
  ASSERT_FALSE(path_exists(nb + "/Alpha"));

  char *raw_id = make_notebook(ctx, raw_nb, VXCORE_NOTEBOOK_RAW);
  ASSERT_NOT_NULL(raw_id);
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, raw_id, ".", "Alpha", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(folder_id);

  vxcore_string_free(nb_id);
  vxcore_string_free(raw_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  cleanup_test_dir(raw_nb);
  std::cout << "  ✓ test_attach_rejects_readonly_and_raw passed" << std::endl;
  return 0;
}

// An incomplete staging directory (no metadata/, or a metadata root whose
// "name" disagrees with the requested name) is refused before any publish.
int test_attach_rejects_malformed_staging() {
  std::cout << "  Running test_attach_rejects_malformed_staging..." << std::endl;
  const std::string nb = get_test_path("test_attach_malformed_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  // Content only, no metadata subtree.
  const std::string bare = staging_root(nb) + "/bare";
  create_directory(bare + "/content");
  write_file(bare + "/content/note.md", "x");
  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", bare.c_str(), &folder_id),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(folder_id);

  // Metadata root name disagrees with the requested name: the top-level
  // vx.json "name" MUST equal the published directory name.
  const StagedBundle bundle = stage_bundle(nb, "tokM", "Alpha", "mal");
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Beta", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_ERR_INVALID_STATE);
  ASSERT_NULL(folder_id);

  // An unsafe name never reaches the filesystem.
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "../escape", bundle.dir.c_str(),
                                          &folder_id),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(folder_id);
  ASSERT_FALSE(path_exists(nb + "/Alpha"));

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_attach_rejects_malformed_staging passed" << std::endl;
  return 0;
}

// --- Crash recovery ---------------------------------------------------------

namespace {

// Reproduces the on-disk state a crash would leave at @phase, by hand:
// the journal plus exactly the artifacts the protocol had published by then.
// Returns the notebook path.
void plant_crash_state(const std::string &nb, const std::string &phase,
                       const std::string &name, const std::string &dest_rel,
                       const std::vector<std::string> &ids,
                       const std::string &parent_bytes) {
  const std::string dir = staging_root(nb) + "/crash";
  create_directory(dir);

  const std::string content_target = dest_rel.empty() ? (nb + "/" + name)
                                                      : (nb + "/" + dest_rel + "/" + name);
  const std::string metadata_target = metadata_dir(nb, dest_rel.empty() ? name
                                                                       : dest_rel + "/" + name);

  nlohmann::json journal;
  journal["phase"] = phase;
  journal["destPath"] = dest_rel.empty() ? "." : dest_rel;
  journal["finalName"] = name;
  journal["contentTarget"] = content_target;
  journal["metadataTarget"] = metadata_target;
  journal["rootFolderId"] = ids.empty() ? std::string() : ids.front();
  journal["ids"] = ids;
  journal["parentConfigBytes"] = parent_bytes;
  write_json(dir + "/" + "journal.json", journal);

  // Everything at or before the recorded phase is already published.
  if (phase != "init") {
    create_directory(content_target);
    write_file(content_target + "/note.md", "# note\n");
  }
  if (phase == "metadata" || phase == "db" || phase == "committed") {
    create_directory(metadata_target);
    nlohmann::json config;
    config["id"] = ids.empty() ? std::string("crash-folder") : ids.front();
    config["name"] = name;
    config["createdUtc"] = 1;
    config["modifiedUtc"] = 1;
    config["metadata"] = nlohmann::json::object();
    config["files"] = nlohmann::json::array();
    config["folders"] = nlohmann::json::array();
    write_json(metadata_target + "/vx.json", config);
  }
}

}  // namespace

// Schema2 rollback requires exact parent before-bytes and recorded directory
// identities. It removes owned private trees without rewriting the parent.
int test_recovery_rolls_back_uncommitted_phases() {
  for (const std::string phase : {"init", "content", "metadata", "db"}) {
    const std::string nb = get_test_path("test_recover_rb_nb_" + phase);
    cleanup_test_dir(nb);
    VxCoreContextHandle ctx = nullptr;
    ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
    char *nb_id = make_notebook(ctx, nb);
    ASSERT_NOT_NULL(nb_id);
    std::string before;
    for (const char ch : binary_bytes(folder_config_path(nb, ""))) {
      if (ch == '\n') before += '\r';
      before += ch;
    }
    write_file(folder_config_path(nb, ""), before);
    const auto bundle = stage_bundle(nb, "crash", "Alpha", "rollback-" + phase);
    auto journal = schema2_journal(nb_id, bundle, before, phase);
    ASSERT_TRUE(journal["stagingIdentity"].is_object());
    if (phase == "db") {
      // Seed real committed derived rows, then reproduce the state just before
      // parent publication. Recovery must actually remove those ghost rows.
      char *id = nullptr;
      ASSERT_EQ(vxcore_folder_attach_imported(ctx, nb_id, ".", "Alpha", bundle.dir.c_str(), &id), VXCORE_OK);
      vxcore_string_free(id);
      write_file(folder_config_path(nb, ""), before);
      create_directory(bundle.dir);
      journal["stagingIdentity"] = directory_identity(bundle.dir);
    } else {
      if (phase != "init")
        std::filesystem::rename(utf8_to_fs_path(bundle.dir + "/content"), utf8_to_fs_path(nb + "/Alpha"));
      if (phase == "metadata")
        std::filesystem::rename(utf8_to_fs_path(bundle.dir + "/metadata"), utf8_to_fs_path(metadata_dir(nb, "Alpha")));
    }
    write_json(bundle.dir + "/journal.json", journal);
    ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);
    vxcore_string_free(nb_id);
    vxcore_context_destroy(ctx);
    ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
    char *reopened = nullptr;
    ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);
    ASSERT_FALSE(path_exists(bundle.dir));
    ASSERT_FALSE(path_exists(nb + "/Alpha"));
    ASSERT_FALSE(path_exists(metadata_dir(nb, "Alpha")));
    ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), before);
    char *resolved = nullptr;
    ASSERT_NE(vxcore_node_get_path_by_id(ctx, reopened, bundle.file_id.c_str(), &resolved), VXCORE_OK);
    vxcore_string_free(resolved);
    ASSERT_EQ(vxcore_notebook_close(ctx, reopened), VXCORE_OK);
    vxcore_string_free(reopened);
    vxcore_context_destroy(ctx);
    cleanup_test_dir(nb);
  }
  return 0;
}

// A crash AFTER the commit point rolls FORWARD: the parent vx.json already
// names the import, so the published tree is kept and only the journal is
// cleaned up. Rolling back here would destroy a successful import.
int test_recovery_rolls_forward_committed_phase() {
  std::cout << "  Running test_recovery_rolls_forward_committed_phase..." << std::endl;
  const std::string nb = get_test_path("test_recover_ff_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  const std::string parent_bytes = [&] {
    std::ifstream file(utf8_to_fs_path(folder_config_path(nb, "")), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  }();

  ASSERT_EQ(vxcore_notebook_close(ctx, nb_id), VXCORE_OK);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);

  plant_crash_state(nb, "committed", "Alpha", "", {"crash-folder-id"}, parent_bytes);

  // The commit point HAD been reached, so the parent index names the import.
  nlohmann::json root_config = read_json(folder_config_path(nb, ""));
  root_config["folders"] = nlohmann::json::array({"Alpha"});
  write_json(folder_config_path(nb, ""), root_config);

  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);

  ASSERT_FALSE(path_exists(staging_root(nb) + "/crash"));
  ASSERT_TRUE(path_exists(nb + "/Alpha/note.md"));
  ASSERT_TRUE(path_exists(folder_config_path(nb, "Alpha")));

  const std::vector<std::string> children = disk_child_folders(nb, "");
  ASSERT_TRUE(std::find(children.begin(), children.end(), std::string("Alpha")) !=
              children.end());

  char *resolved = nullptr;
  ASSERT_EQ(vxcore_node_get_path_by_id(ctx, reopened, "crash-folder-id", &resolved), VXCORE_OK);
  ASSERT_EQ(std::string(resolved), "Alpha");
  vxcore_string_free(resolved);

  // The rolled-forward folder is reachable through the normal API.
  char *config_json = nullptr;
  ASSERT_EQ(vxcore_node_get_config(ctx, reopened, "Alpha", &config_json), VXCORE_OK);
  vxcore_string_free(config_json);

  vxcore_string_free(reopened);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_recovery_rolls_forward_committed_phase passed" << std::endl;
  return 0;
}

// Calling recovery explicitly on a clean notebook is a no-op.
int test_recovery_is_noop_without_journals() {
  std::cout << "  Running test_recovery_is_noop_without_journals..." << std::endl;
  const std::string nb = get_test_path("test_recover_noop_nb");
  cleanup_test_dir(nb);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *nb_id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(nb_id);

  int recovered = -1;
  ASSERT_EQ(vxcore_notebook_recover_imports(ctx, nb_id, &recovered), VXCORE_OK);
  ASSERT_EQ(recovered, 0);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  std::cout << "  ✓ test_recovery_is_noop_without_journals passed" << std::endl;
  return 0;
}


int test_recovery_keeps_visible_cohort_despite_stale_phase() {
  const std::string nb = get_test_path("test_recover_visible_stale_phase");
  cleanup_test_dir(nb);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *notebook = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(notebook);
  const auto binary = [](const std::string &path) {
    std::ifstream input(utf8_to_fs_path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto before_parent = binary(folder_config_path(nb, ""));
  const auto staged = stage_bundle(nb, "visible", "Alpha", "visible-stale");
  char *folder = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, notebook, ".", "Alpha", staged.dir.c_str(), &folder), VXCORE_OK);
  vxcore_string_free(folder);
  ASSERT_EQ(vxcore_notebook_close(ctx, notebook), VXCORE_OK);
  vxcore_string_free(notebook);
  vxcore_context_destroy(ctx);
  auto parent = read_json(folder_config_path(nb, ""));
  parent["metadata"]["laterEdit"] = "preserve this newer metadata";
  write_json(folder_config_path(nb, ""), parent);
  const auto visible_parent = binary(folder_config_path(nb, ""));
  const auto note_bytes = binary(nb + "/Alpha/note.md");
  const auto journal_dir = staging_root(nb) + "/late-receipt";
  create_directory(journal_dir);
  write_json(journal_dir + "/journal.json", {
      {"phase", "db"}, {"destPath", "."}, {"finalName", "Alpha"},
      {"contentTarget", nb + "/Alpha"}, {"metadataTarget", metadata_dir(nb, "Alpha")},
      {"rootFolderId", staged.root_folder_id},
      {"ids", {staged.root_folder_id, staged.child_folder_id, staged.file_id, staged.nested_file_id}},
      {"parentConfigBytes", before_parent}});
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *reopened = nullptr;
  const auto open = vxcore_notebook_open(ctx, nb.c_str(), &reopened);
  const bool preserved = path_exists(nb + "/Alpha/note.md") &&
      binary(nb + "/Alpha/note.md") == note_bytes &&
      binary(folder_config_path(nb, "")) == visible_parent;
  std::cout << "  Stale-phase visible cohort: open=" << open << ", preserved=" << preserved << std::endl;
  vxcore_string_free(reopened);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  ASSERT_EQ(open, VXCORE_OK);
  ASSERT_TRUE(preserved);
  return 0;
}

int test_recovery_preserves_unprovable_journals() {
  for (const std::string mode : {"malformed", "unreadable", "foreign", "legacy", "parent-edit", "identity"}) {
    const auto nb = get_test_path("test_recovery_preserve_" + mode);
    const auto outside = get_test_path("test_recovery_outside_" + mode);
    cleanup_test_dir(nb);
    cleanup_test_dir(outside);
    VxCoreContextHandle ctx = nullptr;
    ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
    char *id = make_notebook(ctx, nb);
    ASSERT_NOT_NULL(id);
    const auto before = binary_bytes(folder_config_path(nb, ""));
    const auto bundle = stage_bundle(nb, "crash", "Alpha", "unprovable-" + mode);
    auto journal = schema2_journal(id, bundle, before);
    if (mode == "foreign" || mode == "legacy") {
      journal = {{"phase", "db"}, {"destPath", "."}, {"finalName", "Alpha"},
          {"contentTarget", mode == "foreign" ? outside : nb + "/Alpha"},
          {"metadataTarget", metadata_dir(nb, "Alpha")}, {"rootFolderId", bundle.root_folder_id},
          {"ids", {bundle.root_folder_id, bundle.child_folder_id, bundle.file_id, bundle.nested_file_id}},
          {"parentConfigBytes", before}};
      create_directory(outside);
      write_file(outside + "/unrelated.bin", std::string("foreign\0bytes", 13));
    }
    std::filesystem::rename(utf8_to_fs_path(bundle.dir + "/content"), utf8_to_fs_path(nb + "/Alpha"));
    std::filesystem::rename(utf8_to_fs_path(bundle.dir + "/metadata"), utf8_to_fs_path(metadata_dir(nb, "Alpha")));
    if (mode == "parent-edit") {
      auto parent = read_json(folder_config_path(nb, ""));
      parent["metadata"]["newer"] = "do not clobber";
      write_json(folder_config_path(nb, ""), parent);
    }
    if (mode == "identity") {
      std::filesystem::rename(utf8_to_fs_path(nb + "/Alpha"), utf8_to_fs_path(nb + "/original-owned"));
      create_directory(nb + "/Alpha");
      write_file(nb + "/Alpha/note.md", "foreign replacement");
    }
    const auto parent_bytes = binary_bytes(folder_config_path(nb, ""));
    const auto note_bytes = binary_bytes(nb + "/Alpha/note.md");
    if (mode == "unreadable") create_directory(bundle.dir + "/journal.json");
    else if (mode == "malformed") write_file(bundle.dir + "/journal.json", "not JSON\r\n");
    else write_json(bundle.dir + "/journal.json", journal);
    const auto journal_bytes = mode == "unreadable" ? std::string() : binary_bytes(bundle.dir + "/journal.json");
    int recovered = -1;
    ASSERT_NE(vxcore_notebook_recover_imports(ctx, id, &recovered), VXCORE_OK);
    ASSERT_EQ(recovered, 0);
    ASSERT_TRUE(path_exists(bundle.dir + "/journal.json"));
    if (mode != "unreadable") ASSERT_EQ(binary_bytes(bundle.dir + "/journal.json"), journal_bytes);
    ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), parent_bytes);
    ASSERT_EQ(binary_bytes(nb + "/Alpha/note.md"), note_bytes);
    ASSERT_TRUE(path_exists(folder_config_path(nb, "Alpha")));
    if (mode == "foreign") ASSERT_EQ(binary_bytes(outside + "/unrelated.bin"), std::string("foreign\0bytes", 13));
    ASSERT_EQ(vxcore_notebook_close(ctx, id), VXCORE_OK);
    vxcore_string_free(id);
    vxcore_context_destroy(ctx);
    cleanup_test_dir(nb);
    cleanup_test_dir(outside);
  }
  return 0;
}

int test_schema2_stale_phase_survives_rename_and_descendant_edits() {
  const auto nb = get_test_path("test_schema2_later_edits");
  cleanup_test_dir(nb);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(id);
  const auto before = binary_bytes(folder_config_path(nb, ""));
  const auto bundle = stage_bundle(nb, "receipt", "Alpha", "later-edits");
  auto journal = schema2_journal(id, bundle, before, "db");
  char *folder = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, id, ".", "Alpha", bundle.dir.c_str(), &folder), VXCORE_OK);
  ASSERT_EQ(std::string(folder), bundle.root_folder_id);
  vxcore_string_free(folder);
  ASSERT_EQ(vxcore_node_rename(ctx, id, "Alpha", "Renamed"), VXCORE_OK);
  ASSERT_EQ(vxcore_node_delete(ctx, id, "Renamed/Sub/deep.md"), VXCORE_OK);
  ASSERT_EQ(vxcore_node_update_metadata(ctx, id, "Renamed", "{\"later\":\"keep\"}"), VXCORE_OK);
  char *new_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, id, "Renamed", "later.md", &new_id), VXCORE_OK);
  write_file(nb + "/Renamed/later.md", std::string("new\0body\r\n", 10));
  const auto parent = binary_bytes(folder_config_path(nb, ""));
  const auto renamed = binary_bytes(folder_config_path(nb, "Renamed"));
  create_directory(bundle.dir);
  journal["stagingIdentity"] = directory_identity(bundle.dir);
  write_json(bundle.dir + "/journal.json", journal);
  int recovered = 0;
  ASSERT_EQ(vxcore_notebook_recover_imports(ctx, id, &recovered), VXCORE_OK);
  ASSERT_EQ(recovered, 1);
  ASSERT_FALSE(path_exists(bundle.dir));
  ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), parent);
  ASSERT_EQ(binary_bytes(folder_config_path(nb, "Renamed")), renamed);
  ASSERT_EQ(binary_bytes(nb + "/Renamed/note.md"), "# note\n");
  ASSERT_EQ(binary_bytes(nb + "/Renamed/later.md"), std::string("new\0body\r\n", 10));
  char *resolved = nullptr;
  ASSERT_EQ(vxcore_node_get_path_by_id(ctx, id, bundle.root_folder_id.c_str(), &resolved), VXCORE_OK);
  ASSERT_EQ(std::string(resolved), "Renamed");
  vxcore_string_free(resolved);
  resolved = nullptr;
  ASSERT_NE(vxcore_node_get_path_by_id(ctx, id, bundle.nested_file_id.c_str(), &resolved), VXCORE_OK);
  vxcore_string_free(resolved);
  resolved = nullptr;
  ASSERT_EQ(vxcore_node_get_path_by_id(ctx, id, new_id, &resolved), VXCORE_OK);
  ASSERT_EQ(std::string(resolved), "Renamed/later.md");
  vxcore_string_free(resolved);
  vxcore_string_free(new_id);
  ASSERT_EQ(vxcore_notebook_close(ctx, id), VXCORE_OK);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  return 0;
}

int test_schema2_committed_receipt_survives_delete_and_cleanup_lock() {
  const auto nb = get_test_path("test_schema2_committed_residue");
  cleanup_test_dir(nb);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(id);
  const auto before = binary_bytes(folder_config_path(nb, ""));
  const auto bundle = stage_bundle(nb, "receipt", "Alpha", "committed-residue");
  auto journal = schema2_journal(id, bundle, before, "committed");
  char *folder = nullptr;
  ASSERT_EQ(vxcore_folder_attach_imported(ctx, id, ".", "Alpha", bundle.dir.c_str(), &folder), VXCORE_OK);
  vxcore_string_free(folder);
  create_directory(bundle.dir);
  journal["stagingIdentity"] = directory_identity(bundle.dir);
  write_json(bundle.dir + "/journal.json", journal);
#ifdef _WIN32
  HANDLE locked = CreateFileW(utf8_to_fs_path(bundle.dir + "/journal.json").c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_TRUE(locked != INVALID_HANDLE_VALUE);
  int recovered = 0;
  const auto locked_recovery = vxcore_notebook_recover_imports(ctx, id, &recovered);
  const bool retained = path_exists(bundle.dir + "/journal.json");
  CloseHandle(locked);
  ASSERT_EQ(locked_recovery, VXCORE_OK);
  ASSERT_EQ(recovered, 1);
  ASSERT_TRUE(retained);
#endif
  ASSERT_EQ(vxcore_node_delete(ctx, id, "Alpha"), VXCORE_OK);
  const auto deleted_parent = binary_bytes(folder_config_path(nb, ""));
  ASSERT_EQ(vxcore_notebook_close(ctx, id), VXCORE_OK);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *reopened = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb.c_str(), &reopened), VXCORE_OK);
  ASSERT_FALSE(path_exists(bundle.dir));
  ASSERT_FALSE(path_exists(nb + "/Alpha"));
  ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), deleted_parent);
  char *resolved = nullptr;
  ASSERT_NE(vxcore_node_get_path_by_id(ctx, reopened, bundle.root_folder_id.c_str(), &resolved), VXCORE_OK);
  vxcore_string_free(resolved);
  ASSERT_EQ(vxcore_notebook_close(ctx, reopened), VXCORE_OK);
  vxcore_string_free(reopened);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  return 0;
}

int test_schema2_uncommitted_cleanup_failure_retains_journal() {
#ifdef _WIN32
  const auto nb = get_test_path("test_schema2_rollback_lock");
  cleanup_test_dir(nb);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(id);
  const auto before = binary_bytes(folder_config_path(nb, ""));
  const auto bundle = stage_bundle(nb, "receipt", "Alpha", "rollback-lock");
  const auto journal = schema2_journal(id, bundle, before, "metadata");
  std::filesystem::rename(utf8_to_fs_path(bundle.dir + "/content"), utf8_to_fs_path(nb + "/Alpha"));
  std::filesystem::rename(utf8_to_fs_path(bundle.dir + "/metadata"), utf8_to_fs_path(metadata_dir(nb, "Alpha")));
  write_json(bundle.dir + "/journal.json", journal);
  HANDLE locked = CreateFileW(utf8_to_fs_path(nb + "/Alpha/note.md").c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_TRUE(locked != INVALID_HANDLE_VALUE);
  int recovered = 0;
  const auto error = vxcore_notebook_recover_imports(ctx, id, &recovered);
  const bool retained = path_exists(bundle.dir + "/journal.json");
  CloseHandle(locked);
  ASSERT_NE(error, VXCORE_OK);
  ASSERT_EQ(recovered, 0);
  ASSERT_TRUE(retained);
  ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), before);
  ASSERT_EQ(vxcore_notebook_recover_imports(ctx, id, &recovered), VXCORE_OK);
  ASSERT_FALSE(path_exists(bundle.dir));
  ASSERT_FALSE(path_exists(nb + "/Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, id), VXCORE_OK);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
#endif
  return 0;
}

int test_attach_rejects_noncontained_staging() {
  const auto nb = get_test_path("test_attach_contained_nb");
  const auto outside = get_test_path("test_attach_contained_outside");
  cleanup_test_dir(nb);
  cleanup_test_dir(outside);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(id);
  const auto bundle = stage_bundle(nb, "escape", "Alpha", "outside");
  std::filesystem::rename(utf8_to_fs_path(bundle.dir), utf8_to_fs_path(outside));
  const auto parent = binary_bytes(folder_config_path(nb, ""));
  char *folder = nullptr;
  ASSERT_NE(vxcore_folder_attach_imported(ctx, id, ".", "Alpha", outside.c_str(), &folder), VXCORE_OK);
  ASSERT_NULL(folder);
  ASSERT_TRUE(path_exists(outside + "/content/note.md"));
  ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), parent);
  ASSERT_FALSE(path_exists(nb + "/Alpha"));
  ASSERT_EQ(vxcore_notebook_close(ctx, id), VXCORE_OK);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
  cleanup_test_dir(outside);
  return 0;
}

int test_attach_reports_failed_committed_marker_without_rollback() {
#ifdef _WIN32
  const auto nb = get_test_path("test_attach_marker_failure");
  cleanup_test_dir(nb);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *id = make_notebook(ctx, nb);
  ASSERT_NOT_NULL(id);
  const auto bundle = stage_bundle(nb, "receipt", "Alpha", "marker-failure");
  struct Lock {
    std::string path;
    HANDLE handle = INVALID_HANDLE_VALUE;
  } lock{bundle.dir + "/journal.json"};
  const auto callback = +[](const char *, const char *, void *data) {
    auto &lock = *static_cast<Lock *>(data);
    if (lock.handle == INVALID_HANDLE_VALUE)
      lock.handle = CreateFileW(utf8_to_fs_path(lock.path).c_str(), GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  };
  ASSERT_EQ(vxcore_on_event(ctx, "folder.config_changed", callback, &lock), VXCORE_OK);
  char *folder = nullptr;
  const auto attach = vxcore_folder_attach_imported(ctx, id, ".", "Alpha", bundle.dir.c_str(), &folder);
  const bool locked = lock.handle != INVALID_HANDLE_VALUE;
  if (locked) CloseHandle(lock.handle);
  ASSERT_EQ(vxcore_off_event(ctx, "folder.config_changed", callback), VXCORE_OK);
  ASSERT_TRUE(locked);
  ASSERT_NE(attach, VXCORE_OK);
  ASSERT_NULL(folder);
  ASSERT_TRUE(path_exists(bundle.dir + "/journal.json"));
  ASSERT_EQ(binary_bytes(nb + "/Alpha/note.md"), "# note\n");
  ASSERT_EQ(read_json(folder_config_path(nb, "Alpha"))["id"], bundle.root_folder_id);
  ASSERT_EQ(vxcore_node_rename(ctx, id, "Alpha", "Renamed"), VXCORE_OK);
  ASSERT_EQ(vxcore_node_update_metadata(ctx, id, "Renamed", "{\"editedAfterFailure\":true}"), VXCORE_OK);
  const auto parent = binary_bytes(folder_config_path(nb, ""));
  int recovered = 0;
  ASSERT_EQ(vxcore_notebook_recover_imports(ctx, id, &recovered), VXCORE_OK);
  ASSERT_EQ(recovered, 1);
  ASSERT_FALSE(path_exists(bundle.dir));
  ASSERT_EQ(binary_bytes(folder_config_path(nb, "")), parent);
  ASSERT_EQ(binary_bytes(nb + "/Renamed/note.md"), "# note\n");
  ASSERT_EQ(read_json(folder_config_path(nb, "Renamed"))["id"], bundle.root_folder_id);
  ASSERT_EQ(vxcore_notebook_close(ctx, id), VXCORE_OK);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb);
#endif
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running folder bundle import tests..." << std::endl;

  RUN_TEST(test_recovery_keeps_visible_cohort_despite_stale_phase);
  RUN_TEST(test_attach_reports_failed_committed_marker_without_rollback);
  RUN_TEST(test_recovery_preserves_unprovable_journals);
  RUN_TEST(test_schema2_stale_phase_survives_rename_and_descendant_edits);
  RUN_TEST(test_schema2_committed_receipt_survives_delete_and_cleanup_lock);
  RUN_TEST(test_schema2_uncommitted_cleanup_failure_retains_journal);
  RUN_TEST(test_attach_rejects_noncontained_staging);
  RUN_TEST(test_import_paths_root_and_nested);
  RUN_TEST(test_import_paths_rejects_readonly_and_raw);
  RUN_TEST(test_collect_node_ids_is_exhaustive);
  RUN_TEST(test_attach_into_root_preserves_everything);
  RUN_TEST(test_attach_into_nested_destination);
  RUN_TEST(test_attach_rejects_existing_name);
  RUN_TEST(test_attach_rejects_id_collision_without_replacing);
  RUN_TEST(test_attach_rejects_cross_kind_id_collision);
  RUN_TEST(test_attach_rejects_readonly_and_raw);
  RUN_TEST(test_attach_rejects_malformed_staging);
  RUN_TEST(test_recovery_rolls_back_uncommitted_phases);
  RUN_TEST(test_recovery_rolls_forward_committed_phase);
  RUN_TEST(test_recovery_is_noop_without_journals);

  std::cout << "All folder bundle import tests passed!" << std::endl;
  return 0;
}
