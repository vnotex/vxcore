#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
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

// Raw notebooks do not support tag operations — they return VXCORE_ERR_UNSUPPORTED.
// These tests verify that behavior.

int test_raw_notebook_sync_tags_on_create() {
  std::cout << "  Running test_raw_notebook_sync_tags_on_create..." << std::endl;
  cleanup_test_dir(get_test_path("test_raw_tag_sync_create"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_raw_tag_sync_create").c_str(),
                               "{\"name\":\"Test Raw Sync\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Tag creation is unsupported on raw notebooks
  err = vxcore_tag_create_path(ctx, notebook_id, "Parent/Child");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  // Tag list returns empty array
  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(std::string(tags_json), "[]");
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_sync_create"));
  std::cout << "  \xE2\x9C\x93 test_raw_notebook_sync_tags_on_create passed" << std::endl;
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

  // Deep tag hierarchy creation is unsupported
  err = vxcore_tag_create_path(ctx, notebook_id, "Root/Level1/Level2/Level3");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  // Tag list returns empty
  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(std::string(tags_json), "[]");
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_sync_deep"));
  std::cout << "  \xE2\x9C\x93 test_raw_notebook_sync_deep_hierarchy_on_create passed" << std::endl;
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

  // All tag mutation operations return UNSUPPORTED
  err = vxcore_tag_create(ctx, notebook_id, "Tag1");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_tag_delete(ctx, notebook_id, "Tag1");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  err = vxcore_tag_move(ctx, notebook_id, "Tag1", "Tag2");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);

  // Tag list always returns empty
  char *tags_json = nullptr;
  err = vxcore_tag_list(ctx, notebook_id, &tags_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(std::string(tags_json), "[]");
  vxcore_string_free(tags_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_raw_tag_ops"));
  std::cout << "  \xE2\x9C\x93 test_raw_notebook_tag_operations passed" << std::endl;
  return 0;
}

static std::string tag_query_text(VxCoreUtf8View text) {
  return text.size == 0 ? std::string() : std::string(text.data, text.size);
}

static int assert_tag_files(const VxCoreTagFileResultView *view, const nlohmann::json &expected) {
  ASSERT_NOT_NULL(view);
  ASSERT_EQ(view->file_count, expected.size());
  if (view->file_count != 0) {
    ASSERT_NOT_NULL(view->files);
  }
  for (size_t i = 0; i < view->file_count; ++i) {
    const auto &file = view->files[i];
    for (const auto text : {file.file_path, file.file_name}) {
      if (text.size != 0) {
        ASSERT_NOT_NULL(text.data);
      }
    }
    ASSERT_EQ(tag_query_text(file.file_path), expected[i]["filePath"].get<std::string>());
    ASSERT_EQ(tag_query_text(file.file_name), expected[i]["fileName"].get<std::string>());
    ASSERT_EQ(file.tag_count, expected[i]["tags"].size());
    if (file.tag_count != 0) {
      ASSERT_NOT_NULL(file.tags);
    }
    for (size_t j = 0; j < file.tag_count; ++j) {
      if (file.tags[j].size != 0) {
        ASSERT_NOT_NULL(file.tags[j].data);
      }
      ASSERT_EQ(tag_query_text(file.tags[j]), expected[i]["tags"][j].get<std::string>());
    }
  }
  return 0;
}

static int assert_tag_counts(const VxCoreTagCountsView *view, const nlohmann::json &expected) {
  ASSERT_NOT_NULL(view);
  ASSERT_EQ(view->tag_count, expected.size());
  if (view->tag_count != 0) {
    ASSERT_NOT_NULL(view->tags);
  }
  for (size_t i = 0; i < view->tag_count; ++i) {
    const auto &entry = view->tags[i];
    if (entry.tag.size != 0) {
      ASSERT_NOT_NULL(entry.tag.data);
    }
    ASSERT_EQ(tag_query_text(entry.tag), expected[i]["tag"].get<std::string>());
    ASSERT_EQ(entry.count, expected[i]["count"].get<int64_t>());
  }
  return 0;
}

int test_typed_tag_queries_parity_and_lifetime() {
  const auto root = get_test_path("test_typed_tag_queries");
  cleanup_test_dir(root);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Typed queries\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_tag_create_path(ctx, notebook_id, "parent/child"), VXCORE_OK);
  ASSERT_EQ(vxcore_tag_create(ctx, notebook_id, "beta"), VXCORE_OK);
  char *node_id = nullptr;
  ASSERT_EQ(vxcore_folder_create(ctx, notebook_id, "", "nested", &node_id), VXCORE_OK);
  vxcore_string_free(node_id);
  // Create out of result order and include a non-ASCII path/name.
  const char *unicode_name = "\xE7\xAC\x94\xE8\xAE\xB0.md";
  for (const auto *name : {"z.md", unicode_name, "a.md"}) {
    ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "nested", name, &node_id), VXCORE_OK);
    vxcore_string_free(node_id);
  }
  ASSERT_EQ(vxcore_file_update_tags(ctx, notebook_id, "nested/z.md", "[\"parent\"]"), VXCORE_OK);
  ASSERT_EQ(vxcore_file_update_tags(ctx, notebook_id, "nested/a.md", "[\"child\",\"beta\"]"),
            VXCORE_OK);
  const std::string unicode_path = std::string("nested/") + unicode_name;
  ASSERT_EQ(vxcore_file_update_tags(ctx, notebook_id, unicode_path.c_str(), "[\"child\"]"),
            VXCORE_OK);

  const auto expected_files = nlohmann::json::parse(R"([
    {"filePath":"nested/a.md","fileName":"a.md","tags":["beta","child"]},
    {"filePath":"nested/z.md","fileName":"z.md","tags":["parent"]},
    {"filePath":"nested/\u7b14\u8bb0.md","fileName":"\u7b14\u8bb0.md","tags":["child"]}
  ])");
  // Parent is one direct association, not the sum of its child's associations.
  const auto expected_counts = nlohmann::json::parse(R"([
    {"tag":"child","count":2}, {"tag":"beta","count":1},
    {"tag":"parent","count":1}
  ])");
  VxCoreTagFileResult *files = nullptr;
  ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, "[\"parent\",\"child\"]", "OR", &files),
            VXCORE_OK);
  ASSERT_NOT_NULL(files);
  const auto *files_view = vxcore_tag_file_result_view(files);
  ASSERT_EQ(assert_tag_files(files_view, expected_files), 0);
  char *legacy_json = nullptr;
  ASSERT_EQ(vxcore_tag_find_files(ctx, notebook_id, "[\"parent\",\"child\"]", "OR", &legacy_json),
            VXCORE_OK);
  auto legacy_files = nlohmann::json::parse(legacy_json);
  vxcore_string_free(legacy_json);
  ASSERT_EQ(legacy_files["matchCount"].get<size_t>(), files_view->file_count);
  ASSERT_EQ(assert_tag_files(files_view, legacy_files["matches"]), 0);

  VxCoreTagCounts *counts = nullptr;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, notebook_id, &counts), VXCORE_OK);
  ASSERT_NOT_NULL(counts);
  const auto *counts_view = vxcore_tag_counts_view(counts);
  ASSERT_EQ(assert_tag_counts(counts_view, expected_counts), 0);
  ASSERT_EQ(vxcore_tag_count_files_by_tag(ctx, notebook_id, &legacy_json), VXCORE_OK);
  ASSERT_EQ(assert_tag_counts(counts_view, nlohmann::json::parse(legacy_json)), 0);
  vxcore_string_free(legacy_json);

  // AND returns the intersection; a single selected tag still returns ALL file tags.
  for (const auto *query : {"[\"child\",\"beta\"]", "[\"beta\"]"}) {
    VxCoreTagFileResult *intersection = nullptr;
    ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, query, "AND", &intersection),
              VXCORE_OK);
    const auto *view = vxcore_tag_file_result_view(intersection);
    ASSERT_EQ(assert_tag_files(view, nlohmann::json::array({expected_files[0]})), 0);
    ASSERT_EQ(vxcore_tag_find_files(ctx, notebook_id, query, "AND", &legacy_json), VXCORE_OK);
    legacy_files = nlohmann::json::parse(legacy_json);
    vxcore_string_free(legacy_json);
    ASSERT_EQ(legacy_files["matchCount"].get<size_t>(), view->file_count);
    ASSERT_EQ(assert_tag_files(view, legacy_files["matches"]), 0);
    vxcore_tag_file_result_free(intersection);
  }

  ASSERT_EQ(vxcore_tag_delete(ctx, notebook_id, "child"), VXCORE_OK);
  VxCoreTagCounts *changed_counts = nullptr;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, notebook_id, &changed_counts), VXCORE_OK);
  // Deletion removes two associations; whether the store retains a zero-count
  // tag row is an indexing detail, not a result-transport contract.
  const auto *changed_view = vxcore_tag_counts_view(changed_counts);
  int64_t associations = 0;
  for (size_t i = 0; i < changed_view->tag_count; ++i) {
    associations += changed_view->tags[i].count;
  }
  ASSERT_EQ(associations, 2);
  vxcore_tag_counts_free(changed_counts);
  VxCoreTagFileResult *changed_files = nullptr;
  ASSERT_EQ(
      vxcore_tag_find_files_typed(ctx, notebook_id, "[\"parent\",\"child\"]", "OR", &changed_files),
      VXCORE_OK);
  ASSERT_EQ(assert_tag_files(vxcore_tag_file_result_view(changed_files),
                             nlohmann::json::array({expected_files[1]})),
            0);
  vxcore_tag_file_result_free(changed_files);
  ASSERT_EQ(assert_tag_files(files_view, expected_files), 0);
  ASSERT_EQ(assert_tag_counts(counts_view, expected_counts), 0);

  ASSERT_EQ(vxcore_notebook_close(ctx, notebook_id), VXCORE_OK);
  ASSERT_EQ(assert_tag_files(files_view, expected_files), 0);
  ASSERT_EQ(assert_tag_counts(counts_view, expected_counts), 0);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  ASSERT_EQ(assert_tag_files(files_view, expected_files), 0);
  ASSERT_EQ(assert_tag_counts(counts_view, expected_counts), 0);
  ASSERT_EQ(vxcore_tag_file_result_view(files), files_view);
  ASSERT_EQ(vxcore_tag_counts_view(counts), counts_view);
  vxcore_tag_file_result_free(files);
  vxcore_tag_counts_free(counts);
  cleanup_test_dir(root);
  return 0;
}

