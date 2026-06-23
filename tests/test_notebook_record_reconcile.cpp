// Part B of fix-notebook-reappears-id-mismatch: regression test proving the
// "closed notebook reappears after restart" bug stays fixed. Exercises the
// Part A defense-in-depth fix in NotebookManager (A1 create-on-existing id
// preservation, A2 close-by-id-OR-root, A4 load-time id reconcile + same-root
// dedupe) through the PUBLIC C API only. Context destroy + recreate == a
// "restart" (the new context re-runs LoadOpenNotebooks).
//
// Each subtest is designed to FAIL without its corresponding A-site fix:
//   (a) reappear regression  -> A4 reconcile + A2 close
//   (b) same-root dedupe      -> A4 dedupe
//   (c) create-on-existing    -> A1 id preservation
//   (d) happy-path guard      -> ensures no spurious resurrection
//
// Test isolation: vxcore_set_test_mode(1) redirects AppData -> %TEMP%/
// vxcore_test_config + %TEMP%/vxcore_test_data. The on-disk session file lives
// at %TEMP%/vxcore_test_config/local/vxsession.json. JSON files are ALWAYS read
// with the non-throwing nlohmann parse overload — the throwing path wedges the
// test process across the MSVC + nlohmann DLL boundary. JSON keys are spelled
// as RAW string literals on purpose (the test_json_key_drift gate excludes test
// dirs; literals here detect drift against the production writer).

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

// Absolute path to the on-disk session file. In test mode ConfigManager writes
// it to %TEMP%/vxcore_test_config/local/vxsession.json.
std::string SessionConfigPath() {
  return fs_path_to_utf8(std::filesystem::temp_directory_path() / "vxcore_test_config" / "local" /
                         "vxsession.json");
}

