#include <filesystem>
#include <iostream>

#include "db/db_manager.h"
#include "db/file_db.h"
#include "db/tag_db.h"
#include "test_utils.h"

// Undefine Windows macros that conflict with our method names
#ifdef CreateFile
#undef CreateFile
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef MoveFile
#undef MoveFile
#endif

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

  std::cout << "✓ All DB layer tests passed" << std::endl;
  return 0;
}
