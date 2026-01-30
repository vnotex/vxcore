#include <filesystem>
#include <fstream>
#include <iostream>

#include "core/metadata_store.h"
#include "db/sqlite_metadata_store.h"
#include "test_utils.h"

using namespace vxcore;
using namespace vxcore::db;

// Test database path
static std::string test_db_path;

// Helper to setup test database
void setup_test_db() {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  test_db_path = (temp_dir / "vxcore_metadata_store_test.sqlite").string();

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
// Lifecycle Tests
// ============================================================================

int test_metadata_store_open_close() {
  std::cout << "  Running test_metadata_store_open_close..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_FALSE(store.IsOpen());

  ASSERT_TRUE(store.Open(test_db_path));
  ASSERT_TRUE(store.IsOpen());

  store.Close();
  ASSERT_FALSE(store.IsOpen());

  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_open_close passed" << std::endl;
  return 0;
}

int test_metadata_store_reopen() {
  std::cout << "  Running test_metadata_store_reopen..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create some data
  StoreFolderRecord folder;
  folder.id = "folder-uuid-1";
  folder.parent_id = "";
  folder.name = "test_folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  store.Close();

  // Reopen and verify data persists
  ASSERT_TRUE(store.Open(test_db_path));

  auto retrieved = store.GetFolder("folder-uuid-1");
  ASSERT_TRUE(retrieved.has_value());
  ASSERT_EQ(retrieved->name, "test_folder");

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_reopen passed" << std::endl;
  return 0;
}

// ============================================================================
// Transaction Tests
// ============================================================================

int test_metadata_store_transactions() {
  std::cout << "  Running test_metadata_store_transactions..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Test commit
  ASSERT_TRUE(store.BeginTransaction());

  StoreFolderRecord folder;
  folder.id = "tx-folder-1";
  folder.parent_id = "";
  folder.name = "committed_folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  ASSERT_TRUE(store.CommitTransaction());

  // Verify committed
  auto committed = store.GetFolder("tx-folder-1");
  ASSERT_TRUE(committed.has_value());

  // Test rollback
  ASSERT_TRUE(store.BeginTransaction());

  StoreFolderRecord folder2;
  folder2.id = "tx-folder-2";
  folder2.parent_id = "";
  folder2.name = "rolled_back_folder";
  folder2.created_utc = 1000;
  folder2.modified_utc = 2000;
  folder2.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder2));

  ASSERT_TRUE(store.RollbackTransaction());

  // Verify rolled back
  auto rolled_back = store.GetFolder("tx-folder-2");
  ASSERT_FALSE(rolled_back.has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_transactions passed" << std::endl;
  return 0;
}

// ============================================================================
// Folder CRUD Tests
// ============================================================================

