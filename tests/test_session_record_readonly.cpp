#include <iostream>
#include <nlohmann/json.hpp>
#include <fstream>

#include "core/notebook.h"
#include "core/notebook_json_keys.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

using namespace vxcore;

// Forward declare vxcore_set_test_mode (not available outside vxcore DLL)
extern "C" void vxcore_set_test_mode(int enabled);

// Subtest 1: Round-trip with read_only=true
int test_roundtrip_read_only_true() {
  std::cout << "  Running test_roundtrip_read_only_true..." << std::endl;

  NotebookRecord original;
  original.id = "test-nb-1";
  original.root_folder = "/home/user/notebook";
  original.type = NotebookType::Bundled;
  original.read_only = true;

  // Convert to JSON
  nlohmann::json json = original.ToJson();

  // Verify the key is present
  ASSERT(json.contains(kJsonKeyReadOnly));
  ASSERT_EQ(json[kJsonKeyReadOnly].get<bool>(), true);

  // Convert back from JSON
  NotebookRecord restored = NotebookRecord::FromJson(json);

  // Verify all fields preserved
  ASSERT_EQ(restored.id, "test-nb-1");
  ASSERT_EQ(restored.root_folder, "/home/user/notebook");
  ASSERT(restored.type == NotebookType::Bundled);
  ASSERT_EQ(restored.read_only, true);

  std::cout << "  ✓ test_roundtrip_read_only_true passed" << std::endl;
  return 0;
}

// Subtest 2: Round-trip with read_only=false
int test_roundtrip_read_only_false() {
  std::cout << "  Running test_roundtrip_read_only_false..." << std::endl;

  NotebookRecord original;
  original.id = "test-nb-2";
  original.root_folder = "/home/user/another";
  original.type = NotebookType::Raw;
  original.read_only = false;

  // Convert to JSON
  nlohmann::json json = original.ToJson();

  // Verify the key is present
  ASSERT(json.contains(kJsonKeyReadOnly));
  ASSERT_EQ(json[kJsonKeyReadOnly].get<bool>(), false);

  // Convert back from JSON
  NotebookRecord restored = NotebookRecord::FromJson(json);

  // Verify all fields preserved
  ASSERT_EQ(restored.id, "test-nb-2");
  ASSERT_EQ(restored.root_folder, "/home/user/another");
  ASSERT(restored.type == NotebookType::Raw);
  ASSERT_EQ(restored.read_only, false);

  std::cout << "  ✓ test_roundtrip_read_only_false passed" << std::endl;
  return 0;
}

// Subtest 3: Backward compat - JSON without readOnly key defaults to false
int test_backward_compat_missing_field() {
  std::cout << "  Running test_backward_compat_missing_field..." << std::endl;

  // Construct JSON WITHOUT the readOnly key (old format)
  nlohmann::json json = nlohmann::json::object();
  json["id"] = "old-notebook";
  json["rootFolder"] = "/path/to/old";
  json["type"] = "bundled";
  // Intentionally omit kJsonKeyReadOnly

  // Load from JSON
  NotebookRecord record = NotebookRecord::FromJson(json);

  // Verify fields loaded
  ASSERT_EQ(record.id, "old-notebook");
  ASSERT_EQ(record.root_folder, "/path/to/old");
  ASSERT(record.type == NotebookType::Bundled);

  // Verify read_only defaulted to false
  ASSERT_EQ(record.read_only, false);

  std::cout << "  ✓ test_backward_compat_missing_field passed" << std::endl;
  return 0;
}

// Subtest 4: Backward compat - load from on-disk fixture JSON file
int test_backward_compat_from_fixture_file() {
  std::cout << "  Running test_backward_compat_from_fixture_file..." << std::endl;

  // Create temp directory for the fixture
  std::string fixture_dir = get_test_path("test_record_readonly");
  cleanup_test_dir(fixture_dir);
  create_directory(fixture_dir);

  // Create a fixture JSON file (simulating old session.json without readOnly)
  std::string fixture_file = fixture_dir + "/fixture_session.json";
  nlohmann::json session_json = nlohmann::json::object();
  session_json["openNotebooks"] = nlohmann::json::array();

  // Add a notebook record WITHOUT the readOnly field (old format)
  nlohmann::json notebook_record = nlohmann::json::object();
  notebook_record["id"] = "fixture-nb";
  notebook_record["rootFolder"] = "/fixtures/notebook";
  notebook_record["type"] = "raw";
  // Intentionally omit kJsonKeyReadOnly

  session_json["openNotebooks"].push_back(notebook_record);

  // Write fixture to file
  std::ofstream file(utf8_to_fs_path(fixture_file));
  ASSERT(file.is_open());
  file << session_json.dump();
  file.close();

  // Read the fixture file back
  std::ifstream infile(utf8_to_fs_path(fixture_file));
  ASSERT(infile.is_open());
  nlohmann::json loaded_session = nlohmann::json::parse(infile);
  infile.close();

  // Parse the first notebook record from the fixture
  ASSERT(loaded_session.contains("openNotebooks"));
  ASSERT(loaded_session["openNotebooks"].is_array());
  ASSERT_EQ(loaded_session["openNotebooks"].size(), 1);

  nlohmann::json record_json = loaded_session["openNotebooks"][0];
  NotebookRecord record = NotebookRecord::FromJson(record_json);

  // Verify fields loaded from fixture
  ASSERT_EQ(record.id, "fixture-nb");
  ASSERT_EQ(record.root_folder, "/fixtures/notebook");
  ASSERT(record.type == NotebookType::Raw);

  // Verify read_only defaulted to false (backward compat)
  ASSERT_EQ(record.read_only, false);

  // Cleanup
  cleanup_test_dir(fixture_dir);

  std::cout << "  ✓ test_backward_compat_from_fixture_file passed" << std::endl;
  return 0;
}

