#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

// Helper: read entire file content as string
static std::string read_file_content(const std::string &path) {
  std::ifstream ifs(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

// ============================================================================
// Test 1: Copy file with assets — verifies content rewrite and asset cloning
// ============================================================================
int test_e2e_copy_file_assets() {
  std::cout << "  Running test_e2e_copy_file_assets..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_copy_file_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_copy_file_assets_nb").c_str(),
                               "{\"name\":\"E2E Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create src_folder and dest_folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create file "note.md" in src_folder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src_folder", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);
  std::string old_uuid(file_id);

  std::string nb_root = get_test_path("e2e_copy_file_assets_nb");

  // Write markdown content with asset references AND a code block reference
  std::string file_path = nb_root + "/src_folder/note.md";
  {
    std::ofstream ofs(file_path, std::ios::binary);
    ofs << "# Test\n"
        << "![img](vx_assets/" << old_uuid << "/pic.png)\n"
        << "[doc](vx_assets/" << old_uuid << "/file.pdf)\n"
        << "```\n"
        << "vx_assets/" << old_uuid << "/code\n"
        << "```\n";
  }

  // Create assets directory with files
  std::string assets_dir = nb_root + "/src_folder/vx_assets/" + old_uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake png data"; }
  { std::ofstream(assets_dir + "/file.pdf") << "fake pdf data"; }
  ASSERT(path_exists(assets_dir + "/pic.png"));
  ASSERT(path_exists(assets_dir + "/file.pdf"));

  // Copy file to dest_folder
  char *new_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src_folder/note.md", "dest_folder", "note_copy.md",
                         &new_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(new_id);
  std::string new_uuid(new_id);

  // UUIDs must differ
  ASSERT_NE(new_uuid, old_uuid);

  // Read copied file content
  std::string copied_path = nb_root + "/dest_folder/note_copy.md";
  std::string content = read_file_content(copied_path);

  // ASSERT: content contains vx_assets/<NEW_uuid>/pic.png (rewritten)
  ASSERT(content.find("vx_assets/" + new_uuid + "/pic.png") != std::string::npos);
  // ASSERT: content contains vx_assets/<NEW_uuid>/file.pdf (rewritten)
  ASSERT(content.find("vx_assets/" + new_uuid + "/file.pdf") != std::string::npos);

  // ASSERT: code block still contains vx_assets/<OLD_uuid>/code (NOT rewritten — preserved)
  ASSERT(content.find("vx_assets/" + old_uuid + "/code") != std::string::npos);

  // ASSERT: dest_folder/vx_assets/<new_uuid>/pic.png exists on disk
  std::string new_assets = nb_root + "/dest_folder/vx_assets/" + new_uuid;
  ASSERT(path_exists(new_assets + "/pic.png"));
  ASSERT(path_exists(new_assets + "/file.pdf"));

  // ASSERT: src_folder/vx_assets/<old_uuid>/ is unchanged
  ASSERT(path_exists(assets_dir + "/pic.png"));
  ASSERT(path_exists(assets_dir + "/file.pdf"));

  vxcore_string_free(new_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_copy_file_assets_nb"));
  std::cout << "  PASS test_e2e_copy_file_assets" << std::endl;
  return 0;
}

// ============================================================================
// Test 2: Move file with assets — verifies assets relocate, content unchanged
// ============================================================================
int test_e2e_move_file_assets() {
  std::cout << "  Running test_e2e_move_file_assets..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_move_file_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_move_file_assets_nb").c_str(),
                               "{\"name\":\"E2E Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder_A with file + assets, and folder_B as destination
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "folder_A", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "folder_B", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "folder_A", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string uuid(file_id);

  std::string nb_root = get_test_path("e2e_move_file_assets_nb");

  // Write markdown content
  std::string file_path = nb_root + "/folder_A/note.md";
  {
    std::ofstream ofs(file_path, std::ios::binary);
    ofs << "# Move Test\n![img](vx_assets/" << uuid << "/pic.png)\n";
  }
  std::string original_content = read_file_content(file_path);

  // Create assets dir
  std::string assets_dir = nb_root + "/folder_A/vx_assets/" + uuid;
  std::filesystem::create_directories(assets_dir);
  { std::ofstream(assets_dir + "/pic.png") << "fake image data"; }
  ASSERT(path_exists(assets_dir + "/pic.png"));

  // Move file from folder_A to folder_B
  err = vxcore_node_move(ctx, notebook_id, "folder_A/note.md", "folder_B");
  ASSERT_EQ(err, VXCORE_OK);

  // ASSERT: folder_B/vx_assets/<uuid>/pic.png exists (same UUID, moved)
  std::string new_assets = nb_root + "/folder_B/vx_assets/" + uuid;
  ASSERT(path_exists(new_assets + "/pic.png"));

  // ASSERT: folder_A/vx_assets/<uuid>/ does NOT exist
  ASSERT_FALSE(path_exists(assets_dir));

  // ASSERT: file content is UNCHANGED (move doesn't rewrite)
  std::string moved_content = read_file_content(nb_root + "/folder_B/note.md");
  ASSERT_EQ(moved_content, original_content);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_move_file_assets_nb"));
  std::cout << "  PASS test_e2e_move_file_assets" << std::endl;
  return 0;
}