int test_metadata_store_create_folder() {
  std::cout << "  Running test_metadata_store_create_folder..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  StoreFolderRecord folder;
  folder.id = "folder-uuid-abc";
  folder.parent_id = "";  // Root folder
  folder.name = "documents";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{\"color\": \"blue\"}";

  ASSERT_TRUE(store.CreateFolder(folder));

  // Retrieve and verify
  auto retrieved = store.GetFolder("folder-uuid-abc");
  ASSERT_TRUE(retrieved.has_value());
  ASSERT_EQ(retrieved->id, "folder-uuid-abc");
  ASSERT_EQ(retrieved->name, "documents");
  ASSERT_EQ(retrieved->parent_id, "");
  ASSERT_EQ(retrieved->created_utc, 1000);
  ASSERT_EQ(retrieved->modified_utc, 2000);
  ASSERT_EQ(retrieved->metadata, "{\"color\": \"blue\"}");

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_create_folder passed" << std::endl;
  return 0;
}

int test_metadata_store_nested_folders() {
  std::cout << "  Running test_metadata_store_nested_folders..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create root folder
  StoreFolderRecord root;
  root.id = "root-uuid";
  root.parent_id = "";
  root.name = "root";
  root.created_utc = 1000;
  root.modified_utc = 2000;
  root.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(root));

  // Create child folder
  StoreFolderRecord child;
  child.id = "child-uuid";
  child.parent_id = "root-uuid";
  child.name = "child";
  child.created_utc = 1100;
  child.modified_utc = 2100;
  child.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(child));

  // Create grandchild folder
  StoreFolderRecord grandchild;
  grandchild.id = "grandchild-uuid";
  grandchild.parent_id = "child-uuid";
  grandchild.name = "grandchild";
  grandchild.created_utc = 1200;
  grandchild.modified_utc = 2200;
  grandchild.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(grandchild));

  // Verify hierarchy via GetFolder
  auto retrieved_child = store.GetFolder("child-uuid");
  ASSERT_TRUE(retrieved_child.has_value());
  ASSERT_EQ(retrieved_child->parent_id, "root-uuid");

  auto retrieved_grandchild = store.GetFolder("grandchild-uuid");
  ASSERT_TRUE(retrieved_grandchild.has_value());
  ASSERT_EQ(retrieved_grandchild->parent_id, "child-uuid");

  // Verify ListFolders
  auto root_children = store.ListFolders("root-uuid");
  ASSERT_EQ(root_children.size(), 1);
  ASSERT_EQ(root_children[0].name, "child");

  auto child_children = store.ListFolders("child-uuid");
  ASSERT_EQ(child_children.size(), 1);
  ASSERT_EQ(child_children[0].name, "grandchild");

  // Verify GetFolderPath
  std::string grandchild_path = store.GetFolderPath("grandchild-uuid");
  ASSERT_EQ(grandchild_path, "root/child/grandchild");

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_nested_folders passed" << std::endl;
  return 0;
}

int test_metadata_store_update_folder() {
  std::cout << "  Running test_metadata_store_update_folder..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder
  StoreFolderRecord folder;
  folder.id = "update-folder-uuid";
  folder.parent_id = "";
  folder.name = "original_name";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{\"version\": 1}";
  ASSERT_TRUE(store.CreateFolder(folder));

  // Update folder
  ASSERT_TRUE(store.UpdateFolder("update-folder-uuid", "new_name", 3000, "{\"version\": 2}"));

  // Verify update
  auto updated = store.GetFolder("update-folder-uuid");
  ASSERT_TRUE(updated.has_value());
  ASSERT_EQ(updated->name, "new_name");
  ASSERT_EQ(updated->modified_utc, 3000);
  ASSERT_EQ(updated->metadata, "{\"version\": 2}");
  // created_utc should be preserved
  ASSERT_EQ(updated->created_utc, 1000);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_update_folder passed" << std::endl;
  return 0;
}

int test_metadata_store_delete_folder() {
  std::cout << "  Running test_metadata_store_delete_folder..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder
  StoreFolderRecord folder;
  folder.id = "delete-folder-uuid";
  folder.parent_id = "";
  folder.name = "to_delete";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  // Verify exists
  ASSERT_TRUE(store.GetFolder("delete-folder-uuid").has_value());

  // Delete
  ASSERT_TRUE(store.DeleteFolder("delete-folder-uuid"));

  // Verify deleted
  ASSERT_FALSE(store.GetFolder("delete-folder-uuid").has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_delete_folder passed" << std::endl;
  return 0;
}

int test_metadata_store_move_folder() {
  std::cout << "  Running test_metadata_store_move_folder..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create parent folders
  StoreFolderRecord parent1;
  parent1.id = "parent1-uuid";
  parent1.parent_id = "";
  parent1.name = "parent1";
  parent1.created_utc = 1000;
  parent1.modified_utc = 2000;
  parent1.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(parent1));

  StoreFolderRecord parent2;
  parent2.id = "parent2-uuid";
  parent2.parent_id = "";
  parent2.name = "parent2";
  parent2.created_utc = 1000;
  parent2.modified_utc = 2000;
  parent2.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(parent2));

  // Create child under parent1
  StoreFolderRecord child;
  child.id = "movable-child-uuid";
  child.parent_id = "parent1-uuid";
  child.name = "child";
  child.created_utc = 1100;
  child.modified_utc = 2100;
  child.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(child));

  // Verify initial parent
  auto before_move = store.GetFolder("movable-child-uuid");
  ASSERT_TRUE(before_move.has_value());
  ASSERT_EQ(before_move->parent_id, "parent1-uuid");

  // Move to parent2
  ASSERT_TRUE(store.MoveFolder("movable-child-uuid", "parent2-uuid"));

  // Verify new parent
  auto after_move = store.GetFolder("movable-child-uuid");
  ASSERT_TRUE(after_move.has_value());
  ASSERT_EQ(after_move->parent_id, "parent2-uuid");

  // Verify path updated
  std::string new_path = store.GetFolderPath("movable-child-uuid");
  ASSERT_EQ(new_path, "parent2/child");

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_move_folder passed" << std::endl;
  return 0;
}

