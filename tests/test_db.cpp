#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

#include "db/db_manager.h"
#include "db/file_db.h"
#include "db/sync_manager.h"
#include "db/tag_db.h"
#include "test_utils.h"

using namespace vxcore::db;

// Test database path
static std::string test_db_path;

// Helper to setup test database
void setup_test_db() {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  test_db_path = (temp_dir / "vxcore_test_db.sqlite").string();

  // Remove if exists
  if (std::filesystem::exists(test_db_path)) {
    std::filesystem::remove(test_db_path);
  }
}

// Helper to cleanup test database
void cleanup_test_db() {
  if (std::filesystem::exists(test_db_path)) {
    std::filesystem::remove(test_db_path);
  }
}

// ============================================================================
// DbSyncManager Test Helpers
// ============================================================================

// Test directory for sync tests (filesystem root with vx.json files)
static std::string test_sync_root;

// Simple file info for vx.json fixture (matches FolderConfig::files structure)
struct TestFileInfo {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  std::vector<std::string> tags;
};

// Simple folder config for vx.json fixture (matches FolderConfig structure)
struct TestFolderConfig {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  std::vector<TestFileInfo> files;
  std::vector<std::string> folders;
};

// Helper to setup sync test directory structure
void setup_sync_test_dir() {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  test_sync_root = (temp_dir / "vxcore_sync_test").string();

  // Remove if exists
  if (std::filesystem::exists(test_sync_root)) {
    std::filesystem::remove_all(test_sync_root);
  }

  // Create base structure: {root}/vx_notebook/notes/
  std::filesystem::create_directories(std::filesystem::path(test_sync_root) / "vx_notebook" /
                                      "notes");
}

// Helper to cleanup sync test directory
void cleanup_sync_test_dir() {
  if (std::filesystem::exists(test_sync_root)) {
    std::filesystem::remove_all(test_sync_root);
  }
}

