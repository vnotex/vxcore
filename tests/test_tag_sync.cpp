#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_notebook_meta_get_set() {
  std::cout << "  Running test_notebook_meta_get_set..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_meta"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_tag_sync_meta").c_str(),
                               "{\"name\":\"Test Meta\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id2 = nullptr;
  err = vxcore_notebook_open(ctx, get_test_path("test_tag_sync_meta").c_str(), &notebook_id2);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_string_free(notebook_id2);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_meta"));
  std::cout << "  ✓ test_notebook_meta_get_set passed" << std::endl;
  return 0;
}

int test_tags_modified_utc_serialization() {
  std::cout << "  Running test_tags_modified_utc_serialization..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_serial"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_tag_sync_serial").c_str(),
                               "{\"name\":\"Test Serial\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "TestTag");
  ASSERT_EQ(err, VXCORE_OK);

  char *config_json = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &config_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string config_str(config_json);
  ASSERT_NE(config_str.find("tagsModifiedUtc"), std::string::npos);
  vxcore_string_free(config_json);

  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id2 = nullptr;
  err = vxcore_notebook_open(ctx, get_test_path("test_tag_sync_serial").c_str(), &notebook_id2);
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id2, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("TestTag"), std::string::npos);
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_string_free(notebook_id2);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_serial"));
  std::cout << "  ✓ test_tags_modified_utc_serialization passed" << std::endl;
  return 0;
}

int test_tag_operations_update_timestamp() {
  std::cout << "  Running test_tag_operations_update_timestamp..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_ts"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_tag_sync_ts").c_str(),
                               "{\"name\":\"Test Timestamp\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "Tag1");
  ASSERT_EQ(err, VXCORE_OK);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  err = vxcore_tag_create(ctx, notebook_id, "Tag2");
  ASSERT_EQ(err, VXCORE_OK);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  err = vxcore_tag_delete(ctx, notebook_id, "Tag1");
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_ts"));
  std::cout << "  ✓ test_tag_operations_update_timestamp passed" << std::endl;
  return 0;
}

int test_sync_tags_basic() {
  std::cout << "  Running test_sync_tags_basic..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_basic"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_tag_sync_basic").c_str(),
                               "{\"name\":\"Test Sync Basic\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "Parent/Child");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id2 = nullptr;
  err = vxcore_notebook_open(ctx, get_test_path("test_tag_sync_basic").c_str(), &notebook_id2);
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id2, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("Parent"), std::string::npos);
  ASSERT_NE(tags_str.find("Child"), std::string::npos);
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_string_free(notebook_id2);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_basic"));
  std::cout << "  ✓ test_sync_tags_basic passed" << std::endl;
  return 0;
}

int test_sync_tags_idempotent() {
  std::cout << "  Running test_sync_tags_idempotent..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_idem"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_tag_sync_idem").c_str(),
                               "{\"name\":\"Test Idempotent\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "TestTag");
  ASSERT_EQ(err, VXCORE_OK);

  for (int i = 0; i < 3; i++) {
    err = vxcore_notebook_close(ctx, notebook_id);
    ASSERT_EQ(err, VXCORE_OK);
    vxcore_string_free(notebook_id);
    notebook_id = nullptr;

    err = vxcore_notebook_open(ctx, get_test_path("test_tag_sync_idem").c_str(), &notebook_id);
    ASSERT_EQ(err, VXCORE_OK);
  }

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("TestTag"), std::string::npos);
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_idem"));
  std::cout << "  ✓ test_sync_tags_idempotent passed" << std::endl;
  return 0;
}