int test_metadata_store_get_folder_by_path() {
  std::cout << "  Running test_metadata_store_get_folder_by_path..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder hierarchy
  StoreFolderRecord root;
  root.id = "path-root-uuid";
  root.parent_id = "";
  root.name = "docs";
  root.created_utc = 1000;
  root.modified_utc = 2000;
  root.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(root));

  StoreFolderRecord sub;
  sub.id = "path-sub-uuid";
  sub.parent_id = "path-root-uuid";
  sub.name = "guides";
  sub.created_utc = 1100;
  sub.modified_utc = 2100;
  sub.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(sub));

  // Find by path
  auto found = store.GetFolderByPath("docs/guides");
  ASSERT_TRUE(found.has_value());
  ASSERT_EQ(found->id, "path-sub-uuid");
  ASSERT_EQ(found->name, "guides");

  // Not found
  auto not_found = store.GetFolderByPath("docs/nonexistent");
  ASSERT_FALSE(not_found.has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_get_folder_by_path passed" << std::endl;
  return 0;
}

// ============================================================================
// File CRUD Tests
// ============================================================================

int test_metadata_store_create_file() {
  std::cout << "  Running test_metadata_store_create_file..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create parent folder first
  StoreFolderRecord folder;
  folder.id = "file-parent-uuid";
  folder.parent_id = "";
  folder.name = "notes";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  // Create file
  StoreFileRecord file;
  file.id = "file-uuid-123";
  file.folder_id = "file-parent-uuid";
  file.name = "readme.md";
  file.created_utc = 1100;
  file.modified_utc = 2100;
  file.metadata = "{\"author\": \"test\"}";
  file.tags = {"important", "documentation"};
  ASSERT_TRUE(store.CreateFile(file));

  // Retrieve and verify
  auto retrieved = store.GetFile("file-uuid-123");
  ASSERT_TRUE(retrieved.has_value());
  ASSERT_EQ(retrieved->id, "file-uuid-123");
  ASSERT_EQ(retrieved->folder_id, "file-parent-uuid");
  ASSERT_EQ(retrieved->name, "readme.md");
  ASSERT_EQ(retrieved->created_utc, 1100);
  ASSERT_EQ(retrieved->modified_utc, 2100);
  ASSERT_EQ(retrieved->metadata, "{\"author\": \"test\"}");
  ASSERT_EQ(retrieved->tags.size(), 2);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_create_file passed" << std::endl;
  return 0;
}