// Helper to write a vx.json file for a folder
// Creates {fs_root}/vx_notebook/notes/{folder_path}/vx.json with folder config
// Returns true on success
bool write_vx_json(const std::string& fs_root, const std::string& folder_path,
                   const TestFolderConfig& config) {
  try {
    std::filesystem::path vx_json_path = std::filesystem::path(fs_root) / "vx_notebook" / "notes";
    if (!folder_path.empty() && folder_path != ".") {
      vx_json_path /= folder_path;
    }

    // Create directory if it doesn't exist
    std::filesystem::create_directories(vx_json_path);

    vx_json_path /= "vx.json";

    // Build JSON manually to match FolderConfig::ToJson() output
    nlohmann::json json;
    json["id"] = config.id;
    json["name"] = config.name;
    json["createdUtc"] = config.created_utc;
    json["modifiedUtc"] = config.modified_utc;
    json["metadata"] = nlohmann::json::object();

    nlohmann::json files_json = nlohmann::json::array();
    for (const auto& file : config.files) {
      nlohmann::json file_json;
      file_json["id"] = file.id;
      file_json["name"] = file.name;
      file_json["createdUtc"] = file.created_utc;
      file_json["modifiedUtc"] = file.modified_utc;
      file_json["metadata"] = nlohmann::json::object();
      file_json["tags"] = file.tags;
      files_json.push_back(file_json);
    }
    json["files"] = files_json;
    json["folders"] = config.folders;

    // Write JSON content
    std::ofstream file(vx_json_path, std::ios::binary);
    if (!file.is_open()) {
      return false;
    }

    file << json.dump(2);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// ============================================================================
// DbManager Tests
// ============================================================================

int test_db_manager_create() {
  std::cout << "  Running test_db_manager_create..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_FALSE(db_manager.IsOpen());
  ASSERT_EQ(db_manager.GetPath(), "");

  cleanup_test_db();
  std::cout << "  ✓ test_db_manager_create passed" << std::endl;
  return 0;
}

int test_db_manager_open_close() {
  std::cout << "  Running test_db_manager_open_close..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.IsOpen());
  ASSERT_EQ(db_manager.GetPath(), test_db_path);
  ASSERT_NOT_NULL(db_manager.GetHandle());

  db_manager.Close();
  ASSERT_FALSE(db_manager.IsOpen());
  ASSERT_EQ(db_manager.GetPath(), "");
  ASSERT_NULL(db_manager.GetHandle());

  cleanup_test_db();
  std::cout << "  ✓ test_db_manager_open_close passed" << std::endl;
  return 0;
}

int test_db_manager_initialize_schema() {
  std::cout << "  Running test_db_manager_initialize_schema..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  // Verify database file exists
  ASSERT_TRUE(std::filesystem::exists(test_db_path));

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_db_manager_initialize_schema passed" << std::endl;
  return 0;
}

int test_db_manager_transactions() {
  std::cout << "  Running test_db_manager_transactions..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  ASSERT_TRUE(db_manager.BeginTransaction());
  ASSERT_TRUE(db_manager.CommitTransaction());

  ASSERT_TRUE(db_manager.BeginTransaction());
  ASSERT_TRUE(db_manager.RollbackTransaction());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_db_manager_transactions passed" << std::endl;
  return 0;
}

int test_db_manager_rebuild() {
  std::cout << "  Running test_db_manager_rebuild..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  // Create some data
  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());
  int64_t folder_id = file_db.CreateFolder(-1, "test_folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Verify data exists
  auto folder = file_db.GetFolder(folder_id);
  ASSERT_TRUE(folder.has_value());

  // Rebuild database (should drop all tables and recreate)
  ASSERT_TRUE(db_manager.RebuildDatabase());

  // Verify data was deleted
  folder = file_db.GetFolder(folder_id);
  ASSERT_FALSE(folder.has_value());

  // Verify we can create new data after rebuild
  int64_t new_folder_id = file_db.CreateFolder(-1, "new_folder", 3000, 4000);
  ASSERT_NE(new_folder_id, -1);

  auto new_folder = file_db.GetFolder(new_folder_id);
  ASSERT_TRUE(new_folder.has_value());
  ASSERT_EQ(new_folder->name, "new_folder");

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_db_manager_rebuild passed" << std::endl;
  return 0;
}

// ============================================================================
// FileDb - Folder Tests
// ============================================================================

int test_filedb_create_folder() {
  std::cout << "  Running test_filedb_create_folder..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create root folder
  int64_t folder_id = file_db.CreateFolder(-1, "root_folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Verify folder exists
  auto folder = file_db.GetFolder(folder_id);
  ASSERT_TRUE(folder.has_value());
  ASSERT_EQ(folder->id, folder_id);
  ASSERT_EQ(folder->name, "root_folder");
  ASSERT_EQ(folder->parent_id, -1);
  ASSERT_EQ(folder->created_utc, 1000);
  ASSERT_EQ(folder->modified_utc, 2000);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_create_folder passed" << std::endl;
  return 0;
}

int test_filedb_create_or_update_folder() {
  std::cout << "  Running test_filedb_create_or_update_folder..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  std::string uuid = "folder-uuid-123";
  std::string name = "guides";
  std::string metadata = "{\"color\": \"blue\"}";

  // Create folder with UUID
  int64_t folder_id = file_db.CreateOrUpdateFolder(uuid, -1, name, 1000, 2000, metadata);
  ASSERT_NE(folder_id, -1);

  // Verify folder
  auto folder = file_db.GetFolderByUuid(uuid);
  ASSERT_TRUE(folder.has_value());
  ASSERT_EQ(folder->uuid, uuid);
  ASSERT_EQ(folder->name, name);
  ASSERT_EQ(folder->metadata, metadata);
  ASSERT_EQ(folder->created_utc, 1000);
  ASSERT_EQ(folder->modified_utc, 2000);

  // Update same folder (should replace)
  std::string new_metadata = "{\"color\": \"red\"}";
  int64_t folder_id2 = file_db.CreateOrUpdateFolder(uuid, -1, name, 1000, 3000, new_metadata);
  ASSERT_NE(folder_id2, -1);

  // Verify update
  auto folder2 = file_db.GetFolderByUuid(uuid);
  ASSERT_TRUE(folder2.has_value());
  ASSERT_EQ(folder2->uuid, uuid);
  ASSERT_EQ(folder2->modified_utc, 3000);
  ASSERT_EQ(folder2->metadata, new_metadata);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_create_or_update_folder passed" << std::endl;
  return 0;
}

int test_filedb_get_folder_path() {
  std::cout << "  Running test_filedb_get_folder_path..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder hierarchy: root -> projects -> vxcore
  int64_t root_id = file_db.CreateFolder(-1, "root", 1000, 2000);
  ASSERT_NE(root_id, -1);

  int64_t projects_id = file_db.CreateFolder(root_id, "projects", 1100, 2100);
  ASSERT_NE(projects_id, -1);

  int64_t vxcore_id = file_db.CreateFolder(projects_id, "vxcore", 1200, 2200);
  ASSERT_NE(vxcore_id, -1);

  // Verify path computation
  std::string root_path = file_db.GetFolderPath(root_id);
  ASSERT_EQ(root_path, "root");

  std::string projects_path = file_db.GetFolderPath(projects_id);
  ASSERT_EQ(projects_path, "root/projects");

  std::string vxcore_path = file_db.GetFolderPath(vxcore_id);
  ASSERT_EQ(vxcore_path, "root/projects/vxcore");

  // Non-existent folder returns empty
  std::string not_found = file_db.GetFolderPath(9999);
  ASSERT_EQ(not_found, "");

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_get_folder_path passed" << std::endl;
  return 0;
}

int test_filedb_list_folders() {
  std::cout << "  Running test_filedb_list_folders..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create root folder
  int64_t root_id = file_db.CreateFolder(-1, "root", 1000, 2000);
  ASSERT_NE(root_id, -1);

  // Create child folders
  int64_t child1_id = file_db.CreateFolder(root_id, "child1", 1100, 2100);
  int64_t child2_id = file_db.CreateFolder(root_id, "child2", 1200, 2200);
  ASSERT_NE(child1_id, -1);
  ASSERT_NE(child2_id, -1);

  // List root folders
  auto root_folders = file_db.ListFolders(-1);
  ASSERT_EQ(root_folders.size(), 1);
  ASSERT_EQ(root_folders[0].name, "root");

  // List child folders
  auto child_folders = file_db.ListFolders(root_id);
  ASSERT_EQ(child_folders.size(), 2);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_list_folders passed" << std::endl;
  return 0;
}

int test_filedb_update_delete_folder() {
  std::cout << "  Running test_filedb_update_delete_folder..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "test_folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Update folder
  ASSERT_TRUE(file_db.UpdateFolder(folder_id, "renamed_folder", 3000));

  auto updated = file_db.GetFolder(folder_id);
  ASSERT_TRUE(updated.has_value());
  ASSERT_EQ(updated->name, "renamed_folder");
  ASSERT_EQ(updated->modified_utc, 3000);

  // Delete folder
  ASSERT_TRUE(file_db.DeleteFolder(folder_id));

  auto deleted = file_db.GetFolder(folder_id);
  ASSERT_FALSE(deleted.has_value());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_update_delete_folder passed" << std::endl;
  return 0;
}

// ============================================================================
// FileDb - File Tests
// ============================================================================

int test_filedb_move_folder() {
  std::cout << "  Running test_filedb_move_folder..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder hierarchy: root1, root2, root1/child
  int64_t root1_id = file_db.CreateFolder(-1, "root1", 1000, 2000);
  int64_t root2_id = file_db.CreateFolder(-1, "root2", 1100, 2100);
  int64_t child_id = file_db.CreateFolder(root1_id, "child", 1200, 2200);
  ASSERT_NE(root1_id, -1);
  ASSERT_NE(root2_id, -1);
  ASSERT_NE(child_id, -1);

  // Move child from root1 to root2
  ASSERT_TRUE(file_db.MoveFolder(child_id, root2_id));

  auto moved = file_db.GetFolder(child_id);
  ASSERT_TRUE(moved.has_value());
  ASSERT_EQ(moved->parent_id, root2_id);

  // Verify path computation reflects the move
  std::string new_path = file_db.GetFolderPath(child_id);
  ASSERT_EQ(new_path, "root2/child");

  // Verify root1 has no children now
  auto root1_children = file_db.ListFolders(root1_id);
  ASSERT_EQ(root1_children.size(), 0);

  // Verify root2 has the child
  auto root2_children = file_db.ListFolders(root2_id);
  ASSERT_EQ(root2_children.size(), 1);
  ASSERT_EQ(root2_children[0].id, child_id);

  // Test cycle detection: cannot move folder to itself
  ASSERT_FALSE(file_db.MoveFolder(root2_id, root2_id));

  // Test cycle detection: cannot move parent to its descendant
  ASSERT_FALSE(file_db.MoveFolder(root2_id, child_id));

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_move_folder passed" << std::endl;
  return 0;
}

int test_filedb_move_file() {
  std::cout << "  Running test_filedb_move_file..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create two folders
  int64_t folder1_id = file_db.CreateFolder(-1, "folder1", 1000, 2000);
  int64_t folder2_id = file_db.CreateFolder(-1, "folder2", 1100, 2100);
  ASSERT_NE(folder1_id, -1);
  ASSERT_NE(folder2_id, -1);

  // Create file in folder1
  std::vector<std::string> tags = {"tag1"};
  int64_t file_id = file_db.CreateFile(folder1_id, "test.md", 1200, 2200, tags);
  ASSERT_NE(file_id, -1);

  // Move file to folder2
  ASSERT_TRUE(file_db.MoveFile(file_id, folder2_id));

  // Verify file moved
  auto moved = file_db.GetFile(file_id);
  ASSERT_TRUE(moved.has_value());
  ASSERT_EQ(moved->folder_id, folder2_id);

  // Verify tags are preserved
  ASSERT_EQ(moved->tags.size(), 1);
  ASSERT_EQ(moved->tags[0], "tag1");

  // Verify folder1 has no files
  auto folder1_files = file_db.ListFiles(folder1_id);
  ASSERT_EQ(folder1_files.size(), 0);

  // Verify folder2 has the file
  auto folder2_files = file_db.ListFiles(folder2_id);
  ASSERT_EQ(folder2_files.size(), 1);
  ASSERT_EQ(folder2_files[0].id, file_id);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_move_file passed" << std::endl;
  return 0;
}

int test_filedb_create_file() {
  std::cout << "  Running test_filedb_create_file..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder first
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Create file with tags
  std::vector<std::string> tags = {"tag1", "tag2"};
  int64_t file_id = file_db.CreateFile(folder_id, "test.md", 1100, 2100, tags);
  ASSERT_NE(file_id, -1);

  // Verify file
  auto file = file_db.GetFile(file_id);
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->id, file_id);
  ASSERT_EQ(file->folder_id, folder_id);
  ASSERT_EQ(file->name, "test.md");
  ASSERT_EQ(file->created_utc, 1100);
  ASSERT_EQ(file->modified_utc, 2100);
  ASSERT_EQ(file->tags.size(), 2);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_create_file passed" << std::endl;
  return 0;
}

int test_filedb_create_or_update_file() {
  std::cout << "  Running test_filedb_create_or_update_file..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  std::string uuid = "file-uuid-789";
  std::string name = "document.md";
  std::string metadata = "{\"importance\": \"high\"}";

  // Create file with UUID
  int64_t file_id = file_db.CreateOrUpdateFile(uuid, folder_id, name, 1100, 2100, metadata);
  ASSERT_NE(file_id, -1);

  // Verify file
  auto file = file_db.GetFileByUuid(uuid);
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->uuid, uuid);
  ASSERT_EQ(file->name, name);
  ASSERT_EQ(file->metadata, metadata);
  ASSERT_EQ(file->created_utc, 1100);
  ASSERT_EQ(file->modified_utc, 2100);

  // Update same file
  std::string new_metadata = "{\"importance\": \"critical\"}";
  int64_t file_id2 = file_db.CreateOrUpdateFile(uuid, folder_id, name, 1100, 3100, new_metadata);
  ASSERT_NE(file_id2, -1);

  // Verify update
  auto file2 = file_db.GetFileByUuid(uuid);
  ASSERT_TRUE(file2.has_value());
  ASSERT_EQ(file2->uuid, uuid);
  ASSERT_EQ(file2->modified_utc, 3100);
  ASSERT_EQ(file2->metadata, new_metadata);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_create_or_update_file passed" << std::endl;
  return 0;
}

int test_filedb_get_file_by_name() {
  std::cout << "  Running test_filedb_get_file_by_name..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Create file
  std::string uuid = "file-uuid-abc";
  int64_t file_id = file_db.CreateOrUpdateFile(uuid, folder_id, "notes.md", 1100, 2100, "{}");
  ASSERT_NE(file_id, -1);

  // Lookup by name
  auto file = file_db.GetFileByName(folder_id, "notes.md");
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->uuid, uuid);
  ASSERT_EQ(file->name, "notes.md");

  // Lookup non-existent file
  auto not_found = file_db.GetFileByName(folder_id, "nonexistent.md");
  ASSERT_FALSE(not_found.has_value());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_get_file_by_name passed" << std::endl;
  return 0;
}

