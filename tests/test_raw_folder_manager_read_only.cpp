// T10 of open-notebook-remote-readonly plan.
//
// Verifies that every public mutating method on RawFolderManager honors the
// per-device read-only flag set via vxcore_notebook_set_read_only.
//
// Mirrors test_bundled_folder_manager_read_only (T9, test_folder.cpp) but
// targets raw notebooks. Three subtests:
//
//   1. Standard mutators (CreateFolder, CreateFile, DeleteFolder,
//      RenameFolder, MoveFolder, CopyFolder, UpdateFolderMetadata,
//      UpdateNodeTimestamps, DeleteFile, RenameFile, MoveFile, CopyFile,
//      UpdateFileMetadata, ImportFile, ImportFolder) — assert each returns
//      VXCORE_ERR_READ_ONLY when the notebook is RO and the on-disk state is
//      unchanged afterwards.
//
//   2. UNSUPPORTED-priority mutators (TagFile, UntagFile, UpdateFileTags,
//      UpdateFileAttachments, AddFileAttachment, DeleteFileAttachment) —
//      these return VXCORE_ERR_UNSUPPORTED for writable raw notebooks. When
//      the notebook is RO they MUST return VXCORE_ERR_READ_ONLY instead
//      (read-only is the higher-priority status). Guard ordering: IsReadOnly
//      check first, UNSUPPORTED early-return second.
//
//   3. Happy path — after flipping read-only back to false, every standard
//      mutator from subtest 1 succeeds again on the same writable raw
//      notebook. Confirms the guards do not leak any persistent state.
//
// Raw notebooks can only be created via vxcore_notebook_create(...,
// VXCORE_NOTEBOOK_RAW, ...); there is no public "open raw notebook" path
// (T10 plan OUT list). Read-only is toggled via the C ABI
// vxcore_notebook_set_read_only added in T3.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

