// Opt-in real-server smoke. The caller supplies a fresh, owned remote collection
// and an isolated local workspace through the environment; no secrets in argv.
#include <sodium.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "sync/credential_provider.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"
#include "vxcore/vxcore.h"

namespace {
namespace fs = std::filesystem;
using namespace vxcore;
using Json = nlohmann::json;

std::string Environment(const char *name) {
  const auto *value = std::getenv(name);
  return value ? value : "";
}

void Require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void Check(VxCoreError result, const char *operation, ISyncBackend *backend = nullptr) {
  if (result == VXCORE_OK) return;
  std::string message = std::string(operation) + " failed (" + std::to_string(result) + ")";
  if (backend) message += ": " + backend->GetLastError();
  throw std::runtime_error(message);
}

void Write(const fs::path &root, const std::string &relative, const std::string &bytes) {
  const auto path = root / PathFromUtf8(relative);
  fs::create_directories(path.parent_path());
  Check(WriteFileAtomic(path, bytes), "write owned local file");
}

std::string Read(const fs::path &root, const std::string &relative) {
  std::ifstream input(root / PathFromUtf8(relative), std::ios::binary);
  Require(input.is_open(), "expected local file is missing");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string Hash(const std::string &bytes) {
  unsigned char digest[crypto_hash_sha256_BYTES];
  char hex[crypto_hash_sha256_BYTES * 2 + 1];
  crypto_hash_sha256(digest, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
  sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
  return hex;
}

struct Context {
  VxCoreContextHandle value = nullptr;
  std::string id;
  Context() { Check(vxcore_context_create(nullptr, &value), "create isolated context"); }
  ~Context() {
    if (!id.empty()) vxcore_notebook_close(value, id.c_str());
    if (value) vxcore_context_destroy(value);
  }
};

struct Client {
  fs::path root;
  SyncConfig config;
  std::shared_ptr<ICredentialProvider> credentials;
  std::unique_ptr<ISyncBackend> backend;

  void Construct() {
    backend = SyncBackendRegistry::Instance().Create(config.backend, config, credentials);
    Require(backend != nullptr, "backend was not registered");
  }
  void Attach() {
    Construct();
    Check(backend->Initialize(PathToUtf8(root), config), "initialize backend", backend.get());
  }
  void Sync() { Check(backend->Sync(nullptr, nullptr), "sync", backend.get()); }
  void Clone() {
    Require(fs::is_empty(root), "clone destination is not empty");
    Construct();
    Check(backend->Clone(PathToUtf8(root), config), "clone", backend.get());
    Attach();
  }
};

Json NoteDigests(const fs::path &root) {
  Json result = Json::object();
  for (const auto &entry : fs::directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".md") {
      const auto name = PathToUtf8(entry.path().filename());
      result[name] = Hash(Read(root, name));
    }
  }
  return result;
}

int Run() {
  const auto backend_name = Environment("VXCORE_LIVE_BACKEND");
  const auto url = Environment("VXCORE_LIVE_URL");
  const auto local = Environment("VXCORE_LIVE_CLIENT_ROOT");
  const auto username = Environment("VXCORE_LIVE_USERNAME");
  const auto password = Environment("VXCORE_LIVE_PASSWORD");
  Require(backend_name == "webdav" || backend_name == "jianguoyun", "select a live backend");
  Require(!url.empty() && !local.empty() && !username.empty() && !password.empty(),
          "required live-smoke environment is missing");
  Require(Environment("VXCORE_LIVE_OWNED_COLLECTION") == "1",
          "caller must provision a fresh disposable remote collection");
  const auto workspace = PathFromUtf8(local);
  SyncConfig config;
  config.backend = backend_name;
  config.remote_url = url;
  SyncCredentials credential;
  credential.extra = {{kJsonKeyUsername, username}, {kJsonKeyPassword, password}};
  auto provider = std::make_shared<InMemoryCredentialProvider>(credential);
  const auto action = Environment("VXCORE_LIVE_ACTION");
  Require(action.empty() || action == "sync-existing", "unknown live smoke action");
  if (action == "sync-existing") {
    const auto expected_id = Environment("VXCORE_LIVE_EXPECTED_NOTEBOOK_ID");
    Require(!expected_id.empty() && fs::is_directory(workspace),
            "owned notebook identity is required");
    const auto notebook = Json::parse(Read(workspace, "vx_notebook/config.json"));
    Require(notebook.value(kJsonKeyId, std::string()) == expected_id,
            "existing notebook differs from the caller-owned identity");
    Context context;
    Client client{workspace, config, provider, nullptr};
    client.Attach();
    client.Sync();
    std::cout << Json{{"backend", backend_name},
                      {"existingRootSynced", true},
                      {"noteDigests", NoteDigests(workspace)}}
                     .dump(2)
              << '\n';
    return 0;
  }
  Require(fs::is_directory(workspace) && fs::is_empty(workspace),
          "live-smoke workspace must already exist and be empty");

  Context context;
  const auto root_a = workspace / "client-a";
  const auto root_b = workspace / "client-b";
  char *created = nullptr;
  Check(vxcore_notebook_create(context.value, PathToUtf8(root_a).c_str(),
                               "{\"name\":\"Disposable live sync\",\"assetsFolder\":\"assets\","
                               "\"recycleBinFolder\":\"recycle\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &created),
        "create notebook");
  context.id = created;
  vxcore_string_free(created);
  fs::create_directories(root_b);

  Client a{root_a, config, provider, nullptr};
  Client b{root_b, config, provider, nullptr};

  Write(root_a, "note.md", "initial live revision\n");
  a.Attach();
  a.Sync();
  std::cout << "live: initial publication complete\n";
  b.Clone();
  Require(Read(root_b, "note.md") == "initial live revision\n", "clone bytes differ");
  std::cout << "live: second-device clone complete\n";

  Write(root_a, "note.md", "client A update\n");
  a.Sync();
  b.Sync();
  Require(Read(root_b, "note.md") == "client A update\n", "remote update not applied");
  Write(root_b, "second.md", "client B addition\n");
  b.Sync();
  a.Sync();
  Require(Read(root_a, "second.md") == "client B addition\n", "addition not synchronized");
  Require(fs::remove(root_a / "second.md"), "owned deletion failed");
  a.Sync();
  b.Sync();
  Require(!fs::exists(root_b / "second.md"), "deletion not synchronized");
  std::cout << "live: bidirectional edits and deletion complete\n";

  Write(root_a, "note.md", "concurrent A revision\n");
  Write(root_b, "note.md", "concurrent B revision\n");
  a.Sync();
  const auto conflict_result = b.backend->Sync(nullptr, nullptr);
  Require(conflict_result == VXCORE_ERR_SYNC_CONFLICT, "concurrent edit was not conflicted");
  std::vector<SyncConflictInfo> conflicts;
  Check(b.backend->GetConflicts(conflicts), "query conflicts", b.backend.get());
  const auto note_conflict =
      std::find_if(conflicts.begin(), conflicts.end(),
                   [](const SyncConflictInfo &item) { return item.path == "note.md"; });
  Require(note_conflict != conflicts.end() && note_conflict->can_keep_both,
          "ordinary note conflict cannot preserve both versions");
  Check(b.backend->ResolveConflict("note.md", SyncConflictResolution::kKeepBoth),
        "resolve keep both", b.backend.get());
  b.Sync();
  a.Sync();
  const auto before_restart = NoteDigests(root_a);
  Require(before_restart == NoteDigests(root_b), "clients differ after conflict resolution");
  bool found_a = false, found_b = false;
  for (const auto &digest : before_restart.items()) {
    found_a = found_a || digest.value() == Hash("concurrent A revision\n");
    found_b = found_b || digest.value() == Hash("concurrent B revision\n");
  }
  Require(found_a && found_b, "conflict resolution lost a revision");
  std::cout << "live: both conflicting revisions retained\n";

  a.backend.reset();
  b.backend.reset();
  a.Attach();
  b.Attach();
  a.Sync();
  b.Sync();
  Require(NoteDigests(root_a) == before_restart && NoteDigests(root_b) == before_restart,
          "restart changed the synchronized revisions");
  Json evidence{{"backend", backend_name}, {"noteDigests", before_restart},
                {"clone", true},           {"bidirectionalEdits", true},
                {"deletion", true},        {"conflictKeepBoth", true},
                {"restart", true}};
  std::cout << evidence.dump(2) << '\n';
  return 0;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  if (sodium_init() < 0) return 1;
  try {
    return Run();
  } catch (const std::exception &error) {
    std::cerr << "Live smoke: " << error.what() << '\n';
    return 1;
  }
}