int test_filedb_list_files() {
  std::cout << "  Running test_filedb_list_files..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Create files
  std::vector<std::string> tags;
  int64_t file1_id = file_db.CreateFile(folder_id, "file1.md", 1100, 2100, tags);
  int64_t file2_id = file_db.CreateFile(folder_id, "file2.md", 1200, 2200, tags);
  ASSERT_NE(file1_id, -1);
  ASSERT_NE(file2_id, -1);

  // List files
  auto files = file_db.ListFiles(folder_id);
  ASSERT_EQ(files.size(), 2);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_list_files passed" << std::endl;
  return 0;
}

int test_filedb_update_delete_file() {
  std::cout << "  Running test_filedb_update_delete_file..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder and file
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  std::vector<std::string> tags = {"tag1"};
  int64_t file_id = file_db.CreateFile(folder_id, "test.md", 1100, 2100, tags);
  ASSERT_NE(file_id, -1);

  // Update file
  std::vector<std::string> new_tags = {"tag1", "tag2", "tag3"};
  ASSERT_TRUE(file_db.UpdateFile(file_id, "renamed.md", 3100, new_tags));

  auto updated = file_db.GetFile(file_id);
  ASSERT_TRUE(updated.has_value());
  ASSERT_EQ(updated->name, "renamed.md");
  ASSERT_EQ(updated->modified_utc, 3100);
  ASSERT_EQ(updated->tags.size(), 3);

  // Delete file
  ASSERT_TRUE(file_db.DeleteFile(file_id));

  auto deleted = file_db.GetFile(file_id);
  ASSERT_FALSE(deleted.has_value());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_update_delete_file passed" << std::endl;
  return 0;
}