// Captures the on-disk state of |dir| as a sorted, newline-joined list of
// "<relative_path>|<size_or_DIR>" entries. Used to assert that read-only
// rejections do not leave any filesystem side effects.
std::string snapshot_dir(const std::string &dir) {
  namespace fs = std::filesystem;
  std::vector<std::string> entries;
  std::error_code ec;
  auto base = utf8_to_fs_path(dir);
  if (!fs::exists(base, ec)) {
    return "<missing>";
  }
  for (auto it = fs::recursive_directory_iterator(
           base, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    fs::path rel = fs::relative(it->path(), base, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    std::string rel_str = fs_path_to_utf8(rel);
    std::replace(rel_str.begin(), rel_str.end(), '\\', '/');
    if (it->is_directory(ec)) {
      entries.push_back(rel_str + "|DIR");
    } else if (it->is_regular_file(ec)) {
      auto sz = fs::file_size(it->path(), ec);
      entries.push_back(rel_str + "|" + std::to_string(static_cast<uint64_t>(sz)));
    }
  }
  std::sort(entries.begin(), entries.end());
  std::string joined;
  for (const auto &e : entries) {
    joined += e;
    joined += "\n";
  }
  return joined;
}

void write_text_file(const std::string &path, const std::string &content) {
  std::ofstream f(utf8_to_fs_path(path), std::ios::binary);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

// Subtest 1: structural and metadata mutators reject when RO and leave disk
// untouched.
int test_raw_mutators_rejected_when_read_only() {
  std::cout << "  Running test_raw_mutators_rejected_when_read_only..." << std::endl;
  const std::string nb_root = get_test_path("test_raw_ro_mutators_nb");
  cleanup_test_dir(nb_root);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, nb_root.c_str(),
                               "{\"name\":\"Raw RO Mutators\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Seed: a folder, a file, and a destination folder for move/copy.
  char *seed_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "seed_folder", &seed_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(seed_folder_id);

  char *dest_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &dest_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(dest_folder_id);

  char *seed_file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "seed.md", &seed_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(seed_file_id);

  // Prepare external sources for ImportFile / ImportFolder rejections.
  const std::string external_root = get_test_path("test_raw_ro_external_src");
  cleanup_test_dir(external_root);
  create_directory(external_root);
  const std::string external_file = external_root + "/external.md";
  write_text_file(external_file, "external content\n");

  const std::string external_subdir = external_root + "/external_folder";
  create_directory(external_subdir);
  write_text_file(external_subdir + "/nested.md", "nested external\n");

  // Capture baseline state AFTER all writable seeding completes.
  const std::string baseline = snapshot_dir(nb_root);
  ASSERT(!baseline.empty());

  // Lock the notebook.
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);

  // --- CreateFolder ---
  char *ro_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "new_folder_ro", &ro_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(ro_folder_id);

  // --- CreateFile ---
  char *ro_file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "new_file_ro.md", &ro_file_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(ro_file_id);

  // --- DeleteFolder (via unified node_delete) ---
  err = vxcore_node_delete(ctx, notebook_id, "seed_folder");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- DeleteFile (via unified node_delete) ---
  err = vxcore_node_delete(ctx, notebook_id, "seed.md");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- RenameFolder ---
  err = vxcore_node_rename(ctx, notebook_id, "seed_folder", "renamed_folder");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- RenameFile ---
  err = vxcore_node_rename(ctx, notebook_id, "seed.md", "renamed.md");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- MoveFolder ---
  err = vxcore_node_move(ctx, notebook_id, "seed_folder", "dest_folder");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- MoveFile ---
  err = vxcore_node_move(ctx, notebook_id, "seed.md", "dest_folder");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- CopyFolder ---
  char *copy_folder_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "seed_folder", "dest_folder", "copy", &copy_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(copy_folder_id);

  // --- CopyFile ---
  char *copy_file_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "seed.md", "dest_folder", "copy.md", &copy_file_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(copy_file_id);

  // --- UpdateFolderMetadata (via unified node_update_metadata) ---
  err = vxcore_node_update_metadata(ctx, notebook_id, "seed_folder", R"({"k":"v"})");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- UpdateFileMetadata (via unified node_update_metadata) ---
  err = vxcore_node_update_metadata(ctx, notebook_id, "seed.md", R"({"k":"v"})");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- UpdateNodeTimestamps ---
  err = vxcore_node_update_timestamps(ctx, notebook_id, "seed.md", 1700000000000LL,
                                      1700000001000LL);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // --- ImportFile ---
  char *imp_file_id = nullptr;
  err = vxcore_file_import(ctx, notebook_id, ".", external_file.c_str(), &imp_file_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(imp_file_id);

  // --- ImportFolder ---
  char *imp_folder_id = nullptr;
  err = vxcore_folder_import(ctx, notebook_id, ".", external_subdir.c_str(), "md",
                             &imp_folder_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(imp_folder_id);

  // After every rejection, on-disk state must equal the baseline byte-for-byte.
  const std::string post = snapshot_dir(nb_root);
  if (post != baseline) {
    std::cerr << "ERROR: filesystem mutated by a read-only-rejected call.\n"
              << "BASELINE:\n" << baseline << "\n"
              << "POST:\n" << post << std::endl;
    return 1;
  }

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_root);
  cleanup_test_dir(external_root);
  std::cout << "  \xE2\x9C\x93 test_raw_mutators_rejected_when_read_only passed" << std::endl;
  return 0;
}

// Subtest 2: methods that return VXCORE_ERR_UNSUPPORTED on writable raw
// notebooks must return VXCORE_ERR_READ_ONLY (not UNSUPPORTED) when the
// notebook is read-only. Read-only is the higher-priority status; the
// guard MUST run before the UNSUPPORTED early-return.
int test_raw_unsupported_mutators_become_read_only() {
  std::cout << "  Running test_raw_unsupported_mutators_become_read_only..." << std::endl;
  const std::string nb_root = get_test_path("test_raw_ro_unsupported_nb");
  cleanup_test_dir(nb_root);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, nb_root.c_str(),
                               "{\"name\":\"Raw RO Unsupported\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Seed a file so the path arg is non-trivial.
  char *seed_file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "seed.md", &seed_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(seed_file_id);

  // Sanity: on a WRITABLE raw notebook, these ops return UNSUPPORTED (the
  // pre-existing behavior of RawFolderManager). This establishes the
  // baseline before flipping read-only on.
  err = vxcore_file_tag(ctx, notebook_id, "seed.md", "alpha");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_file_untag(ctx, notebook_id, "seed.md", "alpha");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_file_update_tags(ctx, notebook_id, "seed.md", R"(["a","b"])");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_file_update_attachments(ctx, notebook_id, "seed.md", R"(["doc.pdf"])");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_file_add_attachment(ctx, notebook_id, "seed.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_file_delete_attachment(ctx, notebook_id, "seed.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  // Now flip read-only on and re-issue every call. The error code MUST
  // change to VXCORE_ERR_READ_ONLY because the guard runs first.
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "seed.md", "alpha");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = vxcore_file_untag(ctx, notebook_id, "seed.md", "alpha");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = vxcore_file_update_tags(ctx, notebook_id, "seed.md", R"(["a","b"])");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = vxcore_file_update_attachments(ctx, notebook_id, "seed.md", R"(["doc.pdf"])");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = vxcore_file_add_attachment(ctx, notebook_id, "seed.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = vxcore_file_delete_attachment(ctx, notebook_id, "seed.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_root);
  std::cout << "  \xE2\x9C\x93 test_raw_unsupported_mutators_become_read_only passed"
            << std::endl;
  return 0;
}

// Subtest 3: writable raw notebooks still succeed for every mutator that
// returned READ_ONLY in subtest 1. Confirms the guards don't leak state.
int test_raw_mutators_succeed_when_writable() {
  std::cout << "  Running test_raw_mutators_succeed_when_writable..." << std::endl;
  const std::string nb_root = get_test_path("test_raw_ro_happypath_nb");
  cleanup_test_dir(nb_root);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, nb_root.c_str(),
                               "{\"name\":\"Raw RO Happy\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Sanity: defaults to writable.
  bool is_readonly = true;
  err = vxcore_notebook_is_read_only(ctx, notebook_id, &is_readonly);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(is_readonly);

  // Flip RO on then back off so we exercise the guard once and confirm
  // recovery — closes the "leaked state" risk window.
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 1);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_notebook_set_read_only(ctx, notebook_id, 0);
  ASSERT_EQ(err, VXCORE_OK);

  // CreateFolder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "fld", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);
  vxcore_string_free(folder_id);

  // CreateFile
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "doc.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);
  vxcore_string_free(file_id);

  // RenameFolder
  err = vxcore_node_rename(ctx, notebook_id, "fld", "fld2");
  ASSERT_EQ(err, VXCORE_OK);

  // RenameFile
  err = vxcore_node_rename(ctx, notebook_id, "doc.md", "doc2.md");
  ASSERT_EQ(err, VXCORE_OK);

  // UpdateFileMetadata (unified path)
  err = vxcore_node_update_metadata(ctx, notebook_id, "doc2.md", R"({"k":"v"})");
  ASSERT_EQ(err, VXCORE_OK);

  // DeleteFile (unified path)
  err = vxcore_node_delete(ctx, notebook_id, "doc2.md");
  ASSERT_EQ(err, VXCORE_OK);

  // DeleteFolder (unified path)
  err = vxcore_node_delete(ctx, notebook_id, "fld2");
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_root);
  std::cout << "  \xE2\x9C\x93 test_raw_mutators_succeed_when_writable passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running RawFolderManager read-only tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_raw_mutators_rejected_when_read_only);
  RUN_TEST(test_raw_unsupported_mutators_become_read_only);
  RUN_TEST(test_raw_mutators_succeed_when_writable);

  std::cout << "\xE2\x9C\x93 All RawFolderManager read-only tests passed" << std::endl;
  return 0;
}