int test_metadata_store_update_file() {
  std::cout << "  Running test_metadata_store_update_file..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder and file
  StoreFolderRecord folder;
  folder.id = "update-file-folder-uuid";
  folder.parent_id = "";
  folder.name = "folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  StoreFileRecord file;
  file.id = "update-file-uuid";
  file.folder_id = "update-file-folder-uuid";
  file.name = "original.md";
  file.created_utc = 1100;
  file.modified_utc = 2100;
  file.metadata = "{\"v\": 1}";
  file.tags = {};
  ASSERT_TRUE(store.CreateFile(file));

  // Update file
  ASSERT_TRUE(store.UpdateFile("update-file-uuid", "renamed.md", 3100, "{\"v\": 2}"));

  // Verify update
  auto updated = store.GetFile("update-file-uuid");
  ASSERT_TRUE(updated.has_value());
  ASSERT_EQ(updated->name, "renamed.md");
  ASSERT_EQ(updated->modified_utc, 3100);
  ASSERT_EQ(updated->metadata, "{\"v\": 2}");
  // created_utc preserved
  ASSERT_EQ(updated->created_utc, 1100);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_update_file passed" << std::endl;
  return 0;
}

int test_metadata_store_delete_file() {
  std::cout << "  Running test_metadata_store_delete_file..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder and file
  StoreFolderRecord folder;
  folder.id = "delete-file-folder-uuid";
  folder.parent_id = "";
  folder.name = "folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  StoreFileRecord file;
  file.id = "delete-file-uuid";
  file.folder_id = "delete-file-folder-uuid";
  file.name = "to_delete.md";
  file.created_utc = 1100;
  file.modified_utc = 2100;
  file.metadata = "{}";
  file.tags = {};
  ASSERT_TRUE(store.CreateFile(file));

  // Verify exists
  ASSERT_TRUE(store.GetFile("delete-file-uuid").has_value());

  // Delete
  ASSERT_TRUE(store.DeleteFile("delete-file-uuid"));

  // Verify deleted
  ASSERT_FALSE(store.GetFile("delete-file-uuid").has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_delete_file passed" << std::endl;
  return 0;
}

int test_metadata_store_move_file() {
  std::cout << "  Running test_metadata_store_move_file..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create two folders
  StoreFolderRecord folder1;
  folder1.id = "move-file-folder1-uuid";
  folder1.parent_id = "";
  folder1.name = "folder1";
  folder1.created_utc = 1000;
  folder1.modified_utc = 2000;
  folder1.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder1));

  StoreFolderRecord folder2;
  folder2.id = "move-file-folder2-uuid";
  folder2.parent_id = "";
  folder2.name = "folder2";
  folder2.created_utc = 1000;
  folder2.modified_utc = 2000;
  folder2.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder2));

  // Create file in folder1
  StoreFileRecord file;
  file.id = "move-file-uuid";
  file.folder_id = "move-file-folder1-uuid";
  file.name = "movable.md";
  file.created_utc = 1100;
  file.modified_utc = 2100;
  file.metadata = "{}";
  file.tags = {"tag1"};
  ASSERT_TRUE(store.CreateFile(file));

  // Verify initial folder
  auto before = store.GetFile("move-file-uuid");
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(before->folder_id, "move-file-folder1-uuid");

  // Move to folder2
  ASSERT_TRUE(store.MoveFile("move-file-uuid", "move-file-folder2-uuid"));

  // Verify new folder
  auto after = store.GetFile("move-file-uuid");
  ASSERT_TRUE(after.has_value());
  ASSERT_EQ(after->folder_id, "move-file-folder2-uuid");

  // Verify tags preserved
  ASSERT_EQ(after->tags.size(), 1);
  ASSERT_EQ(after->tags[0], "tag1");

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_move_file passed" << std::endl;
  return 0;
}

int test_metadata_store_list_files() {
  std::cout << "  Running test_metadata_store_list_files..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder
  StoreFolderRecord folder;
  folder.id = "list-files-folder-uuid";
  folder.parent_id = "";
  folder.name = "folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  // Create multiple files
  for (int i = 1; i <= 3; i++) {
    StoreFileRecord file;
    file.id = "list-file-" + std::to_string(i);
    file.folder_id = "list-files-folder-uuid";
    file.name = "file" + std::to_string(i) + ".md";
    file.created_utc = 1000 + i * 100;
    file.modified_utc = 2000 + i * 100;
    file.metadata = "{}";
    file.tags = {};
    ASSERT_TRUE(store.CreateFile(file));
  }

  // List files
  auto files = store.ListFiles("list-files-folder-uuid");
  ASSERT_EQ(files.size(), 3);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_list_files passed" << std::endl;
  return 0;
}