int test_typed_tag_queries_empty_errors_and_raw() {
  ASSERT_NULL(vxcore_tag_file_result_view(nullptr));
  ASSERT_NULL(vxcore_tag_counts_view(nullptr));
  vxcore_tag_file_result_free(nullptr);
  vxcore_tag_counts_free(nullptr);
  const auto root = get_test_path("test_typed_tag_empty");
  const auto raw_root = get_test_path("test_typed_tag_raw");
  cleanup_test_dir(root);
  cleanup_test_dir(raw_root);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Empty queries\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  VxCoreTagCounts *counts = nullptr;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, notebook_id, &counts), VXCORE_OK);
  ASSERT_NOT_NULL(counts);
  ASSERT_EQ(assert_tag_counts(vxcore_tag_counts_view(counts), nlohmann::json::array()), 0);

  // Empty query must not match all files, even when tagged files exist.
  ASSERT_EQ(vxcore_tag_create(ctx, notebook_id, "present"), VXCORE_OK);
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, "", "note.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);
  ASSERT_EQ(vxcore_file_tag(ctx, notebook_id, "note.md", "present"), VXCORE_OK);
  for (const auto *op : {"AND", "OR"}) {
    for (const auto *query : {"[]", "[\"missing\"]", "[\"missing\",\"absent\"]"}) {
      VxCoreTagFileResult *empty = nullptr;
      ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, query, op, &empty), VXCORE_OK);
      ASSERT_NOT_NULL(empty);
      ASSERT_EQ(assert_tag_files(vxcore_tag_file_result_view(empty), nlohmann::json::array()), 0);
      char *legacy_json = nullptr;
      ASSERT_EQ(vxcore_tag_find_files(ctx, notebook_id, query, op, &legacy_json), VXCORE_OK);
      const auto legacy = nlohmann::json::parse(legacy_json);
      vxcore_string_free(legacy_json);
      ASSERT_EQ(legacy["matchCount"].get<int>(), 0);
      ASSERT_EQ(assert_tag_files(vxcore_tag_file_result_view(empty), legacy["matches"]), 0);
      vxcore_tag_file_result_free(empty);
    }
  }
  VxCoreTagFileResult *files = nullptr;
  ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, "[\"present\"]", "OR", &files),
            VXCORE_OK);
  ASSERT_NOT_NULL(files);
  const auto expected_files =
      nlohmann::json::parse(R"([{"filePath":"note.md","fileName":"note.md","tags":["present"]}])");

  struct InvalidQuery {
    const char *tags;
    const char *op;
    VxCoreError error;
  };
  const InvalidQuery invalid_queries[] = {
      {nullptr, "AND", VXCORE_ERR_NULL_POINTER}, {"[]", nullptr, VXCORE_ERR_NULL_POINTER},
      {"[", "AND", VXCORE_ERR_JSON_PARSE},       {"{}", "AND", VXCORE_ERR_INVALID_PARAM},
      {"[7]", "AND", VXCORE_ERR_INVALID_PARAM},  {"[]", "XOR", VXCORE_ERR_INVALID_PARAM}};
  for (const auto &query : invalid_queries) {
    VxCoreTagFileResult *out = files;
    ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, query.tags, query.op, &out),
              query.error);
    ASSERT_NULL(out);
    char *legacy_json = nullptr;
    ASSERT_EQ(vxcore_tag_find_files(ctx, notebook_id, query.tags, query.op, &legacy_json),
              query.error);
    vxcore_string_free(legacy_json);
    ASSERT_EQ(assert_tag_files(vxcore_tag_file_result_view(files), expected_files), 0);
  }
  VxCoreTagFileResult *file_out = files;
  ASSERT_EQ(vxcore_tag_find_files_typed(nullptr, notebook_id, "[]", "OR", &file_out),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(file_out);
  file_out = files;
  ASSERT_EQ(vxcore_tag_find_files_typed(ctx, nullptr, "[]", "OR", &file_out),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(file_out);
  file_out = files;
  ASSERT_EQ(vxcore_tag_find_files_typed(ctx, "missing-notebook", "[]", "OR", &file_out),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(file_out);
  ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, "[]", "OR", nullptr),
            VXCORE_ERR_NULL_POINTER);
  VxCoreTagCounts *count_out = counts;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(nullptr, notebook_id, &count_out),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(count_out);
  count_out = counts;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, nullptr, &count_out), VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(count_out);
  count_out = counts;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, "missing-notebook", &count_out),
            VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(count_out);
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, notebook_id, nullptr),
            VXCORE_ERR_NULL_POINTER);

  char *raw_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, raw_root.c_str(), "{\"name\":\"Raw queries\"}",
                                   VXCORE_NOTEBOOK_RAW, &raw_id),
            VXCORE_OK);
  VxCoreTagList *raw_tags = nullptr;
  ASSERT_EQ(vxcore_tag_list_typed(ctx, raw_id, &raw_tags), VXCORE_OK);
  ASSERT_NOT_NULL(raw_tags);
  ASSERT_NOT_NULL(vxcore_tag_list_view(raw_tags));
  ASSERT_EQ(vxcore_tag_list_view(raw_tags)->tag_count, 0u);
  char *legacy_json = nullptr;
  ASSERT_EQ(vxcore_tag_list(ctx, raw_id, &legacy_json), VXCORE_OK);
  ASSERT_EQ(std::string(legacy_json), "[]");
  vxcore_string_free(legacy_json);
  legacy_json = nullptr;
  for (const auto *op : {"AND", "OR"}) {
    for (const auto *query : {"[]", "[\"present\"]"}) {
      file_out = files;
      ASSERT_EQ(vxcore_tag_find_files_typed(ctx, raw_id, query, op, &file_out),
                VXCORE_ERR_UNSUPPORTED);
      ASSERT_NULL(file_out);
      ASSERT_EQ(vxcore_tag_find_files(ctx, raw_id, query, op, &legacy_json),
                VXCORE_ERR_UNSUPPORTED);
    }
  }
  count_out = counts;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, raw_id, &count_out), VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(count_out);
  ASSERT_EQ(vxcore_tag_count_files_by_tag(ctx, raw_id, &legacy_json), VXCORE_ERR_UNSUPPORTED);
  vxcore_string_free(legacy_json);

  // Neither successful overwrite nor any error may free an incoming owner.
  file_out = files;
  ASSERT_EQ(vxcore_tag_find_files_typed(ctx, notebook_id, "[]", "OR", &file_out), VXCORE_OK);
  ASSERT_NOT_NULL(file_out);
  ASSERT_NE(file_out, files);
  vxcore_tag_file_result_free(file_out);
  count_out = counts;
  ASSERT_EQ(vxcore_tag_count_files_by_tag_typed(ctx, notebook_id, &count_out), VXCORE_OK);
  ASSERT_NOT_NULL(count_out);
  ASSERT_NE(count_out, counts);
  ASSERT_EQ(assert_tag_counts(vxcore_tag_counts_view(count_out),
                              nlohmann::json::parse(R"([{"tag":"present","count":1}])")),
            0);
  vxcore_tag_counts_free(count_out);
  vxcore_string_free(raw_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  ASSERT_EQ(vxcore_tag_list_view(raw_tags)->tag_count, 0u);
  ASSERT_EQ(assert_tag_files(vxcore_tag_file_result_view(files), expected_files), 0);
  ASSERT_EQ(assert_tag_counts(vxcore_tag_counts_view(counts), nlohmann::json::array()), 0);
  vxcore_tag_list_free(raw_tags);
  vxcore_tag_file_result_free(files);
  vxcore_tag_counts_free(counts);
  cleanup_test_dir(root);
  cleanup_test_dir(raw_root);
  return 0;
}

int main() {
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  std::cout << "Running tag sync tests..." << std::endl;

  // Bundled notebook tests
  std::cout << "\n=== Bundled Notebook Tests ===" << std::endl;
  RUN_TEST(test_notebook_meta_get_set);
  RUN_TEST(test_tags_modified_utc_serialization);
  RUN_TEST(test_tag_operations_update_timestamp);
  RUN_TEST(test_sync_tags_basic);
  RUN_TEST(test_sync_tags_idempotent);
  RUN_TEST(test_sync_tags_parent_ordering);
  RUN_TEST(test_typed_tag_queries_parity_and_lifetime);
  RUN_TEST(test_typed_tag_queries_empty_errors_and_raw);

  // Raw notebook tests
  std::cout << "\n=== Raw Notebook Tests ===" << std::endl;
  RUN_TEST(test_raw_notebook_sync_tags_on_create);
  RUN_TEST(test_raw_notebook_sync_deep_hierarchy_on_create);
  RUN_TEST(test_raw_notebook_tag_operations);

  std::cout << "\n✓ All tag sync tests passed" << std::endl;
  return 0;
}