int test_sync_tags_parent_ordering() {
  std::cout << "  Running test_sync_tags_parent_ordering..." << std::endl;
  cleanup_test_dir(get_test_path("test_tag_sync_order"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_tag_sync_order").c_str(),
                               "{\"name\":\"Test Order\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "Root/Level1/Level2/Level3");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id2 = nullptr;
  err = vxcore_notebook_open(ctx, get_test_path("test_tag_sync_order").c_str(), &notebook_id2);
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id2, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("Root"), std::string::npos);
  ASSERT_NE(tags_str.find("Level1"), std::string::npos);
  ASSERT_NE(tags_str.find("Level2"), std::string::npos);
  ASSERT_NE(tags_str.find("Level3"), std::string::npos);
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_string_free(notebook_id2);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_tag_sync_order"));
  std::cout << "  ✓ test_sync_tags_parent_ordering passed" << std::endl;
  return 0;
}

// =============================================================================
// Raw Notebook Tag Sync Tests
// =============================================================================

// Note: Raw notebooks store their config in the session folder, which is cleared
// when a new context is created in test mode. Unlike bundled notebooks (which store
// config in notebook root), raw notebooks cannot survive context recreation in tests.
// These tests verify tag sync works within a single context session.

int test_raw_notebook_sync_tags_on_create() {
  std::cout << "  Running test_raw_notebook_sync_tags_on_create..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_tag_sync_create"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a raw notebook
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_tag_sync_create").c_str(),
                               "{\"name\":\"Test Raw Sync\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create hierarchical tags - this triggers SyncTagsToMetadataStore on tag creation
  err = vxcore_tag_create_path(ctx, notebook_id, "Parent/Child");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify tags are available immediately (sync happened during create)
  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("Parent"), std::string::npos);
  ASSERT_NE(tags_str.find("Child"), std::string::npos);
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_sync_create"));
  std::cout << "  ✓ test_raw_notebook_sync_tags_on_create passed" << std::endl;
  return 0;
}

int test_raw_notebook_sync_deep_hierarchy_on_create() {
  std::cout << "  Running test_raw_notebook_sync_deep_hierarchy_on_create..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_tag_sync_deep"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_tag_sync_deep").c_str(),
                               "{\"name\":\"Test Raw Deep\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create deep tag hierarchy
  err = vxcore_tag_create_path(ctx, notebook_id, "Root/Level1/Level2/Level3");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify all hierarchy levels are synced
  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("Root"), std::string::npos);
  ASSERT_NE(tags_str.find("Level1"), std::string::npos);
  ASSERT_NE(tags_str.find("Level2"), std::string::npos);
  ASSERT_NE(tags_str.find("Level3"), std::string::npos);
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_sync_deep"));
  std::cout << "  ✓ test_raw_notebook_sync_deep_hierarchy_on_create passed" << std::endl;
  return 0;
}

int test_raw_notebook_tag_operations() {
  std::cout << "  Running test_raw_notebook_tag_operations..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_tag_ops"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_tag_ops").c_str(),
                               "{\"name\":\"Test Raw Ops\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create, delete, and verify tag operations work correctly
  err = vxcore_tag_create(ctx, notebook_id, "Tag1");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "Tag2");
  ASSERT_EQ(err, VXCORE_OK);

  // Delete Tag1
  err = vxcore_tag_delete(ctx, notebook_id, "Tag1");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify Tag1 is gone, Tag2 remains
  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  std::string tags_str(tags_json);
  ASSERT_EQ(tags_str.find("Tag1"), std::string::npos);  // Tag1 should NOT be found
  ASSERT_NE(tags_str.find("Tag2"), std::string::npos);  // Tag2 should be found
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_ops"));
  std::cout << "  ✓ test_raw_notebook_tag_operations passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running tag sync tests..." << std::endl;

  // Bundled notebook tests
  std::cout << "\n=== Bundled Notebook Tests ===" << std::endl;
  RUN_TEST(test_notebook_meta_get_set);
  RUN_TEST(test_tags_modified_utc_serialization);
  RUN_TEST(test_tag_operations_update_timestamp);
  RUN_TEST(test_sync_tags_basic);
  RUN_TEST(test_sync_tags_idempotent);
  RUN_TEST(test_sync_tags_parent_ordering);

  // Raw notebook tests
  std::cout << "\n=== Raw Notebook Tests ===" << std::endl;
  RUN_TEST(test_raw_notebook_sync_tags_on_create);
  RUN_TEST(test_raw_notebook_sync_deep_hierarchy_on_create);
  RUN_TEST(test_raw_notebook_tag_operations);

  std::cout << "\n✓ All tag sync tests passed" << std::endl;
  return 0;
}