int test_metadata_store_get_file_by_path() {
  std::cout << "  Running test_metadata_store_get_file_by_path..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder hierarchy
  StoreFolderRecord root;
  root.id = "file-path-root-uuid";
  root.parent_id = "";
  root.name = "docs";
  root.created_utc = 1000;
  root.modified_utc = 2000;
  root.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(root));

  StoreFolderRecord sub;
  sub.id = "file-path-sub-uuid";
  sub.parent_id = "file-path-root-uuid";
  sub.name = "guides";
  sub.created_utc = 1100;
  sub.modified_utc = 2100;
  sub.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(sub));

  // Create file
  StoreFileRecord file;
  file.id = "file-by-path-uuid";
  file.folder_id = "file-path-sub-uuid";
  file.name = "setup.md";
  file.created_utc = 1200;
  file.modified_utc = 2200;
  file.metadata = "{}";
  file.tags = {};
  ASSERT_TRUE(store.CreateFile(file));

  // Find by path
  auto found = store.GetFileByPath("docs/guides/setup.md");
  ASSERT_TRUE(found.has_value());
  ASSERT_EQ(found->id, "file-by-path-uuid");
  ASSERT_EQ(found->name, "setup.md");

  // Not found
  auto not_found = store.GetFileByPath("docs/guides/nonexistent.md");
  ASSERT_FALSE(not_found.has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_get_file_by_path passed" << std::endl;
  return 0;
}

// ============================================================================
// Tag Tests
// ============================================================================

int test_metadata_store_file_tags() {
  std::cout << "  Running test_metadata_store_file_tags..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder and file
  StoreFolderRecord folder;
  folder.id = "tag-test-folder-uuid";
  folder.parent_id = "";
  folder.name = "folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  StoreFileRecord file;
  file.id = "tag-test-file-uuid";
  file.folder_id = "tag-test-folder-uuid";
  file.name = "tagged.md";
  file.created_utc = 1100;
  file.modified_utc = 2100;
  file.metadata = "{}";
  file.tags = {};
  ASSERT_TRUE(store.CreateFile(file));

  // Add tag
  ASSERT_TRUE(store.AddTagToFile("tag-test-file-uuid", "important"));

  auto tags1 = store.GetFileTags("tag-test-file-uuid");
  ASSERT_EQ(tags1.size(), 1);
  ASSERT_EQ(tags1[0], "important");

  // Add another tag
  ASSERT_TRUE(store.AddTagToFile("tag-test-file-uuid", "work"));

  auto tags2 = store.GetFileTags("tag-test-file-uuid");
  ASSERT_EQ(tags2.size(), 2);

  // Remove tag
  ASSERT_TRUE(store.RemoveTagFromFile("tag-test-file-uuid", "important"));

  auto tags3 = store.GetFileTags("tag-test-file-uuid");
  ASSERT_EQ(tags3.size(), 1);
  ASSERT_EQ(tags3[0], "work");

  // SetFileTags (replace all)
  std::vector<std::string> new_tags = {"tag1", "tag2", "tag3"};
  ASSERT_TRUE(store.SetFileTags("tag-test-file-uuid", new_tags));

  auto tags4 = store.GetFileTags("tag-test-file-uuid");
  ASSERT_EQ(tags4.size(), 3);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_file_tags passed" << std::endl;
  return 0;
}