// ============================================================================
// TagDb - Tag Tests
// ============================================================================

int test_tagdb_create_tag() {
  std::cout << "  Running test_tagdb_create_tag..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());

  // Create root tag
  int64_t tag_id = tag_db.CreateOrUpdateTag("important", -1, "{}");
  ASSERT_NE(tag_id, -1);

  // Verify tag
  auto tag = tag_db.GetTag("important");
  ASSERT_TRUE(tag.has_value());
  ASSERT_EQ(tag->name, "important");
  ASSERT_EQ(tag->parent_id, -1);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_create_tag passed" << std::endl;
  return 0;
}

int test_tagdb_get_or_create_tag() {
  std::cout << "  Running test_tagdb_get_or_create_tag..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());

  // Get or create tag (should create)
  int64_t tag_id1 = tag_db.GetOrCreateTag("work");
  ASSERT_NE(tag_id1, -1);

  // Get or create same tag (should return existing)
  int64_t tag_id2 = tag_db.GetOrCreateTag("work");
  ASSERT_EQ(tag_id1, tag_id2);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_get_or_create_tag passed" << std::endl;
  return 0;
}

int test_filedb_add_tag_to_file() {
  std::cout << "  Running test_filedb_add_tag_to_file..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder and file
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  std::vector<std::string> tags;
  int64_t file_id = file_db.CreateFile(folder_id, "test.md", 1100, 2100, tags);
  ASSERT_NE(file_id, -1);

  // Add tag to file
  ASSERT_TRUE(file_db.AddTagToFile(file_id, "urgent"));

  // Verify tag was added
  auto file = file_db.GetFile(file_id);
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->tags.size(), 1);
  ASSERT_EQ(file->tags[0], "urgent");

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_add_tag_to_file passed" << std::endl;
  return 0;
}

int test_tagdb_list_tags() {
  std::cout << "  Running test_tagdb_list_tags..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());

  // Create tags
  int64_t root_tag = tag_db.CreateOrUpdateTag("category", -1, "{}");
  int64_t child_tag1 = tag_db.CreateOrUpdateTag("work", root_tag, "{}");
  int64_t child_tag2 = tag_db.CreateOrUpdateTag("personal", root_tag, "{}");
  ASSERT_NE(root_tag, -1);
  ASSERT_NE(child_tag1, -1);
  ASSERT_NE(child_tag2, -1);

  // List all tags
  auto all_tags = tag_db.ListAllTags();
  ASSERT_EQ(all_tags.size(), 3);

  // List child tags
  auto child_tags = tag_db.ListChildTags(root_tag);
  ASSERT_EQ(child_tags.size(), 2);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_list_tags passed" << std::endl;
  return 0;
}

// ============================================================================
// DbSyncManager Tests
// ============================================================================

int test_dbsync_needs_synchronization() {
  std::cout << "  Running test_dbsync_needs_synchronization..." << std::endl;

  setup_test_db();
  setup_sync_test_dir();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());
  DbSyncManager sync_manager(db_manager.GetHandle(), &file_db);

  // Create a vx.json file for "notes" folder
  TestFolderConfig config;
  config.id = "folder-uuid-123";
  config.name = "notes";
  config.created_utc = 1000;
  config.modified_utc = 2000;
  ASSERT_TRUE(write_vx_json(test_sync_root, "notes", config));

  // Folder not in DB → needs sync
  ASSERT_TRUE(sync_manager.NeedsSynchronization("notes", test_sync_root));

  // Create folder in DB but without sync state
  int64_t folder_id =
      file_db.CreateOrUpdateFolder("folder-uuid-123", -1, "notes", 1000, 2000, "{}");
  ASSERT_NE(folder_id, -1);

  // Folder in DB but never synced → needs sync
  ASSERT_TRUE(sync_manager.NeedsSynchronization("notes", test_sync_root));

  db_manager.Close();
  cleanup_sync_test_dir();
  cleanup_test_db();
  std::cout << "  ✓ test_dbsync_needs_synchronization passed" << std::endl;
  return 0;
}

int test_dbsync_needs_sync_no_vxjson() {
  std::cout << "  Running test_dbsync_needs_sync_no_vxjson..." << std::endl;

  setup_test_db();
  setup_sync_test_dir();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());
  DbSyncManager sync_manager(db_manager.GetHandle(), &file_db);

  // Create a subfolder on disk without vx.json
  std::string missing_folder = test_sync_root + "/missing_folder";
  std::filesystem::create_directories(missing_folder);

  // SynchronizeFolder should return kJsonNotFound when no vx.json exists
  auto result = sync_manager.SynchronizeFolder("missing_folder", test_sync_root);
  ASSERT_EQ(static_cast<int>(result), static_cast<int>(SyncResult::kJsonNotFound));

  db_manager.Close();
  cleanup_sync_test_dir();
  cleanup_test_db();
  std::cout << "  ✓ test_dbsync_needs_sync_no_vxjson passed" << std::endl;
  return 0;
}

