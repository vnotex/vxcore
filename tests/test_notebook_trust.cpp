// Trust is a local opened-notebook fact, never notebook-authored configuration.
// Public C API and real session files only. Run serially with other vxcore tests.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

fs::path SessionPath() {
  return fs::temp_directory_path() / "vxcore_test_config" / "local" / "vxsession.json";
}

Json ReadJson(const fs::path &path) {
  std::ifstream file(path);
  if (!file.is_open()) return Json::object();
  auto value = Json::parse(file, nullptr, false);
  return value.is_discarded() ? Json::object() : value;
}

bool WriteJson(const fs::path &path, const Json &value) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << value.dump(2);
  file.close();
  return !file.fail();
}

Json Record(const std::string &id) {
  const auto session = ReadJson(SessionPath());
  if (session.contains("notebooks") && session["notebooks"].is_array()) {
    for (const auto &record : session["notebooks"]) {
      if (record.value("id", std::string()) == id) return record;
    }
  }
  return Json::object();
}

struct Fixture {
  VxCoreContextHandle ctx = nullptr;
  fs::path root = utf8_to_fs_path(get_test_path("notebook_trust"));

  Fixture() {
    vxcore_clear_test_directory();
    cleanup_test_dir(fs_path_to_utf8(root));
    fs::create_directories(root);
  }
  ~Fixture() {
    Stop();
    cleanup_test_dir(fs_path_to_utf8(root));
  }
  void Stop() {
    if (ctx) vxcore_context_destroy(ctx);
    ctx = nullptr;
  }
  VxCoreError Start() { return vxcore_context_create(nullptr, &ctx); }
  VxCoreError Restart() {
    Stop();
    return Start();
  }
  VxCoreError Create(const char *name, VxCoreNotebookType type, std::string &id) {
    char *value = nullptr;
    const auto path = fs_path_to_utf8(root / name);
    const auto result = vxcore_notebook_create(
        ctx, path.c_str(), "{\"name\":\"Trust fixture\",\"trusted\":true}", type, &value);
    if (value) {
      id = value;
      vxcore_string_free(value);
    }
    return result;
  }
  VxCoreError Open(const char *name, std::string &id, bool options = false) {
    char *value = nullptr;
    const auto path = fs_path_to_utf8(root / name);
    const auto result =
        options ? vxcore_notebook_open_ex(ctx, path.c_str(), "{\"trusted\":true}", &value)
                : vxcore_notebook_open(ctx, path.c_str(), &value);
    if (value) {
      id = value;
      vxcore_string_free(value);
    }
    return result;
  }
};

int CheckTrust(VxCoreContextHandle ctx, const std::string &id, bool expected) {
  bool trusted = !expected;
  ASSERT_EQ(vxcore_notebook_is_trusted(ctx, id.c_str(), &trusted), VXCORE_OK);
  ASSERT_EQ(trusted, expected);
  const auto record = Record(id);
  ASSERT(record.contains("trusted"));
  ASSERT(record["trusted"].is_boolean());
  ASSERT_EQ(record["trusted"].get<bool>(), expected);
  return 0;
}

// Keep the original bytes recoverable even when an assertion exits a test.
struct BlockSessionPublication {
  fs::path backup = SessionPath().string() + ".trust-test-backup";
  BlockSessionPublication() {
    fs::rename(SessionPath(), backup);
    fs::create_directory(SessionPath());
    std::ofstream(SessionPath() / "obstruction") << "owned test obstruction";
  }
  ~BlockSessionPublication() {
    std::error_code error;
    fs::remove_all(SessionPath(), error);
    fs::rename(backup, SessionPath(), error);
  }
};