int test_metadata_store_find_files_by_tags() {
  std::cout << "  Running test_metadata_store_find_files_by_tags..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder
  StoreFolderRecord folder;
  folder.id = "find-tag-folder-uuid";
  folder.parent_id = "";
  folder.name = "folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  // Create files with different tags
  // file1: tagA, tagB
  // file2: tagA
  // file3: tagB, tagC
  StoreFileRecord file1;
  file1.id = "find-file-1";
  file1.folder_id = "find-tag-folder-uuid";
  file1.name = "file1.md";
  file1.created_utc = 1100;
  file1.modified_utc = 2100;
  file1.metadata = "{}";
  file1.tags = {"tagA", "tagB"};
  ASSERT_TRUE(store.CreateFile(file1));

  StoreFileRecord file2;
  file2.id = "find-file-2";
  file2.folder_id = "find-tag-folder-uuid";
  file2.name = "file2.md";
  file2.created_utc = 1200;
  file2.modified_utc = 2200;
  file2.metadata = "{}";
  file2.tags = {"tagA"};
  ASSERT_TRUE(store.CreateFile(file2));

  StoreFileRecord file3;
  file3.id = "find-file-3";
  file3.folder_id = "find-tag-folder-uuid";
  file3.name = "file3.md";
  file3.created_utc = 1300;
  file3.modified_utc = 2300;
  file3.metadata = "{}";
  file3.tags = {"tagB", "tagC"};
  ASSERT_TRUE(store.CreateFile(file3));

  // FindFilesByTagsOr: files with ANY of tagA, tagC (should be all 3)
  auto or_results = store.FindFilesByTagsOr({"tagA", "tagC"});
  ASSERT_EQ(or_results.size(), 3);

  // FindFilesByTagsAnd: files with ALL of tagA, tagB (should be file1 only)
  auto and_results = store.FindFilesByTagsAnd({"tagA", "tagB"});
  ASSERT_EQ(and_results.size(), 1);
  ASSERT_EQ(and_results[0].file_name, "file1.md");

  // CountFilesByTag
  auto counts = store.CountFilesByTag();
  ASSERT_TRUE(counts.size() >= 3);  // At least tagA, tagB, tagC

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_find_files_by_tags passed" << std::endl;
  return 0;
}

// ============================================================================
// Tag Definition Tests
// ============================================================================

int test_metadata_store_tag_definitions() {
  std::cout << "  Running test_metadata_store_tag_definitions..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create tag
  StoreTagRecord tag;
  tag.name = "category";
  tag.parent_name = "";
  tag.metadata = "{\"color\": \"red\"}";
  ASSERT_TRUE(store.CreateOrUpdateTag(tag));

  // Retrieve tag
  auto retrieved = store.GetTag("category");
  ASSERT_TRUE(retrieved.has_value());
  ASSERT_EQ(retrieved->name, "category");
  ASSERT_EQ(retrieved->metadata, "{\"color\": \"red\"}");

  // Create child tag
  StoreTagRecord child_tag;
  child_tag.name = "work";
  child_tag.parent_name = "category";
  child_tag.metadata = "{}";
  ASSERT_TRUE(store.CreateOrUpdateTag(child_tag));

  // List tags
  auto tags = store.ListTags();
  ASSERT_EQ(tags.size(), 2);

  // Delete tag
  ASSERT_TRUE(store.DeleteTag("work"));

  tags = store.ListTags();
  ASSERT_EQ(tags.size(), 1);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_tag_definitions passed" << std::endl;
  return 0;
}

// ============================================================================
// RebuildAll Test
// ============================================================================