int test_dbsync_synchronize_folder() {
  std::cout << "  Running test_dbsync_synchronize_folder..." << std::endl;

  setup_test_db();
  setup_sync_test_dir();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());
  DbSyncManager sync_manager(db_manager.GetHandle(), &file_db);

  // Create vx.json with folder and files
  TestFolderConfig config;
  config.id = "sync-folder-uuid";
  config.name = "docs";
  config.created_utc = 1000;
  config.modified_utc = 2000;

  TestFileInfo file1;
  file1.id = "file-uuid-1";
  file1.name = "readme.md";
  file1.created_utc = 1100;
  file1.modified_utc = 2100;
  file1.tags = {"important", "documentation"};
  config.files.push_back(file1);

  TestFileInfo file2;
  file2.id = "file-uuid-2";
  file2.name = "guide.md";
  file2.created_utc = 1200;
  file2.modified_utc = 2200;
  file2.tags = {"documentation"};
  config.files.push_back(file2);

  ASSERT_TRUE(write_vx_json(test_sync_root, "docs", config));

  // Synchronize folder
  auto result = sync_manager.SynchronizeFolder("docs", test_sync_root);
  ASSERT_EQ(static_cast<int>(result), static_cast<int>(SyncResult::kSuccess));

  // Verify folder was created in DB
  auto folder = file_db.GetFolderByPath("docs");
  ASSERT_TRUE(folder.has_value());
  ASSERT_EQ(folder->uuid, "sync-folder-uuid");
  ASSERT_EQ(folder->name, "docs");

  // Verify files were created
  auto files = file_db.ListFiles(folder->id);
  ASSERT_EQ(files.size(), 2);

  // Verify tags were applied
  auto readme = file_db.GetFileByName(folder->id, "readme.md");
  ASSERT_TRUE(readme.has_value());
  ASSERT_EQ(readme->tags.size(), 2);

  // Verify sync state was updated
  auto sync_state = sync_manager.GetSyncState("docs");
  ASSERT_TRUE(sync_state.has_value());
  ASSERT_EQ(sync_state->folder_id, folder->id);
  ASSERT_NE(sync_state->last_sync_utc, -1);

  db_manager.Close();
  cleanup_sync_test_dir();
  cleanup_test_db();
  std::cout << "  ✓ test_dbsync_synchronize_folder passed" << std::endl;
  return 0;
}

int test_dbsync_rebuild_folder() {
  std::cout << "  Running test_dbsync_rebuild_folder..." << std::endl;

  setup_test_db();
  setup_sync_test_dir();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());
  DbSyncManager sync_manager(db_manager.GetHandle(), &file_db);

  // Create initial vx.json
  TestFolderConfig config;
  config.id = "rebuild-folder-uuid";
  config.name = "archive";
  config.created_utc = 1000;
  config.modified_utc = 2000;

  TestFileInfo file1;
  file1.id = "old-file-uuid";
  file1.name = "old.md";
  file1.created_utc = 1100;
  file1.modified_utc = 2100;
  config.files.push_back(file1);

  ASSERT_TRUE(write_vx_json(test_sync_root, "archive", config));

  // Initial sync
  auto result = sync_manager.SynchronizeFolder("archive", test_sync_root);
  ASSERT_EQ(static_cast<int>(result), static_cast<int>(SyncResult::kSuccess));

  // Verify initial state
  auto folder = file_db.GetFolderByPath("archive");
  ASSERT_TRUE(folder.has_value());
  auto files_before = file_db.ListFiles(folder->id);
  ASSERT_EQ(files_before.size(), 1);

  // Update vx.json with different content
  config.files.clear();
  TestFileInfo new_file;
  new_file.id = "new-file-uuid";
  new_file.name = "new.md";
  new_file.created_utc = 1300;
  new_file.modified_utc = 2300;
  config.files.push_back(new_file);
  ASSERT_TRUE(write_vx_json(test_sync_root, "archive", config));

  // Rebuild folder (forces re-import)
  result = sync_manager.RebuildFolder("archive", test_sync_root);
  ASSERT_EQ(static_cast<int>(result), static_cast<int>(SyncResult::kSuccess));

  // Verify folder still exists
  folder = file_db.GetFolderByPath("archive");
  ASSERT_TRUE(folder.has_value());

  // Verify files were replaced (rebuild deletes old data)
  auto files_after = file_db.ListFiles(folder->id);
  ASSERT_EQ(files_after.size(), 1);
  ASSERT_EQ(files_after[0].name, "new.md");

  db_manager.Close();
  cleanup_sync_test_dir();
  cleanup_test_db();
  std::cout << "  ✓ test_dbsync_rebuild_folder passed" << std::endl;
  return 0;
}