// Load the session JSON via the non-throwing parse overload. Returns an empty
// object on any failure (missing file, malformed JSON).
nlohmann::json LoadSession() {
  std::ifstream file(utf8_to_fs_path(SessionConfigPath()));
  if (!file.is_open()) {
    return nlohmann::json::object();
  }
  nlohmann::json session = nlohmann::json::parse(file, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (session.is_discarded()) {
    return nlohmann::json::object();
  }
  return session;
}

// Overwrite the session file with the given JSON (pretty-printed).
void WriteSession(const nlohmann::json &session) {
  std::ofstream file(utf8_to_fs_path(SessionConfigPath()), std::ios::binary);
  file << session.dump(2);
}

// Count how many session records have rootFolder == cleaned_root.
int CountRecordsForRoot(const std::string &cleaned_root) {
  nlohmann::json session = LoadSession();
  if (!session.contains("notebooks") || !session["notebooks"].is_array()) {
    return 0;
  }
  int count = 0;
  for (const auto &entry : session["notebooks"]) {
    if (entry.contains("rootFolder") && entry["rootFolder"].is_string() &&
        entry["rootFolder"].get<std::string>() == cleaned_root) {
      ++count;
    }
  }
  return count;
}

// Return the id of the FIRST session record whose rootFolder == cleaned_root,
// or empty string if none.
std::string RecordIdForRoot(const std::string &cleaned_root) {
  nlohmann::json session = LoadSession();
  if (!session.contains("notebooks") || !session["notebooks"].is_array()) {
    return std::string();
  }
  for (const auto &entry : session["notebooks"]) {
    if (entry.contains("rootFolder") && entry["rootFolder"].is_string() &&
        entry["rootFolder"].get<std::string>() == cleaned_root) {
      if (entry.contains("id") && entry["id"].is_string()) {
        return entry["id"].get<std::string>();
      }
    }
  }
  return std::string();
}

// Capture the EXACT stored rootFolder string for a given notebook id by reading
// the session file. Using the persisted string (rather than re-deriving
// CleanPath ourselves) avoids any path-normalization mismatch in comparisons.
std::string StoredRootForId(const std::string &notebook_id) {
  nlohmann::json session = LoadSession();
  if (!session.contains("notebooks") || !session["notebooks"].is_array()) {
    return std::string();
  }
  for (const auto &entry : session["notebooks"]) {
    if (entry.contains("id") && entry["id"].is_string() &&
        entry["id"].get<std::string>() == notebook_id) {
      if (entry.contains("rootFolder") && entry["rootFolder"].is_string()) {
        return entry["rootFolder"].get<std::string>();
      }
    }
  }
  return std::string();
}

// Absolute on-disk path of a bundled notebook's config.json.
std::string ConfigJsonPath(const std::string &nb_path) {
  return fs_path_to_utf8(utf8_to_fs_path(nb_path) / "vx_notebook" / "config.json");
}

// Load a bundled notebook's config.json (non-throwing parse).
nlohmann::json LoadConfigJson(const std::string &nb_path) {
  std::ifstream file(utf8_to_fs_path(ConfigJsonPath(nb_path)));
  if (!file.is_open()) {
    return nlohmann::json::object();
  }
  nlohmann::json j = nlohmann::json::parse(file, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    return nlohmann::json::object();
  }
  return j;
}

// Overwrite a bundled notebook's config.json (pretty-printed).
void WriteConfigJson(const std::string &nb_path, const nlohmann::json &j) {
  std::ofstream file(utf8_to_fs_path(ConfigJsonPath(nb_path)), std::ios::binary);
  file << j.dump(2);
}

// Convenience wrapper around vxcore_notebook_list returning the JSON string.
std::string ListJson(VxCoreContextHandle ctx) {
  char *list = nullptr;
  if (vxcore_notebook_list(ctx, &list) != VXCORE_OK || list == nullptr) {
    return std::string();
  }
  std::string out(list);
  vxcore_string_free(list);
  return out;
}

// True if vxcore_notebook_list reports an entry with rootFolder == cleaned_root.
bool ListContainsRoot(VxCoreContextHandle ctx, const std::string &cleaned_root) {
  nlohmann::json j =
      nlohmann::json::parse(ListJson(ctx), /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_array()) {
    return false;
  }
  for (const auto &entry : j) {
    if (entry.contains("rootFolder") && entry["rootFolder"].is_string() &&
        entry["rootFolder"].get<std::string>() == cleaned_root) {
      return true;
    }
  }
  return false;
}

// (a) Reappear-bug regression (A4 reconcile + A2 close-by-root).
//
// Create a bundled notebook, then simulate OneDrive overwriting config.json
// with a different id (the session record keeps the OLD id). On restart, A4
// must reconcile the stale record id to the config id; close must then succeed
// (A2 matches by root even though the runtime/config id differs from the
// original record id) and the notebook must NOT reappear on the next restart.
int test_reappear_after_id_drift() {
  std::cout << "  Running test_reappear_after_id_drift..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("reconcile_reappear");
  cleanup_test_dir(nb_path);

  // 1. Create the notebook -> id1.
  VxCoreContextHandle ctx1 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx1), VXCORE_OK);
  char *id1 = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx1, nb_path.c_str(), "{\"name\":\"Drift\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id1),
            VXCORE_OK);
  ASSERT_NOT_NULL(id1);
  const std::string sid1(id1);
  vxcore_string_free(id1);

  // Capture the EXACT stored rootFolder string for all later comparisons.
  const std::string root = StoredRootForId(sid1);
  ASSERT_FALSE(root.empty());

  // 2. On disk, flip config.json's id to a different valid-looking UUID. Leave
  //    the session record untouched (it still holds sid1).
  const std::string id2 = "00000000-aaaa-bbbb-cccc-000000000001";
  nlohmann::json cfg = LoadConfigJson(nb_path);
  ASSERT(cfg.contains("id"));
  ASSERT_EQ(cfg["id"].get<std::string>(), sid1);
  cfg["id"] = id2;
  WriteConfigJson(nb_path, cfg);

  // 3. "Restart": destroy + recreate the context (re-runs LoadOpenNotebooks).
  vxcore_context_destroy(ctx1);
  VxCoreContextHandle ctx2 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx2), VXCORE_OK);

  // 5. The session now has exactly ONE record for this root, reconciled to id2.
  ASSERT_EQ(CountRecordsForRoot(root), 1);
  ASSERT_EQ(RecordIdForRoot(root), id2);

  // 6. Close by the reconciled id -> success.
  ASSERT_EQ(vxcore_notebook_close(ctx2, id2.c_str()), VXCORE_OK);

  // 7. Restart once more: the notebook must stay closed.
  vxcore_context_destroy(ctx2);
  VxCoreContextHandle ctx3 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx3), VXCORE_OK);
  ASSERT_FALSE(ListContainsRoot(ctx3, root));
  ASSERT_EQ(CountRecordsForRoot(root), 0);
  vxcore_context_destroy(ctx3);

  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_reappear_after_id_drift passed" << std::endl;
  return 0;
}