int test_metadata_store_rebuild_all() {
  std::cout << "  Running test_metadata_store_rebuild_all..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create some data
  StoreFolderRecord folder;
  folder.id = "rebuild-folder-uuid";
  folder.parent_id = "";
  folder.name = "folder";
  folder.created_utc = 1000;
  folder.modified_utc = 2000;
  folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(folder));

  StoreFileRecord file;
  file.id = "rebuild-file-uuid";
  file.folder_id = "rebuild-folder-uuid";
  file.name = "file.md";
  file.created_utc = 1100;
  file.modified_utc = 2100;
  file.metadata = "{}";
  file.tags = {"tag1"};
  ASSERT_TRUE(store.CreateFile(file));

  // Verify data exists
  ASSERT_TRUE(store.GetFolder("rebuild-folder-uuid").has_value());
  ASSERT_TRUE(store.GetFile("rebuild-file-uuid").has_value());

  // Rebuild (clears all data)
  ASSERT_TRUE(store.RebuildAll());

  // Verify data is gone
  ASSERT_FALSE(store.GetFolder("rebuild-folder-uuid").has_value());
  ASSERT_FALSE(store.GetFile("rebuild-file-uuid").has_value());

  // Verify we can create new data after rebuild
  StoreFolderRecord new_folder;
  new_folder.id = "new-folder-uuid";
  new_folder.parent_id = "";
  new_folder.name = "new_folder";
  new_folder.created_utc = 3000;
  new_folder.modified_utc = 4000;
  new_folder.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(new_folder));

  ASSERT_TRUE(store.GetFolder("new-folder-uuid").has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_rebuild_all passed" << std::endl;
  return 0;
}

// ============================================================================
// IterateAllFiles Test
// ============================================================================

int test_metadata_store_iterate_all_files() {
  std::cout << "  Running test_metadata_store_iterate_all_files..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Create folder hierarchy
  StoreFolderRecord root;
  root.id = "iter-root-uuid";
  root.parent_id = "";
  root.name = "root";
  root.created_utc = 1000;
  root.modified_utc = 2000;
  root.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(root));

  StoreFolderRecord sub;
  sub.id = "iter-sub-uuid";
  sub.parent_id = "iter-root-uuid";
  sub.name = "sub";
  sub.created_utc = 1100;
  sub.modified_utc = 2100;
  sub.metadata = "{}";
  ASSERT_TRUE(store.CreateFolder(sub));

  // Create files in both folders
  StoreFileRecord file1;
  file1.id = "iter-file-1";
  file1.folder_id = "iter-root-uuid";
  file1.name = "root_file.md";
  file1.created_utc = 1200;
  file1.modified_utc = 2200;
  file1.metadata = "{}";
  file1.tags = {};
  ASSERT_TRUE(store.CreateFile(file1));

  StoreFileRecord file2;
  file2.id = "iter-file-2";
  file2.folder_id = "iter-sub-uuid";
  file2.name = "sub_file.md";
  file2.created_utc = 1300;
  file2.modified_utc = 2300;
  file2.metadata = "{}";
  file2.tags = {};
  ASSERT_TRUE(store.CreateFile(file2));

  // Iterate all files
  std::vector<std::pair<std::string, std::string>> found_files;
  store.IterateAllFiles([&](const std::string& path, const StoreFileRecord& record) {
    found_files.push_back({path, record.id});
    return true;  // Continue iteration
  });

  ASSERT_EQ(found_files.size(), 2);

  // Verify paths
  bool found_root_file = false;
  bool found_sub_file = false;
  for (const auto& [path, id] : found_files) {
    if (path == "root/root_file.md" && id == "iter-file-1") {
      found_root_file = true;
    }
    if (path == "root/sub/sub_file.md" && id == "iter-file-2") {
      found_sub_file = true;
    }
  }
  ASSERT_TRUE(found_root_file);
  ASSERT_TRUE(found_sub_file);

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_iterate_all_files passed" << std::endl;
  return 0;
}

// ============================================================================
// Error Handling Tests
// ============================================================================