int test_dbsync_sync_state() {
  std::cout << "  Running test_dbsync_sync_state..." << std::endl;

  setup_test_db();
  setup_sync_test_dir();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());
  DbSyncManager sync_manager(db_manager.GetHandle(), &file_db);

  // Create vx.json
  TestFolderConfig config;
  config.id = "state-folder-uuid";
  config.name = "state_test";
  config.created_utc = 1000;
  config.modified_utc = 2000;
  ASSERT_TRUE(write_vx_json(test_sync_root, "state_test", config));

  // GetSyncState returns nullopt for non-existent folder
  auto state = sync_manager.GetSyncState("state_test");
  ASSERT_FALSE(state.has_value());

  // Sync folder
  auto result = sync_manager.SynchronizeFolder("state_test", test_sync_root);
  ASSERT_EQ(static_cast<int>(result), static_cast<int>(SyncResult::kSuccess));

  // GetSyncState returns valid state after sync
  state = sync_manager.GetSyncState("state_test");
  ASSERT_TRUE(state.has_value());
  ASSERT_NE(state->folder_id, -1);
  ASSERT_NE(state->last_sync_utc, -1);
  ASSERT_NE(state->metadata_file_modified_utc, -1);

  // ClearSyncState works
  ASSERT_TRUE(sync_manager.ClearSyncState("state_test"));

  // After clearing, GetSyncState returns nullopt
  state = sync_manager.GetSyncState("state_test");
  ASSERT_FALSE(state.has_value());

  // ClearSyncState on non-existent folder returns false
  ASSERT_FALSE(sync_manager.ClearSyncState("nonexistent"));

  db_manager.Close();
  cleanup_sync_test_dir();
  cleanup_test_db();
  std::cout << "  ✓ test_dbsync_sync_state passed" << std::endl;
  return 0;
}

// ============================================================================
// FileDb::GetFolderByPath Tests
// ============================================================================

int test_filedb_get_folder_by_path() {
  std::cout << "  Running test_filedb_get_folder_by_path..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder hierarchy: root -> sub -> leaf
  int64_t root_id = file_db.CreateFolder(-1, "root", 1000, 2000);
  ASSERT_NE(root_id, -1);

  int64_t sub_id = file_db.CreateFolder(root_id, "sub", 1100, 2100);
  ASSERT_NE(sub_id, -1);

  int64_t leaf_id = file_db.CreateFolder(sub_id, "leaf", 1200, 2200);
  ASSERT_NE(leaf_id, -1);

  // Find root folder by path "root"
  auto found_root = file_db.GetFolderByPath("root");
  ASSERT_TRUE(found_root.has_value());
  ASSERT_EQ(found_root->id, root_id);
  ASSERT_EQ(found_root->name, "root");

  // Find nested folder by path "root/sub"
  auto found_sub = file_db.GetFolderByPath("root/sub");
  ASSERT_TRUE(found_sub.has_value());
  ASSERT_EQ(found_sub->id, sub_id);
  ASSERT_EQ(found_sub->name, "sub");

  // Find deeply nested folder by path "root/sub/leaf"
  auto found_leaf = file_db.GetFolderByPath("root/sub/leaf");
  ASSERT_TRUE(found_leaf.has_value());
  ASSERT_EQ(found_leaf->id, leaf_id);
  ASSERT_EQ(found_leaf->name, "leaf");

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_get_folder_by_path passed" << std::endl;
  return 0;
}

int test_filedb_get_folder_by_path_not_found() {
  std::cout << "  Running test_filedb_get_folder_by_path_not_found..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create a single folder
  int64_t root_id = file_db.CreateFolder(-1, "root", 1000, 2000);
  ASSERT_NE(root_id, -1);

  // Path with non-existent first component
  auto not_found1 = file_db.GetFolderByPath("nonexistent");
  ASSERT_FALSE(not_found1.has_value());

  // Path with non-existent middle component
  auto not_found2 = file_db.GetFolderByPath("root/nonexistent/deep");
  ASSERT_FALSE(not_found2.has_value());

  // Path with non-existent leaf component
  auto not_found3 = file_db.GetFolderByPath("root/missing");
  ASSERT_FALSE(not_found3.has_value());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_get_folder_by_path_not_found passed" << std::endl;
  return 0;
}

int test_filedb_get_folder_by_path_edge_cases() {
  std::cout << "  Running test_filedb_get_folder_by_path_edge_cases..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create a folder to ensure DB isn't empty
  int64_t root_id = file_db.CreateFolder(-1, "root", 1000, 2000);
  ASSERT_NE(root_id, -1);

  // Empty path returns nullopt (root has no folder record)
  auto empty_path = file_db.GetFolderByPath("");
  ASSERT_FALSE(empty_path.has_value());

  // "." returns nullopt (represents root)
  auto dot_path = file_db.GetFolderByPath(".");
  ASSERT_FALSE(dot_path.has_value());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_get_folder_by_path_edge_cases passed" << std::endl;
  return 0;
}

// ============================================================================
// TagDb Query Tests
// ============================================================================

int test_tagdb_find_files_by_tags_and() {
  std::cout << "  Running test_tagdb_find_files_by_tags_and..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Create files with different tag combinations
  // file1: tag1, tag2
  // file2: tag1
  // file3: tag1, tag2, tag3
  std::vector<std::string> tags1 = {"tag1", "tag2"};
  std::vector<std::string> tags2 = {"tag1"};
  std::vector<std::string> tags3 = {"tag1", "tag2", "tag3"};

  int64_t file1_id = file_db.CreateFile(folder_id, "file1.md", 1100, 2100, tags1);
  int64_t file2_id = file_db.CreateFile(folder_id, "file2.md", 1200, 2200, tags2);
  int64_t file3_id = file_db.CreateFile(folder_id, "file3.md", 1300, 2300, tags3);
  ASSERT_NE(file1_id, -1);
  ASSERT_NE(file2_id, -1);
  ASSERT_NE(file3_id, -1);

  // Find files with ALL of tag1 AND tag2 (should be file1 and file3)
  auto results = tag_db.FindFilesByTagsAnd({"tag1", "tag2"});
  ASSERT_EQ(results.size(), 2);

  // Find files with ALL of tag1, tag2, tag3 (should be only file3)
  auto results2 = tag_db.FindFilesByTagsAnd({"tag1", "tag2", "tag3"});
  ASSERT_EQ(results2.size(), 1);
  ASSERT_EQ(results2[0].file_name, "file3.md");

  // Find files with tag that no file has (empty result)
  auto results3 = tag_db.FindFilesByTagsAnd({"nonexistent"});
  ASSERT_EQ(results3.size(), 0);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_find_files_by_tags_and passed" << std::endl;
  return 0;
}

