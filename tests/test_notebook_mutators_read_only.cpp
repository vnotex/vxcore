#include "test_utils.h"
#include <core/notebook_manager.h>
#include <core/notebook.h>
#include <core/bundled_notebook.h>
#include <core/raw_notebook.h>
#include <core/file_utils.h>
#include <cstdlib>
#include <iostream>

using namespace vxcore;

// Test helpers
static NotebookManager *g_mgr = nullptr;

int test_bundled_notebook_tag_operations_read_only() {
  // Create a bundled notebook
  std::string nb_path = std::string(std::getenv("TEMP")) + "/vxcore_test_bundled_ro";
  CleanupDir(nb_path);

  NotebookConfig config;
  config.id = "test_bundled_ro";
  config.name = "Test Bundled RO";
  config.read_only = false;

  BundledNotebook *nb = BundledNotebook::Create(nb_path, config, nullptr, nullptr);
  ASSERT_NOT_NULL(nb);

  // Initially writable: CreateTag should succeed
  VxCoreError err = nb->CreateTag("tag1", "");
  ASSERT_EQ(err, VXCORE_OK);

  // Now set notebook as read-only
  nb->SetReadOnly(true);

  // CreateTag on read-only should fail with VXCORE_ERR_READ_ONLY
  err = nb->CreateTag("tag2", "");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // CreateTagPath on read-only should fail
  err = nb->CreateTagPath("path/to/tag3");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // DeleteTag on read-only should fail
  err = nb->DeleteTag("tag1");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // MoveTag on read-only should fail
  err = nb->MoveTag("tag1", "");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // UpdateConfig on read-only should fail
  err = nb->UpdateConfig(config);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // EmptyRecycleBin on read-only should fail
  err = nb->EmptyRecycleBin();
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  delete nb;
  CleanupDir(nb_path);
  return 0;
}

int test_raw_notebook_tag_operations_read_only() {
  // Create a raw notebook (in-memory DB)
  std::string nb_path = std::string(std::getenv("TEMP")) + "/vxcore_test_raw_ro";
  CleanupDir(nb_path);

  NotebookConfig config;
  config.id = "test_raw_ro";
  config.name = "Test Raw RO";
  config.read_only = false;

  RawNotebook *nb = RawNotebook::Create(nb_path, config, nullptr, nullptr);
  ASSERT_NOT_NULL(nb);

  // Initially writable: UpdateConfig should succeed
  VxCoreError err = nb->UpdateConfig(config);
  ASSERT_EQ(err, VXCORE_OK);

  // Now set notebook as read-only
  nb->SetReadOnly(true);

  // UpdateConfig on read-only should fail
  err = nb->UpdateConfig(config);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // EmptyRecycleBin on read-only should fail
  err = nb->EmptyRecycleBin();
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  // Tag operations in RawNotebook return VXCORE_ERR_UNSUPPORTED but should still
  // check read-only first
  err = nb->CreateTag("tag1", "");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);  // Read-only guard should fire before UNSUPPORTED

  err = nb->CreateTagPath("path/to/tag");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = nb->DeleteTag("tag1");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  err = nb->MoveTag("tag1", "");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  delete nb;
  CleanupDir(nb_path);
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  RUN_TEST(test_bundled_notebook_tag_operations_read_only);
  RUN_TEST(test_raw_notebook_tag_operations_read_only);

  return 0;
}
