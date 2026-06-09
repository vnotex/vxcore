// T14 of open-notebook-remote-readonly plan: vxcore_notebook_open_ex.
// Verifies:
//   1. open_ex(path, "{}") returns id; opened notebook IsReadOnly()==false;
//      NotebookRecord persisted with readOnly=false.
//   2. open_ex(path, "{\"readOnly\":true}") returns id; IsReadOnly()==true;
//      NotebookRecord persisted with readOnly=true in session.json.
//   3. Back-compat: legacy vxcore_notebook_open(path) STILL works and
//      IsReadOnly()==false (shim defaults read_only=false).
//   4. Malformed options_json (e.g. "not-json") returns VXCORE_ERR_JSON_PARSE
//      AND leaves *out_notebook_id == nullptr (no notebook registered).
//
// Test isolation: vxcore_set_test_mode(1) redirects AppData → %TEMP%/
// vxcore_test_config + %TEMP%/vxcore_test_data per the standard pattern
// documented in libs/vxcore/AGENTS.md § Test isolation on Windows. Each
// subtest creates its own notebook root directory and cleans up after itself.

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vxcore/notebook_json_keys.h>

#include "test_utils.h"
#include "vxcore/vxcore.h"

using namespace vxcore;

namespace {

// Path to the on-disk session.json file. ConfigManager writes it to the
// per-test local data path which, in test mode, equals
// %TEMP%/vxcore_test_config/vxsession.json.
std::string SessionConfigPath() {
  return fs_path_to_utf8(std::filesystem::temp_directory_path() / "vxcore_test_config" /
                          "vxsession.json");
}

// Load session.json and locate the NotebookRecord for the given notebook ID.
// Returns the JSON object, or an empty object on lookup failure (caller's
// ASSERT() should treat that as a failure). Uses the no-throw parse overload
// to avoid the MSVC + nlohmann exception-unwind quirk that wedges the test
// process when the throwing parse path runs out of vxcore.dll.
nlohmann::json FindRecordInSession(const std::string &notebook_id) {
  std::ifstream file(utf8_to_fs_path(SessionConfigPath()));
  if (!file.is_open()) {
    return nlohmann::json::object();
  }
  nlohmann::json session =
      nlohmann::json::parse(file, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (session.is_discarded()) {
    return nlohmann::json::object();
  }
  if (!session.contains("notebooks") || !session["notebooks"].is_array()) {
    return nlohmann::json::object();
  }
  for (const auto &entry : session["notebooks"]) {
    if (entry.contains("id") && entry["id"].is_string() &&
        entry["id"].get<std::string>() == notebook_id) {
      return entry;
    }
  }
  return nlohmann::json::object();
}

// Subtest 1: open_ex with empty options → readOnly defaults to false.
int test_open_ex_empty_options_defaults_false() {
  std::cout << "  Running test_open_ex_empty_options_defaults_false..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("test_open_ex_empty");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  // Bootstrap a bundled notebook on disk so open_ex has something to open.
  char *create_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Empty Opts\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &create_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(create_id);

  // Close + re-open with the new API + "{}" options.
  ASSERT_EQ(vxcore_notebook_close(ctx, create_id), VXCORE_OK);
  vxcore_string_free(create_id);

  char *open_id = nullptr;
  ASSERT_EQ(vxcore_notebook_open_ex(ctx, nb_path.c_str(), "{}", &open_id), VXCORE_OK);
  ASSERT_NOT_NULL(open_id);

  bool is_ro = true;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx, open_id, &is_ro), VXCORE_OK);
  ASSERT_FALSE(is_ro);

  // Verify the persisted record echoes the runtime flag.
  nlohmann::json record = FindRecordInSession(open_id);
  ASSERT(record.contains("id"));
  ASSERT(record.contains(kJsonKeyReadOnly));
  ASSERT_EQ(record[kJsonKeyReadOnly].get<bool>(), false);