int test_tagdb_find_files_by_tags_or() {
  std::cout << "  Running test_tagdb_find_files_by_tags_or..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Create files with different tags
  // file1: tagA
  // file2: tagB
  // file3: tagC
  std::vector<std::string> tagsA = {"tagA"};
  std::vector<std::string> tagsB = {"tagB"};
  std::vector<std::string> tagsC = {"tagC"};

  int64_t file1_id = file_db.CreateFile(folder_id, "file1.md", 1100, 2100, tagsA);
  int64_t file2_id = file_db.CreateFile(folder_id, "file2.md", 1200, 2200, tagsB);
  int64_t file3_id = file_db.CreateFile(folder_id, "file3.md", 1300, 2300, tagsC);
  ASSERT_NE(file1_id, -1);
  ASSERT_NE(file2_id, -1);
  ASSERT_NE(file3_id, -1);

  // Find files with ANY of tagA OR tagB (should be file1 and file2)
  auto results = tag_db.FindFilesByTagsOr({"tagA", "tagB"});
  ASSERT_EQ(results.size(), 2);

  // Find files with ANY of tagA, tagB, tagC (should be all 3)
  auto results2 = tag_db.FindFilesByTagsOr({"tagA", "tagB", "tagC"});
  ASSERT_EQ(results2.size(), 3);

  // Find files with only tagC (should be only file3)
  auto results3 = tag_db.FindFilesByTagsOr({"tagC"});
  ASSERT_EQ(results3.size(), 1);
  ASSERT_EQ(results3[0].file_name, "file3.md");

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_find_files_by_tags_or passed" << std::endl;
  return 0;
}

int test_tagdb_count_files_by_tag() {
  std::cout << "  Running test_tagdb_count_files_by_tag..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  ASSERT_NE(folder_id, -1);

  // Create files with overlapping tags
  // file1: common, rare
  // file2: common
  // file3: common
  std::vector<std::string> tags1 = {"common", "rare"};
  std::vector<std::string> tags2 = {"common"};
  std::vector<std::string> tags3 = {"common"};

  int64_t file1_id = file_db.CreateFile(folder_id, "file1.md", 1100, 2100, tags1);
  int64_t file2_id = file_db.CreateFile(folder_id, "file2.md", 1200, 2200, tags2);
  int64_t file3_id = file_db.CreateFile(folder_id, "file3.md", 1300, 2300, tags3);
  ASSERT_NE(file1_id, -1);
  ASSERT_NE(file2_id, -1);
  ASSERT_NE(file3_id, -1);

  // Count files by tag (sorted by count DESC, then name ASC)
  auto counts = tag_db.CountFilesByTag();
  ASSERT_EQ(counts.size(), 2);  // "common" and "rare"

  // First should be "common" with count 3
  ASSERT_EQ(counts[0].first, "common");
  ASSERT_EQ(counts[0].second, 3);

  // Second should be "rare" with count 1
  ASSERT_EQ(counts[1].first, "rare");
  ASSERT_EQ(counts[1].second, 1);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_count_files_by_tag passed" << std::endl;
  return 0;
}

// ============================================================================
// TagDb CRUD Tests
// ============================================================================

int test_tagdb_get_tag_by_id() {
  std::cout << "  Running test_tagdb_get_tag_by_id..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());

  // Create a tag
  int64_t tag_id = tag_db.CreateOrUpdateTag("mytag", -1, "{\"color\": \"red\"}");
  ASSERT_NE(tag_id, -1);

  // Get tag by ID
  auto tag = tag_db.GetTagById(tag_id);
  ASSERT_TRUE(tag.has_value());
  ASSERT_EQ(tag->id, tag_id);
  ASSERT_EQ(tag->name, "mytag");
  ASSERT_EQ(tag->parent_id, -1);
  ASSERT_EQ(tag->metadata, "{\"color\": \"red\"}");

  // Get non-existent tag
  auto not_found = tag_db.GetTagById(9999);
  ASSERT_FALSE(not_found.has_value());

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_get_tag_by_id passed" << std::endl;
  return 0;
}

int test_tagdb_delete_tag() {
  std::cout << "  Running test_tagdb_delete_tag..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder and file with tag
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  std::vector<std::string> tags = {"deleteme"};
  int64_t file_id = file_db.CreateFile(folder_id, "test.md", 1100, 2100, tags);
  ASSERT_NE(file_id, -1);

  // Get tag ID
  auto tag = tag_db.GetTag("deleteme");
  ASSERT_TRUE(tag.has_value());
  int64_t tag_id = tag->id;

  // Verify file has the tag
  auto file_tags = file_db.GetFileTags(file_id);
  ASSERT_EQ(file_tags.size(), 1);
  ASSERT_EQ(file_tags[0], "deleteme");

  // Delete tag
  ASSERT_TRUE(tag_db.DeleteTag(tag_id));

  // Verify tag is deleted
  auto deleted_tag = tag_db.GetTagById(tag_id);
  ASSERT_FALSE(deleted_tag.has_value());

  // Verify file no longer has the tag (cascade delete)
  auto file_tags_after = file_db.GetFileTags(file_id);
  ASSERT_EQ(file_tags_after.size(), 0);

  // Delete non-existent tag succeeds (idempotent delete, consistent with
  // DeleteFolder/DeleteFile which return true as long as DELETE statement executes)
  ASSERT_TRUE(tag_db.DeleteTag(9999));

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_tagdb_delete_tag passed" << std::endl;
  return 0;
}

// ============================================================================
// Edge Case Tests
// ============================================================================