// Subtest 5: T15 -- LoadOpenNotebooks re-applies per-device read-only flag.
//
// End-to-end: open two distinct bundled notebooks via the C ABI, flip the
// first one to read-only via the new vxcore_notebook_open_ex
// {"readOnly":true} options path (T14 persists read_only into the matching
// NotebookRecord), then tear down the context and rebuild it. The fresh
// context's NotebookManager constructor calls LoadOpenNotebooks, which T15
// taught to honour the persisted record.read_only flag. We verify the
// runtime IsReadOnly() state is back to what was persisted for each
// notebook (true for #1, false for #2).
int test_load_open_notebooks_restores_readonly() {
  std::cout << "  Running test_load_open_notebooks_restores_readonly..." << std::endl;

  // Wipe session config so the loaded set is precisely the two we create
  // in this subtest, with no leftover notebooks from other subtests
  // polluting the LoadOpenNotebooks iteration.
  vxcore_clear_test_directory();

  const std::string nb_ro_path = get_test_path("t15_session_ro_true");
  const std::string nb_rw_path = get_test_path("t15_session_ro_false");
  cleanup_test_dir(nb_ro_path);
  cleanup_test_dir(nb_rw_path);

  // -- First context: create + persist both notebooks with different RO state.
  VxCoreContextHandle ctx_a = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx_a), VXCORE_OK);
  ASSERT_NOT_NULL(ctx_a);

  char *id_ro_seed = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx_a, nb_ro_path.c_str(),
                                   "{\"name\":\"T15 RO Restored\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &id_ro_seed),
            VXCORE_OK);
  ASSERT_NOT_NULL(id_ro_seed);
  // Detach so we can re-open via open_ex with the readOnly:true option,
  // which is the path that persists the RO flag through NotebookRecord.
  ASSERT_EQ(vxcore_notebook_close(ctx_a, id_ro_seed), VXCORE_OK);
  vxcore_string_free(id_ro_seed);

  char *id_ro_opened = nullptr;
  ASSERT_EQ(vxcore_notebook_open_ex(ctx_a, nb_ro_path.c_str(), "{\"readOnly\":true}",
                                     &id_ro_opened),
            VXCORE_OK);
  ASSERT_NOT_NULL(id_ro_opened);
  bool ro_check = false;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx_a, id_ro_opened, &ro_check), VXCORE_OK);
  ASSERT_TRUE(ro_check);
  std::string id_ro_str(id_ro_opened);
  vxcore_string_free(id_ro_opened);

  // Second notebook: create and leave writable (no read_only edit).
  char *id_rw = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx_a, nb_rw_path.c_str(),
                                   "{\"name\":\"T15 RW Restored\"}", VXCORE_NOTEBOOK_BUNDLED,
                                   &id_rw),
            VXCORE_OK);
  ASSERT_NOT_NULL(id_rw);
  ro_check = true;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx_a, id_rw, &ro_check), VXCORE_OK);
  ASSERT_FALSE(ro_check);
  std::string id_rw_str(id_rw);
  vxcore_string_free(id_rw);

  // Tear down context A. ConfigManager flushes session.json on destruction;
  // both NotebookRecord entries should now be on disk with their respective
  // read_only flags.
  vxcore_context_destroy(ctx_a);

  // -- Second context: LoadOpenNotebooks runs from the constructor and must
  // re-apply each record.read_only via notebook->SetReadOnly. Without T15
  // the runtime flag would default to false for the RO notebook (i.e. the
  // bug this subtest guards against).
  VxCoreContextHandle ctx_b = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx_b), VXCORE_OK);
  ASSERT_NOT_NULL(ctx_b);

  bool ro_after = false;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx_b, id_ro_str.c_str(), &ro_after), VXCORE_OK);
  ASSERT_TRUE(ro_after);  // T15: persisted true -> restored true.

  bool rw_after = true;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx_b, id_rw_str.c_str(), &rw_after), VXCORE_OK);
  ASSERT_FALSE(rw_after);  // T15: persisted false -> restored false.

  vxcore_context_destroy(ctx_b);
  cleanup_test_dir(nb_ro_path);
  cleanup_test_dir(nb_rw_path);
  std::cout << "  ✓ test_load_open_notebooks_restores_readonly passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running NotebookRecord read_only field tests..." << std::endl;

  // Run all 5 subtests in order
  RUN_TEST(test_roundtrip_read_only_true);
  RUN_TEST(test_roundtrip_read_only_false);
  RUN_TEST(test_backward_compat_missing_field);
  RUN_TEST(test_backward_compat_from_fixture_file);
  RUN_TEST(test_load_open_notebooks_restores_readonly);

  std::cout << "✓ All NotebookRecord read_only tests passed!" << std::endl;
  return 0;
}

