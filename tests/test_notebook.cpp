#include <iostream>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_notebook_create_bundled() {
  std::cout << "  Running test_notebook_create_bundled..." << std::endl;
  cleanup_test_dir("test_nb_bundled");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_bundled", "{\"name\":\"Test Bundled\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  ASSERT(path_exists("test_nb_bundled/vx_notebook/config.json"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_bundled");
  std::cout << "  ✓ test_notebook_create_bundled passed" << std::endl;
  return 0;
}

int test_notebook_create_raw() {
  std::cout << "  Running test_notebook_create_raw..." << std::endl;
  cleanup_test_dir("test_nb_raw");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_raw", "{\"name\":\"Test Raw\"}", VXCORE_NOTEBOOK_RAW,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  ASSERT(!path_exists("test_nb_raw/vx_notebook"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_raw");
  std::cout << "  ✓ test_notebook_create_raw passed" << std::endl;
  return 0;
}

int test_notebook_open_close() {
  std::cout << "  Running test_notebook_open_close..." << std::endl;
  cleanup_test_dir("test_nb_open");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id1 = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_open", "{\"name\":\"Test Open\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id1);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_close(ctx, notebook_id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id2 = nullptr;
  err = vxcore_notebook_open(ctx, "test_nb_open", &notebook_id2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id2);

  ASSERT_EQ(std::string(notebook_id1), std::string(notebook_id2));

  vxcore_string_free(notebook_id1);
  vxcore_string_free(notebook_id2);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_open");
  std::cout << "  ✓ test_notebook_open_close passed" << std::endl;
  return 0;
}

int test_notebook_get_properties() {
  std::cout << "  Running test_notebook_get_properties..." << std::endl;
  cleanup_test_dir("test_nb_props");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_props",
                               "{\"name\":\"Test Props\",\"description\":\"Test Description\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *properties = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &properties);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(properties);

  std::string props_str(properties);
  ASSERT_NE(props_str.find("\"name\":\"Test Props\""), std::string::npos);
  ASSERT_NE(props_str.find("\"description\":\"Test Description\""), std::string::npos);

  vxcore_string_free(properties);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_props");
  std::cout << "  ✓ test_notebook_get_properties passed" << std::endl;
  return 0;
}

int test_notebook_set_properties() {
  std::cout << "  Running test_notebook_set_properties..." << std::endl;
  cleanup_test_dir("test_nb_set_props");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_set_props", "{\"name\":\"Original Name\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_update_config(ctx, notebook_id,
                                      "{\"name\":\"Updated Name\",\"description\":\"New Desc\"}");
  ASSERT_EQ(err, VXCORE_OK);

  char *properties = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &properties);
  ASSERT_EQ(err, VXCORE_OK);

  std::string props_str(properties);
  ASSERT_NE(props_str.find("\"name\":\"Updated Name\""), std::string::npos);
  ASSERT_NE(props_str.find("\"description\":\"New Desc\""), std::string::npos);

  vxcore_string_free(properties);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_set_props");
  std::cout << "  ✓ test_notebook_set_properties passed" << std::endl;
  return 0;
}

int test_notebook_list() {
  std::cout << "  Running test_notebook_list..." << std::endl;
  cleanup_test_dir("test_nb_list1");
  cleanup_test_dir("test_nb_list2");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id1 = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_list1", "{\"name\":\"Notebook 1\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *id2 = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_list2", "{\"name\":\"Notebook 2\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &id2);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebooks_json = nullptr;
  err = vxcore_notebook_list(ctx, &notebooks_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebooks_json);

  std::string list_str(notebooks_json);
  ASSERT_NE(list_str.find(id1), std::string::npos);
  ASSERT_NE(list_str.find(id2), std::string::npos);

  vxcore_string_free(notebooks_json);
  vxcore_string_free(id1);
  vxcore_string_free(id2);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_list1");
  cleanup_test_dir("test_nb_list2");
  std::cout << "  ✓ test_notebook_list passed" << std::endl;
  return 0;
}

int test_notebook_persistence() {
  std::cout << "  Running test_notebook_persistence..." << std::endl;
  cleanup_test_dir("test_nb_persist");

  VxCoreContextHandle ctx1 = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx1);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx1, "test_nb_persist", "{\"name\":\"Persistent Notebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string saved_id(notebook_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx1);

  VxCoreContextHandle ctx2 = nullptr;
  err = vxcore_context_create(nullptr, &ctx2);
  ASSERT_EQ(err, VXCORE_OK);

  char *reopened_id = nullptr;
  err = vxcore_notebook_open(ctx2, "test_nb_persist", &reopened_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(reopened_id);

  ASSERT_EQ(std::string(reopened_id), saved_id);

  char *properties = nullptr;
  err = vxcore_notebook_get_config(ctx2, reopened_id, &properties);
  ASSERT_EQ(err, VXCORE_OK);

  std::string props_str(properties);
  ASSERT_NE(props_str.find("\"name\":\"Persistent Notebook\""), std::string::npos);

  vxcore_string_free(properties);
  vxcore_string_free(reopened_id);
  vxcore_context_destroy(ctx2);
  cleanup_test_dir("test_nb_persist");
  std::cout << "  ✓ test_notebook_persistence passed" << std::endl;
  return 0;
}

int test_notebook_rebuild_cache() {
  std::cout << "  Running test_notebook_rebuild_cache..." << std::endl;
  cleanup_test_dir("test_nb_rebuild");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create notebook with some content
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_rebuild", "{\"name\":\"Rebuild Cache Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder and file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "readme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Call rebuild cache - should succeed
  err = vxcore_notebook_rebuild_cache(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify data is still accessible after rebuild
  char *folder_config = nullptr;
  err = vxcore_folder_get_config(ctx, notebook_id, "docs", &folder_config);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_config);

  std::string config_str(folder_config);
  ASSERT_NE(config_str.find("\"readme.md\""), std::string::npos);

  vxcore_string_free(folder_config);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_rebuild");
  std::cout << "  ✓ test_notebook_rebuild_cache passed" << std::endl;
  return 0;
}

int test_tag_create_list() {
  std::cout << "  Running test_tag_create_list..." << std::endl;
  cleanup_test_dir("test_nb_tags");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tags", "{\"name\":\"Tag Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "work");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "personal");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(tags_json);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"work\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"personal\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tags");
  std::cout << "  ✓ test_tag_create_list passed" << std::endl;
  return 0;
}

int test_tag_duplicate_create() {
  std::cout << "  Running test_tag_duplicate_create..." << std::endl;
  cleanup_test_dir("test_nb_dup_tag");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_dup_tag", "{\"name\":\"Dup Tag Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_ERR_ALREADY_EXISTS);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_dup_tag");
  std::cout << "  ✓ test_tag_duplicate_create passed" << std::endl;
  return 0;
}

int test_tag_delete() {
  std::cout << "  Running test_tag_delete..." << std::endl;
  cleanup_test_dir("test_nb_del_tag");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_del_tag", "{\"name\":\"Del Tag Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "temp");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_delete(ctx, notebook_id, "temp");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_EQ(tags_str.find("\"temp\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_del_tag");
  std::cout << "  ✓ test_tag_delete passed" << std::endl;
  return 0;
}

int test_file_tag_operations() {
  std::cout << "  Running test_file_tag_operations..." << std::endl;
  cleanup_test_dir("test_nb_file_tag");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_file_tag", "{\"name\":\"File Tag Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "test.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_info = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "test.md", &file_info);
  ASSERT_EQ(err, VXCORE_OK);

  std::string info_str(file_info);
  ASSERT_NE(info_str.find("\"urgent\""), std::string::npos);

  err = vxcore_file_untag(ctx, notebook_id, "test.md", "urgent");
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(file_info);
  err = vxcore_file_get_info(ctx, notebook_id, "test.md", &file_info);
  ASSERT_EQ(err, VXCORE_OK);

  info_str = std::string(file_info);
  ASSERT_EQ(info_str.find("\"urgent\""), std::string::npos);

  vxcore_string_free(file_info);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_file_tag");
  std::cout << "  ✓ test_file_tag_operations passed" << std::endl;
  return 0;
}

int test_tag_validation() {
  std::cout << "  Running test_tag_validation..." << std::endl;
  cleanup_test_dir("test_nb_tag_val");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_val", "{\"name\":\"Tag Validation Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "valid");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "", "doc.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "doc.md", "nonexistent");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  err = vxcore_file_tag(ctx, notebook_id, "doc.md", "valid");
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_val");
  std::cout << "  ✓ test_tag_validation passed" << std::endl;
  return 0;
}

int test_tag_delete_with_files() {
  std::cout << "  Running test_tag_delete_with_files..." << std::endl;
  cleanup_test_dir("test_nb_tag_del_files");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_del_files", "{\"name\":\"Tag Delete Files Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "active");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "active");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_delete(ctx, notebook_id, "active");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_info = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "note.md", &file_info);
  ASSERT_EQ(err, VXCORE_OK);

  std::string info_str(file_info);
  ASSERT_EQ(info_str.find("\"active\""), std::string::npos);

  vxcore_string_free(file_info);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_del_files");
  std::cout << "  ✓ test_tag_delete_with_files passed" << std::endl;
  return 0;
}

int test_tag_delete_children() {
  std::cout << "  Running test_tag_delete_children..." << std::endl;
  cleanup_test_dir("test_nb_tag_del_children");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_del_children",
                               "{\"name\":\"Tag Delete Children Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "root");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "root/child1");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "root/child2");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "root/child1/grandchild");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_delete(ctx, notebook_id, "root");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_EQ(tags_str.find("\"root\""), std::string::npos);
  ASSERT_EQ(tags_str.find("\"child1\""), std::string::npos);
  ASSERT_EQ(tags_str.find("\"child2\""), std::string::npos);
  ASSERT_EQ(tags_str.find("\"grandchild\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_del_children");
  std::cout << "  ✓ test_tag_delete_children passed" << std::endl;
  return 0;
}

int test_file_update_tags() {
  std::cout << "  Running test_file_update_tags..." << std::endl;
  cleanup_test_dir("test_nb_update_tags");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_update_tags", "{\"name\":\"Update Tags Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "tag1");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "tag2");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "", "multi.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_update_tags(ctx, notebook_id, "multi.md", "[\"tag1\",\"tag2\"]");
  ASSERT_EQ(err, VXCORE_OK);

  char *file_info = nullptr;
  err = vxcore_file_get_info(ctx, notebook_id, "multi.md", &file_info);
  ASSERT_EQ(err, VXCORE_OK);

  std::string info_str(file_info);
  ASSERT_NE(info_str.find("\"tag1\""), std::string::npos);
  ASSERT_NE(info_str.find("\"tag2\""), std::string::npos);

  err = vxcore_file_update_tags(ctx, notebook_id, "multi.md", "[\"invalid\"]");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(file_info);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_update_tags");
  std::cout << "  ✓ test_file_update_tags passed" << std::endl;
  return 0;
}

int test_tag_move_basic() {
  std::cout << "  Running test_tag_move_basic..." << std::endl;
  cleanup_test_dir("test_nb_tag_move");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move", "{\"name\":\"Tag Move Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "parent");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "child");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "child", "parent");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"child\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"parent\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move");
  std::cout << "  ✓ test_tag_move_basic passed" << std::endl;
  return 0;
}

int test_tag_move_to_root() {
  std::cout << "  Running test_tag_move_to_root..." << std::endl;
  cleanup_test_dir("test_nb_tag_move_root");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move_root", "{\"name\":\"Tag Move Root Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "parent");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "parent/child");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "parent/child", "");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"parent/child\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move_root");
  std::cout << "  ✓ test_tag_move_to_root passed" << std::endl;
  return 0;
}

int test_tag_move_nonexistent_tag() {
  std::cout << "  Running test_tag_move_nonexistent_tag..." << std::endl;
  cleanup_test_dir("test_nb_tag_move_noexist");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move_noexist", "{\"name\":\"Tag Move Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "parent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "nonexistent", "parent");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move_noexist");
  std::cout << "  ✓ test_tag_move_nonexistent_tag passed" << std::endl;
  return 0;
}

int test_tag_move_nonexistent_parent() {
  std::cout << "  Running test_tag_move_nonexistent_parent..." << std::endl;
  cleanup_test_dir("test_nb_tag_move_noparent");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move_noparent", "{\"name\":\"Tag Move Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "child");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "child", "nonexistent");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move_noparent");
  std::cout << "  ✓ test_tag_move_nonexistent_parent passed" << std::endl;
  return 0;
}

int test_tag_move_self_parent() {
  std::cout << "  Running test_tag_move_self_parent..." << std::endl;
  cleanup_test_dir("test_nb_tag_move_self");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move_self", "{\"name\":\"Tag Move Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "tag");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "tag", "tag");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move_self");
  std::cout << "  ✓ test_tag_move_self_parent passed" << std::endl;
  return 0;
}

int test_tag_move_circular_dependency() {
  std::cout << "  Running test_tag_move_circular_dependency..." << std::endl;
  cleanup_test_dir("test_nb_tag_move_circular");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move_circular", "{\"name\":\"Tag Move Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "parent");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "child");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "grandchild");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "child", "parent");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_move(ctx, notebook_id, "grandchild", "child");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "parent", "grandchild");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move_circular");
  std::cout << "  ✓ test_tag_move_circular_dependency passed" << std::endl;
  return 0;
}

int test_tag_move_reparent() {
  std::cout << "  Running test_tag_move_reparent..." << std::endl;
  cleanup_test_dir("test_nb_tag_move_reparent");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_move_reparent", "{\"name\":\"Tag Move Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "oldparent");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "oldparent/child");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "newparent");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_move(ctx, notebook_id, "oldparent/child", "newparent");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"oldparent/child\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"newparent\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_move_reparent");
  std::cout << "  ✓ test_tag_move_reparent passed" << std::endl;
  return 0;
}

int test_tag_create_path_basic() {
  std::cout << "  Running test_tag_create_path_basic..." << std::endl;
  cleanup_test_dir("test_nb_tag_path_basic");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_path_basic", "{\"name\":\"Tag Path Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "category/subcategory/item");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"category\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"name\":\"subcategory\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"name\":\"item\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"category\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"subcategory\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_path_basic");
  std::cout << "  ✓ test_tag_create_path_basic passed" << std::endl;
  return 0;
}

int test_tag_create_path_single() {
  std::cout << "  Running test_tag_create_path_single..." << std::endl;
  cleanup_test_dir("test_nb_tag_path_single");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_path_single", "{\"name\":\"Tag Path Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "singletag");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"singletag\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_path_single");
  std::cout << "  ✓ test_tag_create_path_single passed" << std::endl;
  return 0;
}

int test_tag_create_path_partial_exists() {
  std::cout << "  Running test_tag_create_path_partial_exists..." << std::endl;
  cleanup_test_dir("test_nb_tag_path_partial");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_path_partial", "{\"name\":\"Tag Path Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "existing");
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "existing/new1/new2");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"existing\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"name\":\"new1\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"name\":\"new2\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"existing\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"parent\":\"new1\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_path_partial");
  std::cout << "  ✓ test_tag_create_path_partial_exists passed" << std::endl;
  return 0;
}

int test_tag_create_path_empty() {
  std::cout << "  Running test_tag_create_path_empty..." << std::endl;
  cleanup_test_dir("test_nb_tag_path_empty");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_path_empty", "{\"name\":\"Tag Path Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_path_empty");
  std::cout << "  ✓ test_tag_create_path_empty passed" << std::endl;
  return 0;
}

int test_tag_create_path_trailing_slash() {
  std::cout << "  Running test_tag_create_path_trailing_slash..." << std::endl;
  cleanup_test_dir("test_nb_tag_path_trailing");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_tag_path_trailing", "{\"name\":\"Tag Path Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create_path(ctx, notebook_id, "tag1/tag2/");
  ASSERT_EQ(err, VXCORE_OK);

  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);

  std::string tags_str(tags_json);
  ASSERT_NE(tags_str.find("\"name\":\"tag1\""), std::string::npos);
  ASSERT_NE(tags_str.find("\"name\":\"tag2\""), std::string::npos);

  vxcore_string_free(tags_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_tag_path_trailing");
  std::cout << "  ✓ test_tag_create_path_trailing_slash passed" << std::endl;
  return 0;
}

// Test that closing a notebook properly releases the DB file lock
// so that the local data folder can be deleted (especially important on Windows)
int test_notebook_close_releases_db_lock() {
  std::cout << "  Running test_notebook_close_releases_db_lock..." << std::endl;
  cleanup_test_dir("test_nb_close_db");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a bundled notebook (which has a local data folder with DB)
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, "test_nb_close_db", "{\"name\":\"Close DB Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create some content to ensure DB is used
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  // Close the notebook - this should release the DB lock and delete local data
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify the notebook is no longer accessible
  char *config_json = nullptr;
  err = vxcore_notebook_get_config(ctx, notebook_id, &config_json);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // The notebook root folder should still exist (it's not deleted on close)
  ASSERT(path_exists("test_nb_close_db"));
  ASSERT(path_exists("test_nb_close_db/vx_notebook/config.json"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir("test_nb_close_db");
  std::cout << "  ✓ test_notebook_close_releases_db_lock passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running notebook tests..." << std::endl;

  vxcore_set_test_mode(1);

  RUN_TEST(test_notebook_create_bundled);
  RUN_TEST(test_notebook_create_raw);
  RUN_TEST(test_notebook_open_close);
  RUN_TEST(test_notebook_close_releases_db_lock);
  RUN_TEST(test_notebook_get_properties);
  RUN_TEST(test_notebook_set_properties);
  RUN_TEST(test_notebook_list);
  RUN_TEST(test_notebook_persistence);
  RUN_TEST(test_notebook_rebuild_cache);
  RUN_TEST(test_tag_create_list);
  RUN_TEST(test_tag_duplicate_create);
  RUN_TEST(test_tag_delete);
  RUN_TEST(test_file_tag_operations);
  RUN_TEST(test_tag_validation);
  RUN_TEST(test_tag_delete_with_files);
  RUN_TEST(test_tag_delete_children);
  RUN_TEST(test_file_update_tags);
  RUN_TEST(test_tag_move_basic);
  RUN_TEST(test_tag_move_to_root);
  RUN_TEST(test_tag_move_nonexistent_tag);
  RUN_TEST(test_tag_move_nonexistent_parent);
  RUN_TEST(test_tag_move_self_parent);
  RUN_TEST(test_tag_move_circular_dependency);
  RUN_TEST(test_tag_move_reparent);
  RUN_TEST(test_tag_create_path_basic);
  RUN_TEST(test_tag_create_path_single);
  RUN_TEST(test_tag_create_path_partial_exists);
  RUN_TEST(test_tag_create_path_empty);
  RUN_TEST(test_tag_create_path_trailing_slash);

  std::cout << "✓ All notebook tests passed" << std::endl;
  return 0;
}