// ============================================================================
// Test 3: Copy folder recursively — verifies deep hierarchy with assets
// ============================================================================
int test_e2e_copy_folder_recursive() {
  std::cout << "  Running test_e2e_copy_folder_recursive..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_copy_folder_recursive_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_copy_folder_recursive_nb").c_str(),
                               "{\"name\":\"E2E Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create src → sub1 → sub2
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "src", "sub1", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, "src/sub1", "sub2", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create file1.md in src with assets
  char *file1_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "file1.md", &file1_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid1(file1_id);

  // Create file2.md in src/sub1 with assets
  char *file2_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src/sub1", "file2.md", &file2_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid2(file2_id);

  // Create file3.txt in src/sub1/sub2 with assets (NOT markdown)
  char *file3_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src/sub1/sub2", "file3.txt", &file3_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string old_uuid3(file3_id);

  std::string nb_root = get_test_path("e2e_copy_folder_recursive_nb");

  // Write markdown content for file1.md
  {
    std::ofstream ofs(nb_root + "/src/file1.md", std::ios::binary);
    ofs << "# File 1\n![img](vx_assets/" << old_uuid1 << "/img1.png)\n";
  }
  // Create assets for file1
  std::string a1 = nb_root + "/src/vx_assets/" + old_uuid1;
  std::filesystem::create_directories(a1);
  { std::ofstream(a1 + "/img1.png") << "image1"; }

  // Write markdown content for file2.md
  {
    std::ofstream ofs(nb_root + "/src/sub1/file2.md", std::ios::binary);
    ofs << "# File 2\n[doc](vx_assets/" << old_uuid2 << "/doc.pdf)\n";
  }
  // Create assets for file2
  std::string a2 = nb_root + "/src/sub1/vx_assets/" + old_uuid2;
  std::filesystem::create_directories(a2);
  { std::ofstream(a2 + "/doc.pdf") << "pdfdoc"; }

  // Write content for file3.txt (non-markdown with asset ref)
  {
    std::ofstream ofs(nb_root + "/src/sub1/sub2/file3.txt", std::ios::binary);
    ofs << "Data: vx_assets/" << old_uuid3 << "/data.bin\n";
  }
  // Create assets for file3
  std::string a3 = nb_root + "/src/sub1/sub2/vx_assets/" + old_uuid3;
  std::filesystem::create_directories(a3);
  { std::ofstream(a3 + "/data.bin") << "binary"; }

  // Save source content for verification
  std::string src_file1_content = read_file_content(nb_root + "/src/file1.md");
  std::string src_file2_content = read_file_content(nb_root + "/src/sub1/file2.md");
  std::string src_file3_content = read_file_content(nb_root + "/src/sub1/sub2/file3.txt");

  // Copy the whole folder tree
  char *copy_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src", ".", "src_copy", &copy_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copy_id);
  vxcore_string_free(copy_id);

  // Get new UUIDs from copied folder config
  char *config_json = nullptr;
  err = vxcore_node_get_config(ctx, notebook_id, "src_copy", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j1 = nlohmann::json::parse(config_json);
  std::string new_uuid1 = j1["files"][0]["id"].get<std::string>();
  vxcore_string_free(config_json);
  ASSERT_NE(new_uuid1, old_uuid1);

  err = vxcore_node_get_config(ctx, notebook_id, "src_copy/sub1", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j2 = nlohmann::json::parse(config_json);
  std::string new_uuid2 = j2["files"][0]["id"].get<std::string>();
  vxcore_string_free(config_json);
  ASSERT_NE(new_uuid2, old_uuid2);

  err = vxcore_node_get_config(ctx, notebook_id, "src_copy/sub1/sub2", &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j3 = nlohmann::json::parse(config_json);
  std::string new_uuid3 = j3["files"][0]["id"].get<std::string>();
  vxcore_string_free(config_json);
  ASSERT_NE(new_uuid3, old_uuid3);

  // Verify assets dirs exist for each copied file
  ASSERT(path_exists(nb_root + "/src_copy/vx_assets/" + new_uuid1 + "/img1.png"));
  ASSERT(path_exists(nb_root + "/src_copy/sub1/vx_assets/" + new_uuid2 + "/doc.pdf"));
  ASSERT(path_exists(nb_root + "/src_copy/sub1/sub2/vx_assets/" + new_uuid3 + "/data.bin"));

  // ASSERT: MD content rewritten with new UUIDs
  std::string copied_file1 = read_file_content(nb_root + "/src_copy/file1.md");
  ASSERT(copied_file1.find("vx_assets/" + new_uuid1) != std::string::npos);
  ASSERT(copied_file1.find("vx_assets/" + old_uuid1) == std::string::npos);

  std::string copied_file2 = read_file_content(nb_root + "/src_copy/sub1/file2.md");
  ASSERT(copied_file2.find("vx_assets/" + new_uuid2) != std::string::npos);
  ASSERT(copied_file2.find("vx_assets/" + old_uuid2) == std::string::npos);

  // ASSERT: file3.txt content is NOT modified (not markdown)
  std::string copied_file3 = read_file_content(nb_root + "/src_copy/sub1/sub2/file3.txt");
  ASSERT(copied_file3.find("vx_assets/" + old_uuid3) != std::string::npos);

  // ASSERT: source tree completely unchanged
  ASSERT_EQ(read_file_content(nb_root + "/src/file1.md"), src_file1_content);
  ASSERT_EQ(read_file_content(nb_root + "/src/sub1/file2.md"), src_file2_content);
  ASSERT_EQ(read_file_content(nb_root + "/src/sub1/sub2/file3.txt"), src_file3_content);
  ASSERT(path_exists(a1 + "/img1.png"));
  ASSERT(path_exists(a2 + "/doc.pdf"));
  ASSERT(path_exists(a3 + "/data.bin"));

  vxcore_string_free(file1_id);
  vxcore_string_free(file2_id);
  vxcore_string_free(file3_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_copy_folder_recursive_nb"));
  std::cout << "  PASS test_e2e_copy_folder_recursive" << std::endl;
  return 0;
}

// ============================================================================
// Test 4: Copy file with no assets — no empty assets dir created
// ============================================================================
int test_e2e_no_assets() {
  std::cout << "  Running test_e2e_no_assets..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_no_assets_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_no_assets_nb").c_str(),
                               "{\"name\":\"E2E Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file with NO assets directory at all
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "plain.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write plain markdown
  std::string nb_root = get_test_path("e2e_no_assets_nb");
  std::string file_path = nb_root + "/plain.md";
  { std::ofstream(file_path, std::ios::binary) << "# Plain file\nNo assets here.\n"; }

  // Copy the file
  char *copied_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "plain.md", ".", "plain_copy.md", &copied_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(copied_id);

  // ASSERT: VXCORE_OK returned (already checked above)

  // ASSERT: NO empty vx_assets/<new_uuid>/ directory was created at destination
  std::string new_assets_dir = nb_root + "/vx_assets/" + std::string(copied_id);
  ASSERT_FALSE(path_exists(new_assets_dir));

  // Verify the file was actually copied
  ASSERT(path_exists(nb_root + "/plain_copy.md"));

  vxcore_string_free(copied_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_no_assets_nb"));
  std::cout << "  PASS test_e2e_no_assets" << std::endl;
  return 0;
}

// ============================================================================
// Test 7: Raw notebook — Copy file with relative-path links
// ============================================================================
int test_e2e_raw_copy_file_relative_links() {
  std::cout << "  Running test_e2e_raw_copy_file_relative_links..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_raw_copy_rel_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_raw_copy_rel_nb").c_str(),
                               "{\"name\":\"Raw Rel Copy\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create src_folder and dest_folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create note.md in src_folder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src_folder", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  std::string nb_root = get_test_path("e2e_raw_copy_rel_nb");

  // Write markdown with relative-path links
  std::string file_path = nb_root + "/src_folder/note.md";
  {
    std::ofstream ofs(file_path, std::ios::binary);
    ofs << "![img](images/photo.jpg)\n[doc](docs/readme.txt)\n";
  }

  // Create the linked files on disk
  std::filesystem::create_directories(nb_root + "/src_folder/images");
  std::filesystem::create_directories(nb_root + "/src_folder/docs");
  { std::ofstream(nb_root + "/src_folder/images/photo.jpg") << "fake jpg"; }
  { std::ofstream(nb_root + "/src_folder/docs/readme.txt") << "fake txt"; }
  ASSERT(path_exists(nb_root + "/src_folder/images/photo.jpg"));
  ASSERT(path_exists(nb_root + "/src_folder/docs/readme.txt"));

  // Copy file to dest_folder
  char *new_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src_folder/note.md", "dest_folder", "note_copy.md",
                         &new_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(new_id);

  // Verify linked files copied to dest_folder
  ASSERT(path_exists(nb_root + "/dest_folder/images/photo.jpg"));
  ASSERT(path_exists(nb_root + "/dest_folder/docs/readme.txt"));

  // Verify src files still exist (copy, not move)
  ASSERT(path_exists(nb_root + "/src_folder/images/photo.jpg"));
  ASSERT(path_exists(nb_root + "/src_folder/docs/readme.txt"));

  vxcore_string_free(new_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_raw_copy_rel_nb"));
  std::cout << "  PASS test_e2e_raw_copy_file_relative_links" << std::endl;
  return 0;
}

// ============================================================================
// Test 8: Raw notebook — Move file with relative-path links
// ============================================================================
int test_e2e_raw_move_file_relative_links() {
  std::cout << "  Running test_e2e_raw_move_file_relative_links..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_raw_move_rel_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_raw_move_rel_nb").c_str(),
                               "{\"name\":\"Raw Rel Move\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create src_folder and dest_folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create note.md in src_folder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src_folder", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  std::string nb_root = get_test_path("e2e_raw_move_rel_nb");

  // Write markdown with relative-path links
  std::string file_path = nb_root + "/src_folder/note.md";
  {
    std::ofstream ofs(file_path, std::ios::binary);
    ofs << "![img](images/photo.jpg)\n[doc](docs/readme.txt)\n";
  }

  // Create the linked files on disk
  std::filesystem::create_directories(nb_root + "/src_folder/images");
  std::filesystem::create_directories(nb_root + "/src_folder/docs");
  { std::ofstream(nb_root + "/src_folder/images/photo.jpg") << "fake jpg"; }
  { std::ofstream(nb_root + "/src_folder/docs/readme.txt") << "fake txt"; }
  ASSERT(path_exists(nb_root + "/src_folder/images/photo.jpg"));
  ASSERT(path_exists(nb_root + "/src_folder/docs/readme.txt"));

  // Move file to dest_folder
  err = vxcore_node_move(ctx, notebook_id, "src_folder/note.md", "dest_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify linked files moved to dest_folder
  ASSERT(path_exists(nb_root + "/dest_folder/images/photo.jpg"));
  ASSERT(path_exists(nb_root + "/dest_folder/docs/readme.txt"));

  // Verify src files do NOT exist (moved, not copied)
  ASSERT_FALSE(path_exists(nb_root + "/src_folder/images/photo.jpg"));
  ASSERT_FALSE(path_exists(nb_root + "/src_folder/docs/readme.txt"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_raw_move_rel_nb"));
  std::cout << "  PASS test_e2e_raw_move_file_relative_links" << std::endl;
  return 0;
}

// ============================================================================
// Test 5: Copy file with relative-path links — verifies linked files are copied
// ============================================================================
int test_e2e_copy_file_relative_links() {
  std::cout << "  Running test_e2e_copy_file_relative_links..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_copy_rel_links_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_copy_rel_links_nb").c_str(),
                               "{\"name\":\"E2E Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create src_folder and dest_folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create file "note.md" in src_folder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src_folder", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  std::string nb_root = get_test_path("e2e_copy_rel_links_nb");

  // Write markdown content with relative-path links
  std::string file_path = nb_root + "/src_folder/note.md";
  {
    std::ofstream ofs(file_path, std::ios::binary);
    ofs << "![img](images/pic.png)\n[data](data/report.csv)\n";
  }

  // Create the linked files on disk
  std::filesystem::create_directories(nb_root + "/src_folder/images");
  std::filesystem::create_directories(nb_root + "/src_folder/data");
  { std::ofstream(nb_root + "/src_folder/images/pic.png") << "fake png data"; }
  { std::ofstream(nb_root + "/src_folder/data/report.csv") << "col1,col2\na,b\n"; }
  ASSERT(path_exists(nb_root + "/src_folder/images/pic.png"));
  ASSERT(path_exists(nb_root + "/src_folder/data/report.csv"));

  // Copy file to dest_folder
  char *new_id = nullptr;
  err = vxcore_node_copy(ctx, notebook_id, "src_folder/note.md", "dest_folder", "note_copy.md",
                         &new_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(new_id);

  // Verify relative-linked files were copied to dest_folder
  ASSERT(path_exists(nb_root + "/dest_folder/images/pic.png"));
  ASSERT(path_exists(nb_root + "/dest_folder/data/report.csv"));

  // Verify source files still exist (copy, not move)
  ASSERT(path_exists(nb_root + "/src_folder/images/pic.png"));
  ASSERT(path_exists(nb_root + "/src_folder/data/report.csv"));

  vxcore_string_free(new_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_copy_rel_links_nb"));
  std::cout << "  PASS test_e2e_copy_file_relative_links" << std::endl;
  return 0;
}

// ============================================================================
// Test 6: Move file with relative-path links — verifies linked files are moved
// ============================================================================
int test_e2e_move_file_relative_links() {
  std::cout << "  Running test_e2e_move_file_relative_links..." << std::endl;
  cleanup_test_dir(get_test_path("e2e_move_rel_links_nb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("e2e_move_rel_links_nb").c_str(),
                               "{\"name\":\"E2E Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create src_folder and dest_folder
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  err = vxcore_folder_create(ctx, notebook_id, ".", "dest_folder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Create file "note.md" in src_folder
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src_folder", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  std::string nb_root = get_test_path("e2e_move_rel_links_nb");

  // Write markdown content with relative-path links
  std::string file_path = nb_root + "/src_folder/note.md";
  {
    std::ofstream ofs(file_path, std::ios::binary);
    ofs << "![img](images/pic.png)\n[data](data/report.csv)\n";
  }

  // Create the linked files on disk
  std::filesystem::create_directories(nb_root + "/src_folder/images");
  std::filesystem::create_directories(nb_root + "/src_folder/data");
  { std::ofstream(nb_root + "/src_folder/images/pic.png") << "fake png data"; }
  { std::ofstream(nb_root + "/src_folder/data/report.csv") << "col1,col2\na,b\n"; }
  ASSERT(path_exists(nb_root + "/src_folder/images/pic.png"));
  ASSERT(path_exists(nb_root + "/src_folder/data/report.csv"));

  // Move file from src_folder to dest_folder
  err = vxcore_node_move(ctx, notebook_id, "src_folder/note.md", "dest_folder");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify relative-linked files were moved to dest_folder
  ASSERT(path_exists(nb_root + "/dest_folder/images/pic.png"));
  ASSERT(path_exists(nb_root + "/dest_folder/data/report.csv"));

  // Verify source files do NOT exist (moved, not copied)
  ASSERT_FALSE(path_exists(nb_root + "/src_folder/images/pic.png"));
  ASSERT_FALSE(path_exists(nb_root + "/src_folder/data/report.csv"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("e2e_move_rel_links_nb"));
  std::cout << "  PASS test_e2e_move_file_relative_links" << std::endl;
  return 0;
}

// ============================================================================
// main
// ============================================================================

int main() {
  std::cout << "Running E2E assets tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  int scenarios_pass = 0;
  int scenarios_total = 0;
  int integration_pass = 0;
  int integration_total = 0;
  int edge_tested = 0;

  // Scenario: File copy with assets
  scenarios_total++;
  if (test_e2e_copy_file_assets() == 0) scenarios_pass++;

  // Scenario: File move with assets
  scenarios_total++;
  if (test_e2e_move_file_assets() == 0) scenarios_pass++;

  // Integration: Recursive folder copy
  integration_total++;
  if (test_e2e_copy_folder_recursive() == 0) integration_pass++;

  // Edge case: No assets
  edge_tested++;
  if (test_e2e_no_assets() != 0) {
    std::cerr << "FAIL: test_e2e_no_assets" << std::endl;
    std::cout << "Scenarios [" << scenarios_pass << "/" << scenarios_total << " pass] | Integration ["
              << integration_pass << "/" << integration_total << "] | Edge Cases [" << edge_tested
              << " tested] | VERDICT: REJECT" << std::endl;
    return 1;
  }

  // Scenario: File copy with relative-path links
  scenarios_total++;
  if (test_e2e_copy_file_relative_links() == 0) scenarios_pass++;

  // Scenario: File move with relative-path links
  scenarios_total++;
  if (test_e2e_move_file_relative_links() == 0) scenarios_pass++;

  // Scenario: Raw notebook copy file with relative links
  scenarios_total++;
  if (test_e2e_raw_copy_file_relative_links() == 0) scenarios_pass++;

  // Scenario: Raw notebook move file with relative links
  scenarios_total++;
  if (test_e2e_raw_move_file_relative_links() == 0) scenarios_pass++;

  bool all_pass = (scenarios_pass == scenarios_total) && (integration_pass == integration_total);

  std::cout << "Scenarios [" << scenarios_pass << "/" << scenarios_total << " pass] | Integration ["
            << integration_pass << "/" << integration_total << "] | Edge Cases [" << edge_tested
            << " tested] | VERDICT: " << (all_pass ? "APPROVE" : "REJECT") << std::endl;

  return all_pass ? 0 : 1;
}
