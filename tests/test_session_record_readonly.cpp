#include <iostream>
#include <nlohmann/json.hpp>
#include <fstream>

#include "core/notebook.h"
#include "core/notebook_json_keys.h"
#include "test_utils.h"

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

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running NotebookRecord read_only field tests..." << std::endl;

  // Run all 4 subtests in order
  RUN_TEST(test_roundtrip_read_only_true);
  RUN_TEST(test_roundtrip_read_only_false);
  RUN_TEST(test_backward_compat_missing_field);
  RUN_TEST(test_backward_compat_from_fixture_file);

  std::cout << "✓ All NotebookRecord read_only tests passed!" << std::endl;
  return 0;
}