int test_generic_creation_persistence_and_forget() {
  for (const auto type : {VXCORE_NOTEBOOK_BUNDLED, VXCORE_NOTEBOOK_RAW}) {
    Fixture f;
    ASSERT_EQ(f.Start(), VXCORE_OK);
    std::string id;
    ASSERT_EQ(f.Create("book", type, id), VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
    ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
    ASSERT_EQ(vxcore_notebook_update_config(f.ctx, id.c_str(),
                                            "{\"name\":\"Renamed\",\"trusted\":false}"),
              VXCORE_OK);
    ASSERT_EQ(vxcore_notebook_set_read_only(f.ctx, id.c_str(), true), VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
    // Approval and revocation also work on read-only notebooks.
    ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), false), VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
    ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
    ASSERT_EQ(f.Restart(), VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
    std::string reopened;
    ASSERT_EQ(f.Open("book", reopened, true), VXCORE_OK);
    ASSERT_EQ(reopened, id);
    ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
    ASSERT_EQ(vxcore_notebook_close(f.ctx, id.c_str()), VXCORE_OK);
    ASSERT(Record(id).empty());
    if (type == VXCORE_NOTEBOOK_BUNDLED) {
      ASSERT_EQ(f.Open("book", reopened, true), VXCORE_OK);
    } else {
      ASSERT_EQ(f.Create("book", type, reopened), VXCORE_OK);
    }
    ASSERT_EQ(CheckTrust(f.ctx, reopened, false), 0);
  }
  return 0;
}

int test_shared_config_and_options_cannot_grant() {
  Fixture f;
  ASSERT_EQ(f.Start(), VXCORE_OK);
  std::string id;
  ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_close(f.ctx, id.c_str()), VXCORE_OK);
  const auto path = f.root / "book" / "vx_notebook" / "config.json";
  auto config = ReadJson(path);
  ASSERT(config.contains("id"));
  config["trusted"] = true;
  ASSERT(WriteJson(path, config));
  ASSERT_EQ(f.Open("book", id, true), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  ASSERT_EQ(f.Restart(), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  ASSERT_EQ(vxcore_notebook_close(f.ctx, id.c_str()), VXCORE_OK);
  ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  return 0;
}

int test_only_json_booleans_restore_trust() {
  for (const auto &value :
       Json::array({nullptr, "true", 1, 0, Json::object(), Json::array(), false, true})) {
    Fixture f;
    ASSERT_EQ(f.Start(), VXCORE_OK);
    std::string id;
    ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
    f.Stop();
    auto session = ReadJson(SessionPath());
    ASSERT_EQ(session["notebooks"].size(), 1u);
    session["notebooks"][0]["trusted"] = value;
    ASSERT(WriteJson(SessionPath(), session));
    ASSERT_EQ(f.Start(), VXCORE_OK);
    bool trusted = true;
    ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, id.c_str(), &trusted), VXCORE_OK);
    ASSERT_EQ(trusted, value.is_boolean() && value.get<bool>());
    // Force serialization even when loading needed no reconciliation.
    ASSERT_EQ(vxcore_notebook_update_config(f.ctx, id.c_str(), "{\"name\":\"Serialized\"}"),
              VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, id, trusted), 0);
    f.Stop();
    session = ReadJson(SessionPath());
    session["notebooks"][0].erase("trusted");
    ASSERT(WriteJson(SessionPath(), session));
    ASSERT_EQ(f.Start(), VXCORE_OK);
    trusted = true;
    ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, id.c_str(), &trusted), VXCORE_OK);
    ASSERT_FALSE(trusted);
  }
  return 0;
}

int test_copy_at_new_root_does_not_inherit() {
  Fixture f;
  ASSERT_EQ(f.Start(), VXCORE_OK);
  std::string id;
  ASSERT_EQ(f.Create("original", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
  f.Stop();
  fs::copy(f.root / "original", f.root / "copy", fs::copy_options::recursive);
  ASSERT_EQ(f.Start(), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_close(f.ctx, id.c_str()), VXCORE_OK);
  std::string copied;
  ASSERT_EQ(f.Open("copy", copied), VXCORE_OK);
  ASSERT_EQ(copied, id);
  ASSERT_EQ(CheckTrust(f.ctx, copied, false), 0);
  return 0;
}

int test_phantom_recovery_and_identity_guard() {
  // Ordinary explicit open and create-on-existing both recover registered approval.
  for (int scenario = 0; scenario < 3; ++scenario) {
    Fixture f;
    ASSERT_EQ(f.Start(), VXCORE_OK);
    std::string id;
    ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
    ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
    f.Stop();
    fs::rename(f.root / "book", f.root / "unavailable");
    ASSERT_EQ(f.Start(), VXCORE_OK);
    bool trusted = true;
    ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, id.c_str(), &trusted), VXCORE_ERR_NOT_FOUND);
    ASSERT_FALSE(trusted);
    ASSERT_EQ(Record(id)["trusted"], Json(true));
    fs::rename(f.root / "unavailable", f.root / "book");
    if (scenario == 2) {
      auto config = ReadJson(f.root / "book" / "vx_notebook" / "config.json");
      config["id"] = "12345678-aaaa-bbbb-cccc-123456789abc";
      ASSERT(WriteJson(f.root / "book" / "vx_notebook" / "config.json", config));
    }
    std::string recovered;
    ASSERT_EQ(scenario == 1 ? f.Create("book", VXCORE_NOTEBOOK_BUNDLED, recovered)
                            : f.Open("book", recovered),
              VXCORE_OK);
    ASSERT_EQ(CheckTrust(f.ctx, recovered, scenario < 2), 0);
  }
  return 0;
}