int test_metadata_store_not_found_errors() {
  std::cout << "  Running test_metadata_store_not_found_errors..." << std::endl;

  setup_test_db();

  SqliteMetadataStore store;
  ASSERT_TRUE(store.Open(test_db_path));

  // Operations on non-existent items should fail gracefully
  ASSERT_FALSE(store.UpdateFolder("nonexistent-uuid", "name", 1000, "{}"));
  ASSERT_FALSE(store.DeleteFolder("nonexistent-uuid"));
  ASSERT_FALSE(store.MoveFolder("nonexistent-uuid", ""));

  ASSERT_FALSE(store.UpdateFile("nonexistent-uuid", "name", 1000, "{}"));
  ASSERT_FALSE(store.DeleteFile("nonexistent-uuid"));
  ASSERT_FALSE(store.MoveFile("nonexistent-uuid", ""));

  ASSERT_FALSE(store.DeleteTag("nonexistent-tag"));

  // Get operations return nullopt
  ASSERT_FALSE(store.GetFolder("nonexistent-uuid").has_value());
  ASSERT_FALSE(store.GetFile("nonexistent-uuid").has_value());
  ASSERT_FALSE(store.GetTag("nonexistent-tag").has_value());

  store.Close();
  cleanup_test_db();
  std::cout << "  ✓ test_metadata_store_not_found_errors passed" << std::endl;
  return 0;
}

int test_metadata_store_not_open_errors() {
  std::cout << "  Running test_metadata_store_not_open_errors..." << std::endl;

  SqliteMetadataStore store;
  // Don't open the store

  // All operations should fail gracefully when store is not open
  StoreFolderRecord folder;
  folder.id = "test";
  ASSERT_FALSE(store.CreateFolder(folder));
  ASSERT_FALSE(store.UpdateFolder("test", "name", 1000, "{}"));
  ASSERT_FALSE(store.DeleteFolder("test"));
  ASSERT_FALSE(store.GetFolder("test").has_value());

  StoreFileRecord file;
  file.id = "test";
  ASSERT_FALSE(store.CreateFile(file));
  ASSERT_FALSE(store.UpdateFile("test", "name", 1000, "{}"));
  ASSERT_FALSE(store.DeleteFile("test"));
  ASSERT_FALSE(store.GetFile("test").has_value());

  ASSERT_FALSE(store.BeginTransaction());
  ASSERT_FALSE(store.CommitTransaction());
  ASSERT_FALSE(store.RollbackTransaction());
  ASSERT_FALSE(store.RebuildAll());

  std::cout << "  ✓ test_metadata_store_not_open_errors passed" << std::endl;
  return 0;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "Running MetadataStore tests..." << std::endl;

  // Lifecycle tests
  RUN_TEST(test_metadata_store_open_close);
  RUN_TEST(test_metadata_store_reopen);

  // Transaction tests
  RUN_TEST(test_metadata_store_transactions);

  // Folder CRUD tests
  RUN_TEST(test_metadata_store_create_folder);
  RUN_TEST(test_metadata_store_nested_folders);
  RUN_TEST(test_metadata_store_update_folder);
  RUN_TEST(test_metadata_store_delete_folder);
  RUN_TEST(test_metadata_store_move_folder);
  RUN_TEST(test_metadata_store_get_folder_by_path);

  // File CRUD tests
  RUN_TEST(test_metadata_store_create_file);
  RUN_TEST(test_metadata_store_update_file);
  RUN_TEST(test_metadata_store_delete_file);
  RUN_TEST(test_metadata_store_move_file);
  RUN_TEST(test_metadata_store_list_files);
  RUN_TEST(test_metadata_store_get_file_by_path);

  // Tag tests
  RUN_TEST(test_metadata_store_file_tags);
  RUN_TEST(test_metadata_store_find_files_by_tags);
  RUN_TEST(test_metadata_store_tag_definitions);

  // RebuildAll test
  RUN_TEST(test_metadata_store_rebuild_all);

  // IterateAllFiles test
  RUN_TEST(test_metadata_store_iterate_all_files);

  // Error handling tests
  RUN_TEST(test_metadata_store_not_found_errors);
  RUN_TEST(test_metadata_store_not_open_errors);

  std::cout << std::endl << "All MetadataStore tests passed!" << std::endl;
  return 0;
}
