#include "test_utils.h"

#include "vxcore/vxcore.h"

#include <iostream>
#include <string>

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
  err = vxcore_notebook_create(ctx, "test_nb_raw", "{\"name\":\"Test Raw\"}",
                                VXCORE_NOTEBOOK_RAW, &notebook_id);
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
  err = vxcore_notebook_get_properties(ctx, notebook_id, &properties);
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

  err = vxcore_notebook_set_properties(ctx, notebook_id,
                                        "{\"name\":\"Updated Name\",\"description\":\"New Desc\"}");
  ASSERT_EQ(err, VXCORE_OK);

  char *properties = nullptr;
  err = vxcore_notebook_get_properties(ctx, notebook_id, &properties);
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
  err = vxcore_notebook_get_properties(ctx2, reopened_id, &properties);
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

int main() {
  std::cout << "Running notebook tests..." << std::endl;
  int result = 0;
  
  result |= test_notebook_create_bundled();
  result |= test_notebook_create_raw();
  result |= test_notebook_open_close();
  result |= test_notebook_get_properties();
  result |= test_notebook_set_properties();
  result |= test_notebook_list();
  result |= test_notebook_persistence();
  
  if (result == 0) {
    std::cout << "✓ All notebook tests passed" << std::endl;
  } else {
    std::cerr << "✗ Some notebook tests failed" << std::endl;
  }
  
  return result;
}