int test_filedb_move_folder_deep_cycle_detection() {
  std::cout << "  Running test_filedb_move_folder_deep_cycle_detection..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create deep folder hierarchy: A -> B -> C -> D
  int64_t a_id = file_db.CreateFolder(-1, "A", 1000, 2000);
  int64_t b_id = file_db.CreateFolder(a_id, "B", 1100, 2100);
  int64_t c_id = file_db.CreateFolder(b_id, "C", 1200, 2200);
  int64_t d_id = file_db.CreateFolder(c_id, "D", 1300, 2300);
  ASSERT_NE(a_id, -1);
  ASSERT_NE(b_id, -1);
  ASSERT_NE(c_id, -1);
  ASSERT_NE(d_id, -1);

  // Verify hierarchy
  ASSERT_EQ(file_db.GetFolderPath(d_id), "A/B/C/D");

  // Cannot move A under its deep descendant D (would create cycle)
  ASSERT_FALSE(file_db.MoveFolder(a_id, d_id));

  // Cannot move B under D (D is descendant of B via C)
  ASSERT_FALSE(file_db.MoveFolder(b_id, d_id));

  // Cannot move C under D (D is direct child of C)
  ASSERT_FALSE(file_db.MoveFolder(c_id, d_id));

  // Can move D to root (no cycle)
  ASSERT_TRUE(file_db.MoveFolder(d_id, -1));

  // Verify D is now at root
  auto d = file_db.GetFolder(d_id);
  ASSERT_TRUE(d.has_value());
  ASSERT_EQ(d->parent_id, -1);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_move_folder_deep_cycle_detection passed" << std::endl;
  return 0;
}

int test_filedb_set_file_tags_behavior() {
  std::cout << "  Running test_filedb_set_file_tags_behavior..." << std::endl;

  setup_test_db();

  DbManager db_manager;
  ASSERT_TRUE(db_manager.Open(test_db_path));
  ASSERT_TRUE(db_manager.InitializeSchema());

  TagDb tag_db(db_manager.GetHandle());
  FileDb file_db(db_manager.GetHandle());

  // Create folder and file with initial tags
  int64_t folder_id = file_db.CreateFolder(-1, "folder", 1000, 2000);
  std::vector<std::string> initial_tags = {"tag1", "tag2"};
  int64_t file_id = file_db.CreateFile(folder_id, "test.md", 1100, 2100, initial_tags);
  ASSERT_NE(file_id, -1);

  // Verify initial tags
  auto tags_before = file_db.GetFileTags(file_id);
  ASSERT_EQ(tags_before.size(), 2);

  // SetFileTags with new tags (should replace, not append)
  // Note: SetFileTags deletes existing tags first, so partial failure results in no tags
  std::vector<std::string> new_tags = {"tag3", "tag4"};
  ASSERT_TRUE(file_db.SetFileTags(file_id, new_tags));

  // Verify old tags are replaced with new tags
  auto tags_after = file_db.GetFileTags(file_id);
  ASSERT_EQ(tags_after.size(), 2);

  // Verify new tags are present (order may vary)
  bool has_tag3 = false;
  bool has_tag4 = false;
  for (const auto& tag : tags_after) {
    if (tag == "tag3") has_tag3 = true;
    if (tag == "tag4") has_tag4 = true;
  }
  ASSERT_TRUE(has_tag3);
  ASSERT_TRUE(has_tag4);

  // SetFileTags with empty list clears all tags
  std::vector<std::string> empty_tags;
  ASSERT_TRUE(file_db.SetFileTags(file_id, empty_tags));

  auto tags_cleared = file_db.GetFileTags(file_id);
  ASSERT_EQ(tags_cleared.size(), 0);

  db_manager.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_filedb_set_file_tags_behavior passed" << std::endl;
  return 0;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "Running DB layer tests..." << std::endl;

  // DbManager tests
  RUN_TEST(test_db_manager_create);
  RUN_TEST(test_db_manager_open_close);
  RUN_TEST(test_db_manager_initialize_schema);
  RUN_TEST(test_db_manager_transactions);
  RUN_TEST(test_db_manager_rebuild);

  // FileDb - Folder tests
  RUN_TEST(test_filedb_create_folder);
  RUN_TEST(test_filedb_create_or_update_folder);
  RUN_TEST(test_filedb_get_folder_path);
  RUN_TEST(test_filedb_list_folders);
  RUN_TEST(test_filedb_update_delete_folder);
  RUN_TEST(test_filedb_move_folder);

  // FileDb - File tests
  RUN_TEST(test_filedb_create_file);
  RUN_TEST(test_filedb_create_or_update_file);
  RUN_TEST(test_filedb_get_file_by_name);
  RUN_TEST(test_filedb_list_files);
  RUN_TEST(test_filedb_update_delete_file);
  RUN_TEST(test_filedb_move_file);

  // FileDb - Tag tests
  RUN_TEST(test_tagdb_create_tag);
  RUN_TEST(test_tagdb_get_or_create_tag);
  RUN_TEST(test_filedb_add_tag_to_file);
  RUN_TEST(test_tagdb_list_tags);

  // DbSyncManager tests
  RUN_TEST(test_dbsync_needs_synchronization);
  RUN_TEST(test_dbsync_needs_sync_no_vxjson);
  RUN_TEST(test_dbsync_synchronize_folder);
  RUN_TEST(test_dbsync_rebuild_folder);
  RUN_TEST(test_dbsync_sync_state);

  // FileDb - GetFolderByPath tests
  RUN_TEST(test_filedb_get_folder_by_path);
  RUN_TEST(test_filedb_get_folder_by_path_not_found);
  RUN_TEST(test_filedb_get_folder_by_path_edge_cases);

  // TagDb - Query tests
  RUN_TEST(test_tagdb_find_files_by_tags_and);
  RUN_TEST(test_tagdb_find_files_by_tags_or);
  RUN_TEST(test_tagdb_count_files_by_tag);

  // TagDb - CRUD tests
  RUN_TEST(test_tagdb_get_tag_by_id);
  RUN_TEST(test_tagdb_delete_tag);

  // Edge case tests
  RUN_TEST(test_filedb_move_folder_deep_cycle_detection);
  RUN_TEST(test_filedb_set_file_tags_behavior);

  std::cout << "✓ All DB layer tests passed" << std::endl;
  return 0;
}