// (b) Same-root dedupe (A4). Two session records pointing at the same root must
// collapse to ONE on restart, and the survivor's id must be the real on-disk
// config id (not the bogus clone). Without A4 dedupe BOTH records survive.
int test_same_root_dedupe() {
  std::cout << "  Running test_same_root_dedupe..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("reconcile_dedupe");
  cleanup_test_dir(nb_path);

  // 1. Create -> id1, then close the context so exactly one record persists.
  VxCoreContextHandle ctx1 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx1), VXCORE_OK);
  char *id1 = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx1, nb_path.c_str(), "{\"name\":\"Dedupe\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id1),
            VXCORE_OK);
  ASSERT_NOT_NULL(id1);
  const std::string sid1(id1);
  vxcore_string_free(id1);
  const std::string root = StoredRootForId(sid1);
  ASSERT_FALSE(root.empty());
  vxcore_context_destroy(ctx1);

  // 2. Inject a SECOND record for the SAME root with a bogus id.
  nlohmann::json session = LoadSession();
  ASSERT(session.contains("notebooks"));
  ASSERT(session["notebooks"].is_array());
  nlohmann::json clone;
  bool found = false;
  for (const auto &entry : session["notebooks"]) {
    if (entry.contains("rootFolder") && entry["rootFolder"].is_string() &&
        entry["rootFolder"].get<std::string>() == root) {
      clone = entry;  // copy the original record verbatim
      found = true;
      break;
    }
  }
  ASSERT(found);
  clone["id"] = "11111111-dead-beef-cafe-111111111111";
  session["notebooks"].push_back(clone);
  WriteSession(session);
  ASSERT_EQ(CountRecordsForRoot(root), 2);  // sanity: two duplicates seeded

  // 3. Restart -> LoadOpenNotebooks dedupes to a single canonical record.
  VxCoreContextHandle ctx2 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx2), VXCORE_OK);

  // 4. Exactly one record survives, with the real config id (sid1).
  ASSERT_EQ(CountRecordsForRoot(root), 1);
  ASSERT_EQ(RecordIdForRoot(root), sid1);
  vxcore_context_destroy(ctx2);

  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_same_root_dedupe passed" << std::endl;
  return 0;
}

// (c) Create-on-existing preserves id (A1). Re-creating a bundled notebook at a
// root that still holds a config.json must reuse the existing id rather than
// minting a fresh one and truncate-overwriting config.json.
int test_create_on_existing_preserves_id() {
  std::cout << "  Running test_create_on_existing_preserves_id..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("reconcile_recreate");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx1 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx1), VXCORE_OK);

  // 1. Create -> id1, then close (removes the record; config.json survives).
  char *id1 = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx1, nb_path.c_str(), "{\"name\":\"Readd\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id1),
            VXCORE_OK);
  ASSERT_NOT_NULL(id1);
  const std::string sid1(id1);
  ASSERT_EQ(vxcore_notebook_close(ctx1, id1), VXCORE_OK);
  vxcore_string_free(id1);

  // 2. Create again at the SAME root -> id must be preserved, not re-minted.
  char *id_again = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx1, nb_path.c_str(), "{\"name\":\"Readd\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id_again),
            VXCORE_OK);
  ASSERT_NOT_NULL(id_again);
  ASSERT_EQ(std::string(id_again), sid1);
  vxcore_string_free(id_again);

  // 3. config.json on disk still carries the original id (not overwritten).
  nlohmann::json cfg = LoadConfigJson(nb_path);
  ASSERT(cfg.contains("id"));
  ASSERT_EQ(cfg["id"].get<std::string>(), sid1);
  vxcore_context_destroy(ctx1);

  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_create_on_existing_preserves_id passed" << std::endl;
  return 0;
}

// (d) Happy-path guard: a cleanly-matching create -> close -> restart leaves the
// notebook closed (no spurious session rewrite resurrects it).
int test_close_then_restart_stays_closed() {
  std::cout << "  Running test_close_then_restart_stays_closed..." << std::endl;
  vxcore_clear_test_directory();
  const std::string nb_path = get_test_path("reconcile_happy");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx1 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx1), VXCORE_OK);
  char *id1 = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx1, nb_path.c_str(), "{\"name\":\"Happy\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id1),
            VXCORE_OK);
  ASSERT_NOT_NULL(id1);
  const std::string root = StoredRootForId(std::string(id1));
  ASSERT_FALSE(root.empty());
  ASSERT_EQ(vxcore_notebook_close(ctx1, id1), VXCORE_OK);
  vxcore_string_free(id1);
  vxcore_context_destroy(ctx1);

  // Restart: list must not contain the closed notebook.
  VxCoreContextHandle ctx2 = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx2), VXCORE_OK);
  ASSERT_FALSE(ListContainsRoot(ctx2, root));
  ASSERT_EQ(CountRecordsForRoot(root), 0);
  vxcore_context_destroy(ctx2);

  cleanup_test_dir(nb_path);
  std::cout << "    \xE2\x9C\x93 test_close_then_restart_stays_closed passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  std::cout << "Running vxcore notebook record reconcile tests..." << std::endl;

  RUN_TEST(test_reappear_after_id_drift);
  RUN_TEST(test_same_root_dedupe);
  RUN_TEST(test_create_on_existing_preserves_id);
  RUN_TEST(test_close_then_restart_stays_closed);

  std::cout << "\xE2\x9C\x93 All vxcore notebook record reconcile tests passed!" << std::endl;
  return 0;
}