int test_noncanonical_record_root_accepts_consent() {
  Fixture f;
  ASSERT_EQ(f.Start(), VXCORE_OK);
  std::string id;
  ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  f.Stop();
  auto session = ReadJson(SessionPath());
  auto root = session["notebooks"][0]["rootFolder"].get<std::string>() + "/.";
#ifdef _WIN32
  std::replace(root.begin(), root.end(), '/', '\\');
#endif
  session["notebooks"][0]["rootFolder"] = root;
  ASSERT(WriteJson(SessionPath(), session));
  ASSERT_EQ(f.Start(), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
  ASSERT_EQ(f.Restart(), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
  return 0;
}

int test_failed_grant_has_no_latent_approval() {
  Fixture f;
  ASSERT_EQ(f.Start(), VXCORE_OK);
  std::string id, other;
  ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  ASSERT_EQ(f.Create("other", VXCORE_NOTEBOOK_BUNDLED, other), VXCORE_OK);
  const auto original = ReadJson(SessionPath());
  {
    BlockSessionPublication obstruction;
    // No-op setters must not attempt a write.
    ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), false), VXCORE_OK);
    ASSERT_NE(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
    bool trusted = true;
    ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, id.c_str(), &trusted), VXCORE_OK);
    ASSERT_FALSE(trusted);
    ASSERT_EQ(ReadJson(obstruction.backup), original);
  }
  ASSERT_EQ(ReadJson(SessionPath()), original);
  // An unrelated successful session write exposes any latent in-memory grant.
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, other.c_str(), true), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  ASSERT_EQ(CheckTrust(f.ctx, other, true), 0);
  ASSERT_EQ(f.Restart(), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  ASSERT_EQ(CheckTrust(f.ctx, other, true), 0);
  return 0;
}

int test_failed_close_retains_notebook_and_record_order() {
  Fixture f;
  ASSERT_EQ(f.Start(), VXCORE_OK);
  std::string before, id, after;
  ASSERT_EQ(f.Create("before", VXCORE_NOTEBOOK_BUNDLED, before), VXCORE_OK);
  ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  ASSERT_EQ(f.Create("after", VXCORE_NOTEBOOK_BUNDLED, after), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, id.c_str(), true), VXCORE_OK);
  const auto records = ReadJson(SessionPath())["notebooks"];
  {
    BlockSessionPublication obstruction;
    ASSERT_NE(vxcore_notebook_close(f.ctx, id.c_str()), VXCORE_OK);
    bool trusted = false;
    ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, id.c_str(), &trusted), VXCORE_OK);
    ASSERT_TRUE(trusted);
    ASSERT_EQ(ReadJson(obstruction.backup)["notebooks"], records);
  }
  // Force another save, then compare all records including their original order.
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, before.c_str(), true), VXCORE_OK);
  auto expected = records;
  for (auto &record : expected) {
    if (record["id"] == before) record["trusted"] = true;
  }
  ASSERT_EQ(ReadJson(SessionPath())["notebooks"], expected);
  ASSERT_EQ(CheckTrust(f.ctx, id, true), 0);
  ASSERT_EQ(vxcore_notebook_close(f.ctx, id.c_str()), VXCORE_OK);
  ASSERT(Record(id).empty());
  ASSERT_EQ(CheckTrust(f.ctx, before, true), 0);
  ASSERT_EQ(CheckTrust(f.ctx, after, false), 0);
  ASSERT_EQ(f.Open("book", id), VXCORE_OK);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  return 0;
}

int test_argument_errors_reset_output() {
  Fixture f;
  ASSERT_EQ(f.Start(), VXCORE_OK);
  std::string id;
  ASSERT_EQ(f.Create("book", VXCORE_NOTEBOOK_BUNDLED, id), VXCORE_OK);
  bool trusted = true;
  ASSERT_EQ(vxcore_notebook_is_trusted(nullptr, id.c_str(), &trusted), VXCORE_ERR_NULL_POINTER);
  ASSERT_FALSE(trusted);
  trusted = true;
  ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, nullptr, &trusted), VXCORE_ERR_NULL_POINTER);
  ASSERT_FALSE(trusted);
  ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, id.c_str(), nullptr), VXCORE_ERR_NULL_POINTER);
  trusted = true;
  ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, "", &trusted), VXCORE_ERR_INVALID_PARAM);
  ASSERT_FALSE(trusted);
  trusted = true;
  ASSERT_EQ(vxcore_notebook_is_trusted(f.ctx, "missing", &trusted), VXCORE_ERR_NOT_FOUND);
  ASSERT_FALSE(trusted);
  ASSERT_EQ(vxcore_notebook_set_trusted(nullptr, id.c_str(), true), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, nullptr, true), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, "", true), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_notebook_set_trusted(f.ctx, "missing", true), VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(CheckTrust(f.ctx, id, false), 0);
  return 0;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_generic_creation_persistence_and_forget);
  RUN_TEST(test_shared_config_and_options_cannot_grant);
  RUN_TEST(test_only_json_booleans_restore_trust);
  RUN_TEST(test_copy_at_new_root_does_not_inherit);
  RUN_TEST(test_phantom_recovery_and_identity_guard);
  RUN_TEST(test_noncanonical_record_root_accepts_consent);
  RUN_TEST(test_failed_grant_has_no_latent_approval);
  RUN_TEST(test_failed_close_retains_notebook_and_record_order);
  RUN_TEST(test_argument_errors_reset_output);
  return 0;
}