  vxcore_string_free(open_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_open_ex_empty_options_defaults_false passed" << std::endl;
  return 0;
}

// Subtest 2: open_ex with {"readOnly":true} → flag applied + persisted.
int test_open_ex_readonly_true_persists() {
  std::cout << "  Running test_open_ex_readonly_true_persists..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("test_open_ex_ro_true");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *create_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"RO True\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &create_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(create_id);

  ASSERT_EQ(vxcore_notebook_close(ctx, create_id), VXCORE_OK);
  vxcore_string_free(create_id);

  char *open_id = nullptr;
  ASSERT_EQ(vxcore_notebook_open_ex(ctx, nb_path.c_str(), "{\"readOnly\":true}", &open_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(open_id);

  bool is_ro = false;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx, open_id, &is_ro), VXCORE_OK);
  ASSERT_TRUE(is_ro);

  // The whole point of this subtest: the runtime flag must round-trip through
  // session.json so a downstream restart can re-apply it (T15 closes that
  // loop end-to-end).
  nlohmann::json record = FindRecordInSession(open_id);
  ASSERT(record.contains("id"));
  ASSERT(record.contains(kJsonKeyReadOnly));
  ASSERT_EQ(record[kJsonKeyReadOnly].get<bool>(), true);

  vxcore_string_free(open_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_open_ex_readonly_true_persists passed" << std::endl;
  return 0;
}

// Subtest 3: legacy vxcore_notebook_open path still works (back-compat shim).
int test_legacy_open_still_works() {
  std::cout << "  Running test_legacy_open_still_works..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("test_legacy_open");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *create_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Legacy\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &create_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(create_id);

  ASSERT_EQ(vxcore_notebook_close(ctx, create_id), VXCORE_OK);
  vxcore_string_free(create_id);

  // Old entry point — must produce the same observable behavior as the
  // pre-T14 callers depend on.
  char *open_id = nullptr;
  ASSERT_EQ(vxcore_notebook_open(ctx, nb_path.c_str(), &open_id), VXCORE_OK);
  ASSERT_NOT_NULL(open_id);

  bool is_ro = true;
  ASSERT_EQ(vxcore_notebook_is_read_only(ctx, open_id, &is_ro), VXCORE_OK);
  ASSERT_FALSE(is_ro);

  // The shim also persists, so the session record must reflect the default.
  nlohmann::json record = FindRecordInSession(open_id);
  ASSERT(record.contains(kJsonKeyReadOnly));
  ASSERT_EQ(record[kJsonKeyReadOnly].get<bool>(), false);

  vxcore_string_free(open_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_legacy_open_still_works passed" << std::endl;
  return 0;
}

// Subtest 4: malformed JSON in options → VXCORE_ERR_JSON_PARSE and no
// notebook is registered (out parameter stays nullptr).
int test_open_ex_malformed_json_rejects() {
  std::cout << "  Running test_open_ex_malformed_json_rejects..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("test_open_ex_malformed");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  // Even create + close it first so a successful path would have something
  // valid to open — proving the failure really IS the parse rejection.
  char *create_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"Malformed\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &create_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(create_id);
  ASSERT_EQ(vxcore_notebook_close(ctx, create_id), VXCORE_OK);
  vxcore_string_free(create_id);

  // Pre-seed the out-pointer with a sentinel so we can prove the C API
  // clears it on failure (the documented "no notebook registered" contract).
  char *sentinel = const_cast<char *>("sentinel-not-cleared");
  char *open_id = sentinel;
  VxCoreError err = vxcore_notebook_open_ex(ctx, nb_path.c_str(), "not-json", &open_id);
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);
  ASSERT_NULL(open_id);

  // Also exercise the "valid JSON but not an object" failure mode (top-level
  // array). Same contract: parse error reported AND out_notebook_id cleared.
  open_id = sentinel;
  err = vxcore_notebook_open_ex(ctx, nb_path.c_str(), "[]", &open_id);
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);
  ASSERT_NULL(open_id);

  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_open_ex_malformed_json_rejects passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  std::cout << "Running vxcore_notebook_open_ex tests..." << std::endl;

  RUN_TEST(test_open_ex_empty_options_defaults_false);
  RUN_TEST(test_open_ex_readonly_true_persists);
  RUN_TEST(test_legacy_open_still_works);
  RUN_TEST(test_open_ex_malformed_json_rejects);

  std::cout << "\xE2\x9C\x93 All vxcore_notebook_open_ex tests passed!" << std::endl;
  return 0;
}
