// Hermetic ordinary-folder sync tests. Run only through run_webdav_test.py.
// Each subprocess borrows the fixture through --reuse-fixture and receives its own
// context config/temp directory. Recovery children alone reuse their crashed root.
#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/config_manager.h"
#include "core/notebook.h"
#include "sync/credential_provider.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_json_keys.h"
#include "sync/webdav/webdav_sync_backend.h"
#include "sync/webdav/webdav_transport.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace {
using namespace vxcore;
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr const char *kConfigPath = "vx_notebook/config.json";
constexpr const char *kPrivatePath = "vx_notebook/vx_sync/webdav";
constexpr const char *kStatePath = "vx_notebook/vx_sync/webdav/state.json";
constexpr const char *kPendingPath = "vx_notebook/vx_sync/webdav/pending.json";
constexpr const char *kCollisionMessage =
    "A file and folder use the same path. Rename one of them, then sync again.";
std::string executable;

std::string Environment(const char *name) {
  const auto *value = std::getenv(name);
  return value ? value : "";
}

std::string RandomName() {
  std::array<unsigned char, 16> bytes{};
  std::array<char, 33> hex{};
  randombytes_buf(bytes.data(), bytes.size());
  sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
  return hex.data();
}

std::string Hash(const std::string &bytes) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char *>(bytes.data()),
                     bytes.size());
  sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
  return hex.data();
}

std::string Encode(const std::string &bytes) {
  std::string out(sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char *>(bytes.data()),
                    bytes.size(), sodium_base64_VARIANT_ORIGINAL);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::string Decode(const std::string &text) {
  std::string out(text.size(), '\0');
  size_t length = 0;
  if (sodium_base642bin(reinterpret_cast<unsigned char *>(out.data()), out.size(), text.data(),
                        text.size(), nullptr, &length, nullptr, sodium_base64_VARIANT_ORIGINAL))
    throw std::runtime_error("Fixture returned invalid base64");
  out.resize(length);
  return out;
}

std::string ReadBytes(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Could not read test-owned file: " + PathToUtf8(path));
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteBytes(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  if (WriteFileAtomic(path, bytes) != VXCORE_OK)
    throw std::runtime_error("Could not publish test-owned binary file");
}

Json ReadJson(const fs::path &path) {
  const auto value = Json::parse(ReadBytes(path), nullptr, false);
  if (value.is_discarded()) throw std::runtime_error("Expected valid persisted JSON");
  return value;
}

size_t ControlWrite(char *bytes, size_t size, size_t count, void *userdata) {
  auto &out = *static_cast<std::string *>(userdata);
  const size_t length = size * count;
  if (length > 4 * 1024 * 1024 - out.size()) return 0;
  out.append(bytes, length);
  return length;
}

Json Control(const Json &input) {
  const auto url = Environment("VXCORE_WEBDAV_TEST_CONTROL_URL");
  const auto token = Environment("VXCORE_WEBDAV_TEST_CONTROL_TOKEN");
  const auto ca = Environment("VXCORE_WEBDAV_TEST_CA_FILE");
  const auto body = input.dump();
  CURL *handle = curl_easy_init();
  if (!handle) throw std::runtime_error("Could not create fixture control session");
  curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
  headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
  std::string response;
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
  if (!ca.empty()) curl_easy_setopt(handle, CURLOPT_CAINFO, ca.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, ControlWrite);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  const auto code = curl_easy_perform(handle);
  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  const auto result = Json::parse(response, nullptr, false);
  if (code != CURLE_OK || status != 200 || !result.is_object() || !result.value("ok", false)) {
    std::cerr << "Fixture action " << input.value("action", "unknown") << ", curl "
              << static_cast<int>(code) << ", HTTP " << status << "\n";
    throw std::runtime_error("Fixture control failed (details redacted)");
  }
  return result;
}

void PutRemote(const std::string &path, const std::string &bytes) {
  Control(
      {{"action", "put"}, {"path", path}, {"content_base64", Encode(bytes)}, {"parents", true}});
}

Json Inspect(const std::string &path) {
  return Control({{"action", "inspect"}, {"path", path}, {"content", true}});
}

std::optional<std::string> RemoteBytes(const std::string &path) {
  const auto item = Inspect(path);
  if (!item.value("exists", false)) return std::nullopt;
  if (item.at("kind") != "file") throw std::runtime_error("Expected ordinary remote file");
  return Decode(item.at("content_base64").get<std::string>());
}

Json RemoteTree() {
  Json result = Json::object();
  for (size_t offset = 0;; offset += 1000) {
    const auto page = Control({{"action", "tree"}, {"offset", offset}, {"limit", 1000}});
    for (const auto &entry : page.at("entries"))
      result[entry.at("path").get<std::string>()] = entry;
    if (offset + 1000 >= page.at("total").get<size_t>()) return result;
  }
}

uint64_t RequestCursor() {
  return Control({{"action", "requests"}, {"limit", 1}}).at("total").get<uint64_t>();
}

bool NoMutationsSince(uint64_t cursor) {
  const auto requests = Control({{"action", "requests"}, {"since", cursor}, {"limit", 10000}});
  for (const auto &request : requests.at("requests")) {
    const auto method = request.at("method").get<std::string>();
    if (method == "PUT" || method == "MOVE" || method == "DELETE" || method == "MKCOL")
      return false;
  }
  return true;
}

bool WaitBarrier(const std::string &id) {
  return Control({{"action", "wait"}, {"id", id}, {"timeout_ms", 10000}}).value("reached", false);
}

struct FaultScope {
  ~FaultScope() {
    try {
      Control({{"action", "clear_faults"}});
    } catch (...) {
    }
  }
};

SyncConfig Configuration() {
  SyncConfig config;
  config.backend = "webdav";
  config.remote_url = Environment("VXCORE_WEBDAV_TEST_URL");
  config.auto_sync_enabled = false;
  return config;
}

SyncCredentials Credentials(
    const std::string &password = Environment("VXCORE_WEBDAV_TEST_PASSWORD")) {
  SyncCredentials credentials;
  credentials.extra = {{kJsonKeyUsername, Environment("VXCORE_WEBDAV_TEST_USERNAME")},
                       {kJsonKeyPassword, password}};
  return credentials;
}

std::shared_ptr<ICredentialProvider> Provider(
    const std::string &password = Environment("VXCORE_WEBDAV_TEST_PASSWORD")) {
  return std::make_shared<InMemoryCredentialProvider>(Credentials(password));
}

std::string CredentialsJson() { return Json{{kJsonKeyExtra, Credentials().extra}}.dump(); }

struct Workspace {
  fs::path path;
  Workspace() : path(PathFromUtf8(Environment("VXCORE_WEBDAV_TEST_CLIENT_ROOT")) / RandomName()) {
    fs::create_directories(path);
  }
  ~Workspace() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

struct Context {
  VxCoreContextHandle value = nullptr;
  Context() {
    if (vxcore_context_create(nullptr, &value) != VXCORE_OK)
      throw std::runtime_error("Could not create isolated core context");
  }
  ~Context() {
    if (value) vxcore_context_destroy(value);
  }
};

struct CoreString {
  char *value = nullptr;
  ~CoreString() {
    if (value) vxcore_string_free(value);
  }
  CoreString() = default;
  CoreString(const CoreString &) = delete;
  CoreString &operator=(const CoreString &) = delete;
};

struct Client {
  Workspace workspace;
  fs::path root = workspace.path / "notebook";
  Context context;
  std::string id;
  SyncConfig config = Configuration();
  std::unique_ptr<WebDavSyncBackend> backend;

  Client() {
    CoreString created;
    const auto root_text = PathToUtf8(root);
    if (vxcore_notebook_create(context.value, root_text.c_str(),
                               "{\"name\":\"DAV ordinary notebook\",\"assetsFolder\":\"assets\","
                               "\"recycleBinFolder\":\"recycle\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &created.value) != VXCORE_OK)
      throw std::runtime_error("Could not create bundled fixture notebook");
    id = created.value;
  }
  ~Client() {
    backend.reset();
    // Retire the session record before Workspace removes its notebook files.
    vxcore_notebook_close(context.value, id.c_str());
  }
  fs::path Path(const std::string &path) const { return root / PathFromUtf8(path); }
  void Write(const std::string &path, const std::string &bytes) { WriteBytes(Path(path), bytes); }
  std::string Read(const std::string &path) const { return ReadBytes(Path(path)); }
  Json State() const { return ReadJson(Path(kStatePath)); }
  VxCoreError Attach() {
    backend = std::make_unique<WebDavSyncBackend>(config, Provider());
    return backend->Initialize(PathToUtf8(root), config);
  }
  VxCoreError Sync() { return backend->Sync(nullptr, nullptr); }
};

std::set<std::string> ConflictPaths(WebDavSyncBackend &backend) {
  std::vector<SyncConflictInfo> conflicts;
  if (backend.GetConflicts(conflicts) != VXCORE_OK)
    throw std::runtime_error("Could not query durable conflicts");
  std::set<std::string> paths;
  for (const auto &conflict : conflicts) paths.insert(conflict.path);
  return paths;
}

bool CommonFile(const Client &client, const std::string &path) {
  const auto remote = Inspect(path);
  const auto state = client.State();
  return remote.value("exists", false) && remote.at("kind") == "file" &&
         state.at("entries").contains(path) &&
         state.at("entries").at(path).at("sha256") == remote.at("sha256") &&
         state.at("entries").at(path).at("etag") == remote.at("etag") &&
         Hash(client.Read(path)) == remote.at("sha256");
}

bool SnapshotContains(const Client &client, const std::string &bytes) {
  const auto snapshots = client.Path(std::string(kPrivatePath) + "/snapshots");
  if (!fs::exists(snapshots)) return false;
  for (const auto &entry : fs::recursive_directory_iterator(snapshots)) {
    if (entry.is_regular_file() && ReadBytes(entry.path()) == bytes) return true;
  }
  return false;
}

bool PendingHasOperations(const fs::path &root) {
  const auto path = root / PathFromUtf8(kPendingPath);
  return fs::is_regular_file(path) && !ReadJson(path).at("operations").empty();
}

bool SafeJournal(const Client &client) {
  const auto pending = ReadJson(client.Path(kPendingPath));
  if (pending.at("version") != 1 || pending.at("notebookId") != client.id ||
      pending.at("remoteUrl") != client.config.remote_url ||
      pending.at("usernameHash") != Hash(Environment("VXCORE_WEBDAV_TEST_USERNAME")))
    return false;
  const auto operation_id = pending.at("operationId").get<std::string>();
  if (WebDavTransport::ValidateRelativePath(operation_id) != VXCORE_OK ||
      operation_id.find('/') != std::string::npos)
    return false;
  for (const auto &operation : pending.at("operations")) {
    if (WebDavTransport::ValidateRelativePath(operation.at("path")) != VXCORE_OK) return false;
    for (const auto *field : {"sourceSnapshot", "previousSnapshot"}) {
      const auto &snapshot = operation.at(field);
      if (snapshot.is_null()) continue;
      const auto name = snapshot.get<std::string>();
      const auto prefix = "snapshots/" + operation_id + "/";
      if (name.compare(0, prefix.size(), prefix) ||
          WebDavTransport::ValidateRelativePath(name) != VXCORE_OK ||
          !fs::is_regular_file(client.Path(std::string(kPrivatePath) + "/" + name)))
        return false;
    }
    for (const auto *field : {"expectedLocalSha256", "oldSha256", "newSha256"}) {
      if (!operation.at(field).is_null() &&
          !std::regex_match(operation.at(field).get<std::string>(), std::regex("[0-9a-f]{64}")))
        return false;
    }
  }
  const auto bytes = ReadBytes(client.Path(kPendingPath));
  return bytes.find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) == std::string::npos &&
         bytes.find(PathToUtf8(client.root)) == std::string::npos;
}

struct NetworkCall {
  WebDavSyncBackend &backend;
  SyncCancellationPtr token = std::make_shared<SyncCancellation>();
  std::future<VxCoreError> result;
  explicit NetworkCall(WebDavSyncBackend &value) : backend(value) {
    backend.SetCancellation(token);
    result = std::async(std::launch::async, [&] { return backend.FetchRebasePush(); });
  }
  ~NetworkCall() {
    token->Cancel();
    try {
      Control({{"action", "clear_faults"}});
    } catch (...) {
    }
    if (result.valid()) result.wait();
    backend.SetCancellation(nullptr);
  }
};

int TestBootstrapOwnershipAndProjection() {
  Control({{"action", "reset"}});
  Client client;
  auto config = ReadJson(client.Path(kConfigPath));
  config[kJsonKeySyncEnabled] = true;
  config[kJsonKeySyncBackend] = "webdav";
  config[kJsonKeySyncRemoteUrl] = client.config.remote_url;
  config[kJsonKeyAutoSyncEnabled] = false;
  config["futureUnknownField"] = {{"preserve", "value"}};
  client.Write(kConfigPath, config.dump(2));
  client.Write("note.md", "ordinary\r\nbytes\n");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_TRUE(client.backend->IsInitialized());
  ASSERT_EQ(client.backend->GetName(), "webdav");
  ASSERT_EQ(client.backend->GetCapabilities() & uint32_t(255), uint32_t(255));
  ASSERT_FALSE(Inspect(kConfigPath).value("exists", false));
  ASSERT_FALSE(Inspect("note.md").value("exists", false));
  std::vector<SyncFileInfo> status;
  const auto cursor = RequestCursor();
  ASSERT_EQ(client.backend->GetStatus(status), VXCORE_OK);
  for (const auto &file : status) {
    ASSERT_TRUE(file.status == SyncFileStatus::kAddedLocal ||
                file.status == SyncFileStatus::kModifiedLocal ||
                file.status == SyncFileStatus::kDeletedLocal);
  }
  ASSERT_TRUE(ConflictPaths(*client.backend).empty());
  ASSERT_EQ(RequestCursor(), cursor);
  bool committed = true;
  ASSERT_EQ(client.backend->StageAndCommit(&committed), VXCORE_OK);
  ASSERT_FALSE(committed);
  ASSERT_EQ(RequestCursor(), cursor);
  ASSERT_EQ(client.backend->FetchRebasePush(), VXCORE_OK);
  std::vector<std::string> changed;
  const auto before_apply = RequestCursor();
  ASSERT_EQ(client.backend->ApplySync({}, changed), VXCORE_OK);
  ASSERT_EQ(RequestCursor(), before_apply);
  ASSERT_TRUE(CommonFile(client, "note.md"));
  auto remote = Json::parse(*RemoteBytes(kConfigPath));
  for (const auto *key :
       {kJsonKeySyncEnabled, kJsonKeySyncBackend, kJsonKeySyncRemoteUrl, kJsonKeyAutoSyncEnabled})
    ASSERT_FALSE(remote.contains(key));
  ASSERT_EQ(remote.at("id"), client.id);
  ASSERT_TRUE(remote.at("futureUnknownField") == config.at("futureUnknownField"));
  const auto state = client.State();
  ASSERT_EQ(state.at("entries").at(kConfigPath).at("sha256"), Hash(remote.dump()));
  ASSERT_EQ(ReadJson(client.Path(kConfigPath)), config);
  const auto stable = RequestCursor();
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(NoMutationsSince(stable));

  Client stranger;
  ASSERT_NE(stranger.Attach(), VXCORE_OK);
  ASSERT_FALSE(stranger.backend->IsInitialized());
  ASSERT_EQ(*RemoteBytes("note.md"), "ordinary\r\nbytes\n");
  Control({{"action", "reset"}});
  PutRemote("unrelated.txt", "belongs to another application");
  const auto unrelated = RemoteTree();
  Client nonempty;
  ASSERT_NE(nonempty.Attach(), VXCORE_OK);
  ASSERT_FALSE(nonempty.backend->IsInitialized());
  ASSERT_TRUE(RemoteTree() == unrelated);
  return 0;
}

int TestBootstrapRaceAndConditionalProbes() {
  const std::pair<const char *, const char *> conditions[] = {
      {"ignore_create_conditions", "If-None-Match"},
      {"ignore_move_conditions", "MOVE"},
      {"ignore_delete_conditions", "DELETE"},
  };
  for (const auto &condition : conditions) {
    Control({{"action", "reset"}});
    Control({{"action", "configure"}, {condition.first, true}});
    Client client;
    client.Write("must-not-publish.md", "local only");
    ASSERT_EQ(client.Attach(), VXCORE_ERR_UNSUPPORTED);
    ASSERT_FALSE(client.backend->IsInitialized());
    ASSERT_FALSE(Inspect(kConfigPath).value("exists", false));
    ASSERT_FALSE(Inspect("must-not-publish.md").value("exists", false));
    const auto error = client.backend->GetLastError();
    ASSERT_TRUE(error.find(condition.second) != std::string::npos);
    ASSERT_TRUE(error.find("HTTP 412") != std::string::npos);
    ASSERT_TRUE(error.find("HTTP 201") != std::string::npos ||
                error.find("HTTP 204") != std::string::npos);
    std::cout << "Capability diagnostic: " << error << std::endl;
    const auto probe_tree = RemoteTree();
    for (const auto &item : probe_tree.items()) {
      ASSERT_TRUE(item.key().empty() || item.key().find(".vnote-webdav-tmp-") == 0);
    }
  }
  Control({{"action", "reset"}});
  Client loser;
  loser.Write("loser.md", "must stay local");
  ASSERT_EQ(loser.Attach(), VXCORE_OK);
  Client winner;
  winner.Write("winner.md", "winner content");
  ASSERT_EQ(winner.Attach(), VXCORE_OK);
  ASSERT_EQ(winner.Sync(), VXCORE_OK);
  const auto before = RemoteTree();
  ASSERT_NE(loser.Sync(), VXCORE_OK);
  ASSERT_TRUE(RemoteTree() == before);
  ASSERT_EQ(loser.Read("loser.md"), "must stay local");
  return 0;
}

int TestSetupFailureDiagnostics() {
  struct Scenario {
    Json fault;
    const char *method;
    const char *status;
    bool file_listing;
    bool etag_error;
  };
  const Scenario scenarios[] = {
      {{{"method", "PROPFIND"}, {"effect", "status"}, {"status", 405}},
       "PROPFIND", "HTTP 405", false, false},
      {{{"method", "PUT"}, {"effect", "missing_etag"}}, "PUT", "HTTP 201", false, true},
      {{{"method", "PROPFIND"}, {"effect", "weak_etag"}}, "PROPFIND", "HTTP 207", true, true},
      {{{"method", "GET"}, {"effect", "missing_etag"}}, "GET", "HTTP 200", false, true},
  };
  for (const auto &scenario : scenarios) {
    Control({{"action", "reset"}});
    FaultScope cleanup;
    const std::string foreign_path = ".vnote-webdav-tmp-private-filename";
    const std::string foreign_body = "private-server-content";
    if (scenario.file_listing) PutRemote(foreign_path, foreign_body);
    auto fault = scenario.fault;
    fault["action"] = "fault";
    Control(fault); // Wildcard path also reaches randomly named probe files.
    Client client;
    client.Write("must-not-publish.md", "local only");
    ASSERT_EQ(client.Attach(), VXCORE_ERR_UNSUPPORTED);
    const auto error = client.backend->GetLastError();
    ASSERT_TRUE(error.find(scenario.method) != std::string::npos);
    ASSERT_TRUE(error.find(scenario.status) != std::string::npos);
    ASSERT_EQ(error.find("ETag") != std::string::npos, scenario.etag_error);
    ASSERT_TRUE(error.find(client.config.remote_url) == std::string::npos);
    ASSERT_TRUE(error.find(Environment("VXCORE_WEBDAV_TEST_USERNAME")) == std::string::npos);
    ASSERT_TRUE(error.find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) == std::string::npos);
    ASSERT_TRUE(error.find(foreign_path) == std::string::npos);
    ASSERT_TRUE(error.find(foreign_body) == std::string::npos);
    ASSERT_FALSE(Inspect(kConfigPath).value("exists", false));
    ASSERT_FALSE(Inspect("must-not-publish.md").value("exists", false));
    std::cout << "Setup diagnostic: " << error << std::endl;

    Control({{"action", "clear_faults"}});
    ASSERT_EQ(client.backend->Initialize(PathToUtf8(client.root), client.config), VXCORE_OK);
    ASSERT_TRUE(client.backend->GetLastError().empty());
    if (scenario.file_listing) ASSERT_EQ(*RemoteBytes(foreign_path), foreign_body);
  }

  Control({{"action", "reset"}});
  Client denied;
  const std::string wrong_password = "private-incorrect-password";
  denied.backend = std::make_unique<WebDavSyncBackend>(denied.config, Provider(wrong_password));
  ASSERT_EQ(denied.backend->Initialize(PathToUtf8(denied.root), denied.config),
            VXCORE_ERR_SYNC_AUTH_FAILED);
  const auto error = denied.backend->GetLastError();
  ASSERT_TRUE(error.find("PROPFIND") != std::string::npos);
  ASSERT_TRUE(error.find("HTTP 401") != std::string::npos);
  ASSERT_TRUE(error.find(wrong_password) == std::string::npos);
  ASSERT_FALSE(Inspect(kConfigPath).value("exists", false));

  // A successful listing followed by an identity rejection is not an HTTP failure.
  Control({{"action", "reset"}});
  PutRemote("foreign.md", "preserve this existing collection");
  Client foreign;
  ASSERT_EQ(foreign.Attach(), VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(foreign.backend->GetLastError().find("HTTP") == std::string::npos);
  ASSERT_EQ(*RemoteBytes("foreign.md"), "preserve this existing collection");
  return 0;
}

int TestExcludedAndUnsupportedLocalNames() {
  Control({{"action", "reset"}});
  Client client;
  client.config.exclude_paths.push_back("ignored/");
  client.Write("included.md", "included");
  client.Write("ignored/private.txt", "excluded subtree");
  client.Write(".git/HEAD", "not a git sync repository");
  client.Write("scratch.vswp", "unsaved editor state");
  client.Write("vx_notebook/vx_transfer/payload", "transfer-private");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto uploaded_tree = RemoteTree();
  for (const auto &item : uploaded_tree.items()) {
    ASSERT_TRUE(item.key().find("ignored") != 0);
    ASSERT_TRUE(item.key().find(".git") != 0);
    ASSERT_TRUE(item.key().find("vx_notebook/vx_sync") != 0);
    ASSERT_TRUE(item.key().find("vx_notebook/vx_transfer") != 0);
    ASSERT_TRUE(item.key().find(".vswp") == std::string::npos);
  }
  PutRemote("ignored/remote.txt", "remote excluded content");
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_FALSE(fs::exists(client.Path("ignored/remote.txt")));
  const auto before = RemoteTree();
  client.Write(".vnote-webdav-tmp-user-note", "do not silently ignore me");
  ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_ERR_UNSUPPORTED);
  ASSERT_FALSE(client.backend->GetLastError().empty());
  ASSERT_TRUE(RemoteTree() == before);
  fs::remove(client.Path(".vnote-webdav-tmp-user-note"));
  PutRemote(".vnote-webdav-tmp-other-client", "owned by another client");
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_EQ(*RemoteBytes(".vnote-webdav-tmp-other-client"), "owned by another client");
  ASSERT_FALSE(fs::exists(client.Path(".vnote-webdav-tmp-other-client")));

  for (const auto *field : {"assetsFolder", "recycleBinFolder"}) {
    Client escaping;
    auto config = ReadJson(escaping.Path(kConfigPath));
    config[field] = "../external";
    escaping.Write(kConfigPath, config.dump());
    ASSERT_NE(escaping.Attach(), VXCORE_OK);
    ASSERT_FALSE(escaping.backend->IsInitialized());
  }
  return 0;
}

int TestCompleteScanRequiredForDeletions() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("kept.md", "keep local bytes");
  client.Write("nested/child.md", "keep child bytes");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto baseline = client.Read(kStatePath);
  const std::vector<Json> faults = {
      {{"method", "PROPFIND"}, {"path", ""}, {"effect", "status"}, {"status", 404}},
      {{"method", "PROPFIND"}, {"path", "nested"}, {"effect", "status"}, {"status", 500}},
      {{"method", "PROPFIND"}, {"path", ""}, {"effect", "xml"}, {"body", "<broken"}},
      {{"method", "PROPFIND"},
       {"path", ""},
       {"effect", "xml"},
       {"body",
        "<multistatus xmlns='DAV:'><response><href>/notebook/</href><propstat>"
        "<prop><resourcetype><collection/></resourcetype></prop>"
        "<status>HTTP/1.1 200 OK</status></propstat></response>"
        "<response><href>/notebook/kept.md</href><status>HTTP/1.1 403 Forbidden</status>"
        "</response></multistatus>"}},
      {{"method", "PROPFIND"}, {"path", ""}, {"effect", "weak_etag"}},
      {{"method", "PROPFIND"}, {"path", ""}, {"effect", "missing_etag"}}};
  for (auto fault : faults) {
    FaultScope cleanup;
    fault["action"] = "fault";
    fault["count"] = -1;
    Control(fault);
    ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
    const auto cursor = RequestCursor();
    ASSERT_NE(client.backend->FetchRebasePush(), VXCORE_OK);
    ASSERT_TRUE(NoMutationsSince(cursor));
    ASSERT_EQ(client.Read("kept.md"), "keep local bytes");
    ASSERT_EQ(client.Read("nested/child.md"), "keep child bytes");
    ASSERT_EQ(client.Read(kStatePath), baseline);
  }
  Control({{"action", "delete"}, {"path", "kept.md"}});
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_FALSE(fs::exists(client.Path("kept.md")));
  ASSERT_FALSE(client.State().at("entries").contains("kept.md"));
  return 0;
}

int TestConflictMatrixAndDurableChoices() {
  const std::vector<SyncConflictResolution> choices = {SyncConflictResolution::kKeepLocal,
                                                       SyncConflictResolution::kKeepRemote,
                                                       SyncConflictResolution::kKeepBoth};
  // Distinct transitions: edit/edit, local edit/remote delete, local delete/remote
  // edit, and independent additions with no baseline. Every choice is observable.
  for (int transition = 0; transition != 4; ++transition) {
    for (const auto choice : choices) {
      Control({{"action", "reset"}});
      Client client;
      if (transition != 3) client.Write("conflict.bin", std::string("base\0\r\n", 7));
      client.Write("unrelated.md", "unchanged");
      ASSERT_EQ(client.Attach(), VXCORE_OK);
      ASSERT_EQ(client.Sync(), VXCORE_OK);
      const std::string local("local\0\r\n", 8), remote("remote\0\n", 8);
      if (transition == 2)
        fs::remove(client.Path("conflict.bin"));
      else
        client.Write("conflict.bin", local);
      if (transition == 1)
        Control({{"action", "delete"}, {"path", "conflict.bin"}});
      else
        PutRemote("conflict.bin", remote);
      client.Write("unrelated.md", "must not publish while any conflict exists");
      const auto before = RemoteTree();
      const auto cursor = RequestCursor();
      ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_CONFLICT);
      ASSERT_TRUE(NoMutationsSince(cursor));
      ASSERT_TRUE(RemoteTree() == before);
      ASSERT_TRUE(ConflictPaths(*client.backend) == std::set<std::string>{"conflict.bin"});
      if (transition != 2) ASSERT_TRUE(SnapshotContains(client, local));
      if (transition != 1) ASSERT_TRUE(SnapshotContains(client, remote));
      const auto durable = client.State().at("conflicts").at("conflict.bin");
      ASSERT_TRUE(durable.at("localSha256").is_null() == (transition == 2));
      ASSERT_TRUE(durable.at("remoteSha256").is_null() == (transition == 1));
      ASSERT_FALSE(durable.contains("resolution"));
      ASSERT_TRUE(durable.at("canKeepBoth").get<bool>());
      ASSERT_TRUE(durable.at("isBinary").get<bool>());
      client.backend.reset();
      ASSERT_EQ(client.Attach(), VXCORE_OK);
      ASSERT_TRUE(ConflictPaths(*client.backend).count("conflict.bin"));
      ASSERT_NE(client.backend->ResolveConflict("unknown.md", choice), VXCORE_OK);
      const auto local_before = fs::exists(client.Path("conflict.bin"))
                                    ? std::optional<std::string>(client.Read("conflict.bin"))
                                    : std::nullopt;
      const auto resolve_cursor = RequestCursor();
      ASSERT_EQ(client.backend->ResolveConflict("conflict.bin", choice), VXCORE_OK);
      ASSERT_EQ(RequestCursor(), resolve_cursor);
      ASSERT_TRUE(RemoteTree() == before);
      ASSERT_TRUE(local_before == (fs::exists(client.Path("conflict.bin"))
                                       ? std::optional<std::string>(client.Read("conflict.bin"))
                                       : std::nullopt));
      // Restart after choosing: the choice, not an in-memory dialog, owns consent.
      client.backend.reset();
      ASSERT_EQ(client.Attach(), VXCORE_OK);
      ASSERT_EQ(client.Sync(), VXCORE_OK);
      ASSERT_TRUE(ConflictPaths(*client.backend).empty());
      const bool keep_local = choice == SyncConflictResolution::kKeepLocal;
      const bool keep_remote = choice == SyncConflictResolution::kKeepRemote;
      const bool absent = (keep_local && transition == 2) || (keep_remote && transition == 1);
      ASSERT_TRUE(fs::exists(client.Path("conflict.bin")) != absent);
      ASSERT_TRUE(RemoteBytes("conflict.bin").has_value() != absent);
      if (!absent) {
        const auto expected =
            keep_remote || (choice == SyncConflictResolution::kKeepBoth && transition == 2) ? remote
                                                                                            : local;
        ASSERT_EQ(client.Read("conflict.bin"), expected);
        ASSERT_EQ(*RemoteBytes("conflict.bin"), expected);
        ASSERT_TRUE(CommonFile(client, "conflict.bin"));
      }
      size_t copies = 0;
      for (const auto &entry : fs::directory_iterator(client.root)) {
        const auto name = PathToUtf8(entry.path().filename());
        if (name.find("conflict.sync-conflict-") != 0) continue;
        ++copies;
        ASSERT_TRUE(std::regex_match(
            name, std::regex("conflict\\.sync-conflict-[0-9]+-[0-9a-fA-F-]+\\.bin")));
        ASSERT_EQ(ReadBytes(entry.path()), remote);
        ASSERT_EQ(*RemoteBytes(name), remote);
        ASSERT_TRUE(CommonFile(client, name));
      }
      ASSERT_EQ(copies, size_t(choice == SyncConflictResolution::kKeepBoth && transition != 1 &&
                                       transition != 2
                                   ? 1
                                   : 0));
    }
  }
  return 0;
}

int TestStaleChoicesAndSameBytesEtag() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("note.md", "base");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  client.Write("note.md", "local choice");
  PutRemote("note.md", "remote choice");
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(client.backend->ResolveConflict("note.md", SyncConflictResolution::kKeepLocal),
            VXCORE_OK);
  PutRemote("note.md", "third remote edit after choice");
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(*RemoteBytes("note.md"), "third remote edit after choice");
  ASSERT_EQ(client.Read("note.md"), "local choice");
  ASSERT_FALSE(client.State().at("conflicts").at("note.md").contains("resolution"));
  ASSERT_EQ(client.backend->ResolveConflict("note.md", SyncConflictResolution::kKeepRemote),
            VXCORE_OK);
  client.Write("note.md", "fourth local edit after choice");
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(client.Read("note.md"), "fourth local edit after choice");
  ASSERT_EQ(client.backend->ResolveConflict("note.md", SyncConflictResolution::kKeepRemote),
            VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto previous = Inspect("note.md").at("etag");
  Control({{"action", "touch_etag"}, {"path", "note.md"}});
  ASSERT_TRUE(Inspect("note.md").at("etag") != previous);
  const auto cursor = RequestCursor();
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(ConflictPaths(*client.backend).empty());
  ASSERT_TRUE(NoMutationsSince(cursor));
  ASSERT_TRUE(CommonFile(client, "note.md"));
  return 0;
}

struct EncryptedRevision {
  std::string envelope;
  std::string ciphertext;
};

EncryptedRevision MakeEncryptedRevision(const std::string &notebook_id,
                                        const std::string &document_id,
                                        const std::string &plaintext) {
  Workspace staging;
  NotebookEncryption::KeyEnvelope envelope;
  NotebookEncryption::Key master, notebook, note;
  NotebookEncryption::ObjectHeader header;
  const std::string passphrase = "disposable authenticated envelope fixture";
  auto require_success = [](VxCoreError result) {
    if (result != VXCORE_OK)
      throw std::runtime_error("Could not create real encrypted test revision");
  };
  require_success(NotebookEncryption::PrepareNewKeys(
      notebook_id, passphrase.data(), passphrase.size(), envelope, master, notebook));
  require_success(NotebookEncryption::GenerateKey(note));
  const std::vector<uint8_t> body(plaintext.begin(), plaintext.end());
  const Json manifest{{"editorType", "markdown"}};
  const auto path = staging.path / "snapshot.vne";
  require_success(NotebookEncryption::WriteNoteSnapshot(path, envelope, notebook, note, document_id,
                                                        body, manifest, &header));
  std::vector<uint8_t> decoded_body;
  Json decoded_manifest;
  require_success(
      NotebookEncryption::ReadNoteSnapshot(path, header, note, decoded_body, decoded_manifest));
  if (decoded_body != body || decoded_manifest != manifest)
    throw std::runtime_error("Encrypted fixture did not authenticate its original content");
  NotebookEncryption::WipeBytes(decoded_body);
  NotebookEncryption::WipeJson(decoded_manifest);
  EncryptedRevision result;
  require_success(NotebookEncryption::EncodeKeyEnvelope(envelope, result.envelope));
  result.ciphertext = ReadBytes(path);
  return result;
}

int TestMetadataAndCiphertextConflictPolicy() {
  Control({{"action", "reset"}});
  Client client;
  std::string document_id;
  ASSERT_EQ(NotebookEncryption::GenerateIdentity(document_id), VXCORE_OK);
  const auto baseline = MakeEncryptedRevision(client.id, document_id, "protected baseline content");
  const auto local_revision =
      MakeEncryptedRevision(client.id, document_id, "protected local content");
  const auto remote_revision =
      MakeEncryptedRevision(client.id, document_id, "protected remote content");
  client.Write("secret.vne", baseline.ciphertext);
  client.Write("vx_notebook/encryption.vne", baseline.envelope);
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_EQ(*RemoteBytes("secret.vne"), baseline.ciphertext);
  ASSERT_EQ(*RemoteBytes("vx_notebook/encryption.vne"), baseline.envelope);
  const std::vector<std::string> paths = {kConfigPath, "vx_notebook/contents/vx.json", "secret.vne",
                                          "vx_notebook/encryption.vne"};
  for (const auto &path : paths) {
    const auto original = client.Read(path);
    if (path.find(".json") != std::string::npos) {
      auto local = Json::parse(original), remote = local;
      local["metadata"]["side"] = "local metadata";
      remote["metadata"]["side"] = "remote metadata";
      client.Write(path, local.dump());
      PutRemote(path, remote.dump());
    } else {
      const bool key_envelope = path == "vx_notebook/encryption.vne";
      client.Write(path, key_envelope ? local_revision.envelope : local_revision.ciphertext);
      PutRemote(path, key_envelope ? remote_revision.envelope : remote_revision.ciphertext);
    }
  }
  const auto before = RemoteTree();
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  std::vector<SyncConflictInfo> conflicts;
  ASSERT_EQ(client.backend->GetConflicts(conflicts), VXCORE_OK);
  ASSERT_EQ(conflicts.size(), paths.size());
  for (const auto &conflict : conflicts) {
    ASSERT_FALSE(conflict.can_keep_both);
    ASSERT_EQ(client.backend->ResolveConflict(conflict.path, SyncConflictResolution::kKeepBoth),
              VXCORE_ERR_UNSUPPORTED);
    ASSERT_FALSE(client.State().at("conflicts").at(conflict.path).contains("resolution"));
    // Both snapshots remain complete revisions; no JSON/ciphertext conflict markers.
    const auto record = client.State().at("conflicts").at(conflict.path);
    const auto local = ReadBytes(client.Path(std::string(kPrivatePath) + "/" +
                                             record.at("localSnapshot").get<std::string>()));
    const auto remote = ReadBytes(client.Path(std::string(kPrivatePath) + "/" +
                                              record.at("remoteSnapshot").get<std::string>()));
    ASSERT_TRUE(local.find("<<<<<<<") == std::string::npos);
    ASSERT_TRUE(remote.find("<<<<<<<") == std::string::npos);
    if (conflict.path.find(".json") != std::string::npos) {
      ASSERT_TRUE(Json::parse(local, nullptr, false).is_object());
      ASSERT_TRUE(Json::parse(remote, nullptr, false).is_object());
    } else {
      const bool key_envelope = conflict.path == "vx_notebook/encryption.vne";
      ASSERT_EQ(local, key_envelope ? local_revision.envelope : local_revision.ciphertext);
      ASSERT_EQ(remote, key_envelope ? remote_revision.envelope : remote_revision.ciphertext);
      if (key_envelope) {
        NotebookEncryption::KeyEnvelope local_key, remote_key;
        ASSERT_EQ(NotebookEncryption::DecodeKeyEnvelope(local.data(), local.size(), local_key),
                  VXCORE_OK);
        ASSERT_EQ(NotebookEncryption::DecodeKeyEnvelope(remote.data(), remote.size(), remote_key),
                  VXCORE_OK);
        ASSERT_EQ(local_key.notebook_id, client.id);
        ASSERT_EQ(remote_key.notebook_id, client.id);
      }
      ASSERT_TRUE(local.find("protected local content") == std::string::npos);
      ASSERT_TRUE(remote.find("protected remote content") == std::string::npos);
    }
  }
  ASSERT_TRUE(RemoteTree() == before);
  return 0;
}

int TestTypeCollisionsAndRetainedCollections() {
  for (bool local_file : {false, true}) {
    Control({{"action", "reset"}});
    Client client;
    client.Write("common.md", "baseline");
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    if (local_file) {
      client.Write("collision", "local file");
      PutRemote("collision/remote-child", "remote collection child");
    } else {
      client.Write("collision/local-child", "local collection child");
      PutRemote("collision", "remote file");
    }
    const auto baseline = client.Read(kStatePath);
    const auto remote = RemoteTree();
    const auto cursor = RequestCursor();
    ASSERT_EQ(client.Sync(), VXCORE_ERR_UNSUPPORTED);
    ASSERT_EQ(client.backend->GetLastError(), kCollisionMessage);
    ASSERT_TRUE(NoMutationsSince(cursor));
    ASSERT_TRUE(RemoteTree() == remote);
    ASSERT_EQ(client.Read(kStatePath), baseline);
    ASSERT_TRUE(ConflictPaths(*client.backend).empty());
    ASSERT_EQ(client.Read(local_file ? "collision" : "collision/local-child"),
              local_file ? "local file" : "local collection child");
    fs::rename(client.Path("collision"), client.Path("renamed-local"));
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    ASSERT_EQ(client.Read(local_file ? "collision/remote-child" : "collision"),
              local_file ? "remote collection child" : "remote file");
    ASSERT_EQ(*RemoteBytes(local_file ? "renamed-local" : "renamed-local/local-child"),
              local_file ? "local file" : "local collection child");
  }
  Control({{"action", "reset"}});
  Client client;
  client.Write("removed-folder/old.md", "old child");
  fs::create_directories(client.Path("empty/nested"));
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_EQ(Inspect("empty/nested").at("kind"), "collection");
  fs::remove(client.Path("removed-folder/old.md"));
  fs::remove(client.Path("removed-folder"));
  PutRemote("removed-folder/concurrent.md", "new external child survives");
  const auto cursor = RequestCursor();
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_FALSE(RemoteBytes("removed-folder/old.md").has_value());
  ASSERT_EQ(*RemoteBytes("removed-folder/concurrent.md"), "new external child survives");
  ASSERT_EQ(client.Read("removed-folder/concurrent.md"), "new external child survives");
  const auto requests = Control({{"action", "requests"}, {"since", cursor}, {"limit", 10000}});
  for (const auto &request : requests.at("requests")) {
    ASSERT_FALSE(request.at("method") == "DELETE" && request.at("path") == "removed-folder");
    ASSERT_FALSE(request.at("method") == "DELETE" && request.at("path") == "empty");
  }
  fs::remove(client.Path("empty/nested"));
  fs::remove(client.Path("empty"));
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto converged = RequestCursor();
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(NoMutationsSince(converged));
  ASSERT_EQ(Inspect("empty/nested").at("kind"), "collection");
  return 0;
}

int TestIncomingConfigRestoresRouting() {
  Control({{"action", "reset"}});
  Client client;
  auto local = ReadJson(client.Path(kConfigPath));
  local[kJsonKeySyncEnabled] = true;
  local[kJsonKeySyncBackend] = "webdav";
  local[kJsonKeySyncRemoteUrl] = client.config.remote_url;
  local[kJsonKeyAutoSyncEnabled] = false;
  client.Write(kConfigPath, local.dump(2));
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  auto remote = Json::parse(*RemoteBytes(kConfigPath));
  remote["description"] = "external notebook metadata";
  remote["unknownPortableKey"] = Json::array({"not removed", 17});
  remote[kJsonKeySyncEnabled] = false;
  remote[kJsonKeySyncBackend] = "git";
  remote[kJsonKeySyncRemoteUrl] = "https://other.invalid/untrusted";
  remote[kJsonKeyAutoSyncEnabled] = true;
  PutRemote(kConfigPath, remote.dump(2));
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto installed = ReadJson(client.Path(kConfigPath));
  ASSERT_EQ(installed.at("description"), "external notebook metadata");
  ASSERT_TRUE(installed.at("unknownPortableKey") == remote.at("unknownPortableKey"));
  for (const auto *key :
       {kJsonKeySyncEnabled, kJsonKeySyncBackend, kJsonKeySyncRemoteUrl, kJsonKeyAutoSyncEnabled})
    ASSERT_TRUE(installed.at(key) == local.at(key));
  const auto cursor = RequestCursor();
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(NoMutationsSince(cursor));
  ASSERT_TRUE(ConflictPaths(*client.backend).empty());
  return 0;
}

int TestDeferredApplyProtectsWholeCohort() {
  for (bool protected_path : {false, true}) {
    Control({{"action", "reset"}});
    Client client;
    client.Write("a-first.md", "first baseline");
    client.Write("z-protected.md", "protected baseline");
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    PutRemote("a-first.md", "incoming first");
    PutRemote("z-protected.md", "incoming protected");
    ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
    ASSERT_EQ(client.backend->FetchRebasePush(), VXCORE_OK);
    ASSERT_EQ(client.Read("a-first.md"), "first baseline");
    ASSERT_EQ(client.Read("z-protected.md"), "protected baseline");
    ASSERT_TRUE(PendingHasOperations(client.root));
    ASSERT_TRUE(SafeJournal(client));
    if (!protected_path) client.Write("z-protected.md", "editor save after preparation");
    const auto baseline_entries = client.State().at("entries");
    const auto cursor = RequestCursor();
    std::vector<std::string> changed{"must be cleared"};
    ASSERT_EQ(client.backend->ApplySync(protected_path ? std::vector<std::string>{"z-protected.md"}
                                                       : std::vector<std::string>{},
                                        changed),
              VXCORE_ERR_SYNC_CONFLICT);
    ASSERT_TRUE(changed.empty());
    ASSERT_EQ(RequestCursor(), cursor);
    ASSERT_EQ(client.Read("a-first.md"), "first baseline");
    ASSERT_EQ(client.Read("z-protected.md"),
              protected_path ? "protected baseline" : "editor save after preparation");
    ASSERT_TRUE(client.State().at("entries") == baseline_entries);
    ASSERT_TRUE(ConflictPaths(*client.backend).count("z-protected.md"));
    ASSERT_TRUE(SnapshotContains(client, "incoming first"));
    ASSERT_TRUE(SnapshotContains(client, "incoming protected"));
    ASSERT_EQ(client.backend->ResolveConflict("z-protected.md", SyncConflictResolution::kKeepLocal),
              VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    ASSERT_EQ(client.Read("a-first.md"), "incoming first");
    ASSERT_EQ(*RemoteBytes("z-protected.md"),
              protected_path ? "protected baseline" : "editor save after preparation");
  }
  return 0;
}

int TestConditionalRacesPreserveThirdPartyVersion() {
  for (bool deletion : {false, true}) {
    Control({{"action", "reset"}});
    Client client;
    client.Write("race.md", "common old version");
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    if (deletion)
      fs::remove(client.Path("race.md"));
    else
      client.Write("race.md", "prepared local version");
    ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
    FaultScope cleanup;
    Control({{"action", "barrier"},
             {"id", "conditional-race"},
             {"method", deletion ? "DELETE" : "MOVE"},
             {"path", deletion ? "race.md" : "*"},
             {"phase", "before"}});
    NetworkCall work(*client.backend);
    ASSERT_TRUE(WaitBarrier("conditional-race"));
    ASSERT_TRUE(PendingHasOperations(client.root));
    ASSERT_TRUE(SnapshotContains(client, "common old version"));
    PutRemote("race.md", "new third-party version");
    Control({{"action", "release"}, {"id", "conditional-race"}});
    ASSERT_TRUE(work.result.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    ASSERT_EQ(work.result.get(), VXCORE_ERR_SYNC_CONFLICT);
    ASSERT_EQ(*RemoteBytes("race.md"), "new third-party version");
    ASSERT_TRUE(ConflictPaths(*client.backend).count("race.md"));
    if (!deletion)
      ASSERT_EQ(client.Read("race.md"), "prepared local version");
    else
      ASSERT_FALSE(fs::exists(client.Path("race.md")));
  }
  return 0;
}

int TestCancellationAndConcurrentSnapshots() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("note.md", "common");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  client.Write("note.md", "new immutable upload");
  ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
  FaultScope cleanup;
  Control({{"action", "barrier"}, {"id", "cancel-put"}, {"method", "PUT"}, {"phase", "before"}});
  {
    NetworkCall work(*client.backend);
    ASSERT_TRUE(WaitBarrier("cancel-put"));
    ASSERT_TRUE(PendingHasOperations(client.root));
    ASSERT_TRUE(SafeJournal(client));
    const auto cursor = RequestCursor();
    auto queries = std::async(std::launch::async, [&] {
      std::vector<SyncFileInfo> status;
      std::vector<SyncConflictInfo> conflicts;
      return client.backend->GetStatus(status) == VXCORE_OK &&
             client.backend->GetConflicts(conflicts) == VXCORE_OK &&
             client.backend->GetLastError().find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) ==
                 std::string::npos;
    });
    const bool ready = queries.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool network_free = RequestCursor() == cursor;
    // Always unblock the worker before asserting, including the failure path.
    work.token->Cancel();
    const bool cancelled =
        work.result.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    Control({{"action", "release"}, {"id", "cancel-put"}});
    ASSERT_TRUE(ready);
    ASSERT_TRUE(queries.get());
    ASSERT_TRUE(cancelled);
    ASSERT_EQ(work.result.get(), VXCORE_ERR_CANCELLED);
    ASSERT_TRUE(network_free);
  }
  ASSERT_TRUE(PendingHasOperations(client.root));
  ASSERT_EQ(*RemoteBytes("note.md"), "common");
  client.backend.reset();
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(CommonFile(client, "note.md"));

  PutRemote("note.md", "download to defer");
  ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
  ASSERT_EQ(client.backend->FetchRebasePush(), VXCORE_OK);
  auto token = std::make_shared<SyncCancellation>();
  token->Cancel();
  client.backend->SetCancellation(token);
  std::vector<std::string> changed{"stale output"};
  ASSERT_EQ(client.backend->ApplySync({}, changed), VXCORE_ERR_CANCELLED);
  ASSERT_TRUE(changed.empty());
  ASSERT_EQ(client.Read("note.md"), "new immutable upload");
  ASSERT_TRUE(PendingHasOperations(client.root));
  client.backend->SetCancellation(nullptr);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_EQ(client.Read("note.md"), "download to defer");
  return 0;
}

int TestProviderRotationAndCompositeProgress() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("payload.bin", std::string(192 * 1024, 'p'));
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  std::vector<SyncProgress> progress;
  bool callback_query_ok = true;
  int cookie = 31;
  const auto sync_result = client.backend->Sync(
      [&](const SyncProgress &event, void *userdata) {
        if (userdata != &cookie) callback_query_ok = false;
        std::vector<SyncFileInfo> status;
        if (client.backend->GetStatus(status) != VXCORE_OK) callback_query_ok = false;
        progress.push_back(event);
      },
      &cookie);
  if (sync_result != VXCORE_OK) {
    std::cerr << "Composite sync result " << sync_result << ": " << client.backend->GetLastError()
              << "\n"
              << Control({{"action", "requests"}}).dump() << "\n";
  }
  ASSERT_EQ(sync_result, VXCORE_OK);
  ASSERT_TRUE(callback_query_ok);
  ASSERT_FALSE(progress.empty());
  bool terminal = false, transfer = false;
  for (const auto &event : progress) {
    ASSERT_TRUE(event.percentage >= 0 && event.percentage <= 100);
    terminal = terminal || event.current_state == SyncState::kIdle;
    transfer = transfer || (event.percentage > 0 && event.percentage < 100);
    ASSERT_TRUE(event.message.find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) ==
                std::string::npos);
  }
  ASSERT_TRUE(terminal && transfer);
  const auto initial_provider = client.backend->GetCredsProviderSnapshot();
  ASSERT_NOT_NULL(initial_provider.get());
  client.Write("payload.bin", "operation-owned credential snapshot");
  ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
  {
    FaultScope cleanup;
    Control({{"action", "barrier"},
             {"id", "provider-snapshot"},
             {"method", "PROPFIND"},
             {"path", ""},
             {"phase", "before"}});
    NetworkCall work(*client.backend);
    ASSERT_TRUE(WaitBarrier("provider-snapshot"));
    client.backend->ReplaceCredsProvider(Provider("not-valid-on-this-server"));
    Control({{"action", "release"}, {"id", "provider-snapshot"}});
    ASSERT_TRUE(work.result.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    ASSERT_EQ(work.result.get(), VXCORE_OK);
  }
  std::vector<std::string> changed;
  ASSERT_EQ(client.backend->ApplySync({}, changed), VXCORE_OK);
  ASSERT_EQ(*RemoteBytes("payload.bin"), "operation-owned credential snapshot");
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_AUTH_FAILED);
  client.backend->ReplaceCredsProvider(initial_provider);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto new_secret = "rotated-" + RandomName();
  Control({{"action", "configure"}, {"password", new_secret}});
  client.Write("payload.bin", "rotated credentials upload");
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_AUTH_FAILED);
  ASSERT_TRUE(client.backend->GetLastError().find(new_secret) == std::string::npos);
  client.backend->ReplaceCredsProvider(Provider(new_secret));
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(CommonFile(client, "payload.bin"));
  ASSERT_EQ(client.State().at("usernameHash"), Hash(Environment("VXCORE_WEBDAV_TEST_USERNAME")));
  for (const auto &entry : fs::recursive_directory_iterator(client.Path(kPrivatePath))) {
    if (!entry.is_regular_file()) continue;
    const auto contents = ReadBytes(entry.path());
    ASSERT_TRUE(contents.find(new_secret) == std::string::npos);
    ASSERT_TRUE(contents.find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) == std::string::npos);
  }
  const auto rotated_tree = RemoteTree();
  for (const auto &entry : rotated_tree.items()) {
    ASSERT_TRUE(entry.key().find(new_secret) == std::string::npos);
    ASSERT_TRUE(entry.key().find("vx_sync") == std::string::npos);
  }
  return 0;
}

// Native process creation avoids shell quoting and never puts credentials on an
// argument list. The runner, not a shared singleton Context, allocates each device.
class Child {
 public:
  Workspace control;
  bool running = false;
#ifdef _WIN32
  PROCESS_INFORMATION process{};
#else
  pid_t process = -1;
#endif

  Child(const std::string &mode, const fs::path &root) {
    const std::vector<std::string> arguments = {Environment("VXCORE_WEBDAV_TEST_PYTHON"),
                                                Environment("VXCORE_WEBDAV_TEST_RUNNER"),
                                                "--reuse-fixture",
                                                "--",
                                                executable,
                                                "--child",
                                                mode,
                                                PathToUtf8(root),
                                                PathToUtf8(control.path)};
#ifdef _WIN32
    auto quote = [](const std::string &argument) {
      const auto wide = PathFromUtf8(argument).native();
      std::wstring result = L"\"";
      size_t slashes = 0;
      for (const auto character : wide) {
        if (character == L'\\') {
          ++slashes;
          continue;
        }
        if (character == L'"')
          result.append(slashes * 2 + 1, L'\\');
        else
          result.append(slashes, L'\\');
        slashes = 0;
        result += character;
      }
      result.append(slashes * 2, L'\\');
      result += L'"';
      return result;
    };
    std::wstring command;
    for (const auto &argument : arguments) {
      if (!command.empty()) command += L' ';
      command += quote(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    const auto python = PathFromUtf8(arguments.front()).native();
    if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process))
      throw std::runtime_error("Could not launch isolated fixture child");
    CloseHandle(process.hThread);
#else
    process = fork();
    if (process == 0) {
      std::vector<char *> argv;
      for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
      argv.push_back(nullptr);
      execv(argv.front(), argv.data());
      _exit(127);
    }
    if (process < 0) throw std::runtime_error("Could not launch isolated fixture child");
#endif
    running = true;
  }

  bool WaitFile(const char *name, std::chrono::seconds timeout = std::chrono::seconds(30)) const {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      if (fs::is_regular_file(control.path / name)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  void Continue() { WriteBytes(control.path / "go", "continue"); }

  bool KillClient() {
    if (!fs::is_regular_file(control.path / "ready.json")) return false;
    const auto pid = ReadJson(control.path / "ready.json").at("pid").get<uint64_t>();
#ifdef _WIN32
    const auto child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!child) return false;
    const bool killed = TerminateProcess(child, 91) != 0;
    const bool stopped = WaitForSingleObject(child, 10000) == WAIT_OBJECT_0;
    CloseHandle(child);
    return killed && stopped;
#else
    return kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
  }

  int Wait() {
#ifdef _WIN32
    if (WaitForSingleObject(process.hProcess, 45000) != WAIT_OBJECT_0)
      throw std::runtime_error("Isolated fixture child timed out");
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    process.hProcess = nullptr;
    running = false;
    return static_cast<int>(result);
#else
    const auto deadline = Clock::now() + std::chrono::seconds(45);
    while (Clock::now() < deadline) {
      int status = 0;
      if (waitpid(process, &status, WNOHANG) == process) {
        running = false;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Isolated fixture child timed out");
#endif
  }

  Json Report() const { return ReadJson(control.path / "result.json"); }

  ~Child() {
    if (!running) return;
    try {
      KillClient();
      Control({{"action", "clear_faults"}});
    } catch (...) {
    }
#ifdef _WIN32
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 92);
      WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hProcess);
#else
    kill(process, SIGTERM);
    int status = 0;
    waitpid(process, &status, 0);
#endif
  }
};

bool ChildReady(const fs::path &control) {
#ifdef _WIN32
  const auto pid = GetCurrentProcessId();
#else
  const auto pid = getpid();
#endif
  WriteBytes(control / "ready.json",
             Json{{"pid", pid}, {"temp", PathToUtf8(fs::temp_directory_path())}}.dump());
  const auto deadline = Clock::now() + std::chrono::seconds(40);
  while (Clock::now() < deadline) {
    if (fs::is_regular_file(control / "go")) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::string BinaryPayload() {
  std::string bytes;
  bytes.reserve(256 * 32);
  for (size_t i = 0; i != 256 * 32; ++i) bytes += static_cast<char>(i % 256);
  return bytes;
}

int ChildClone(const std::string &mode, const fs::path &root, const fs::path &control) {
  Context context;
  ASSERT_TRUE(ChildReady(control));
  const auto config = Configuration();
  const auto config_json = config.ToJson().dump();
  const auto credentials = CredentialsJson();
  CoreString id;
  const auto cursor = RequestCursor();
  const auto result =
      vxcore_sync_clone(context.value, PathToUtf8(root).c_str(), config_json.c_str(),
                        mode == "clone-anonymous" ? nullptr : credentials.c_str(), &id.value);
  if (mode == "clone-unstable") {
    ASSERT_NE(result, VXCORE_OK);
    ASSERT_NULL(id.value);
    ASSERT_TRUE(NoMutationsSince(cursor));
    WriteBytes(control / "result.json", Json{{"result", result}}.dump());
    return 0;
  }
  ASSERT_EQ(result, VXCORE_OK);
  ASSERT_NOT_NULL(id.value);
  int registered = 1;
  ASSERT_EQ(vxcore_sync_is_registered(context.value, id.value, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 0);
  ASSERT_TRUE(NoMutationsSince(cursor));
  ASSERT_EQ(ReadJson(root / PathFromUtf8(kConfigPath)).at("id"), id.value);
  ASSERT_EQ(ReadBytes(root / "note.md"), "indexed device A\r\n");
  ASSERT_EQ(ReadBytes(root / PathFromUtf8(u8"nested/笔记 space # %.md")), "UTF-8 path\r\n");
  ASSERT_EQ(ReadBytes(root / "assets/attachment.bin"), BinaryPayload());
  ASSERT_EQ(ReadBytes(root / "assets/comments/note.json"), "{\"comments\":[\"sidecar\"]}\r\n");
  ASSERT_EQ(ReadBytes(root / "recycle/deleted.md"), "recycled original\n");
  ASSERT_TRUE(fs::is_directory(root / "nested/empty"));
  ASSERT_FALSE(fs::exists(root / ".git"));
  if (mode == "clone-anonymous") {
    CoreString file_id;
    ASSERT_EQ(
        vxcore_file_create(context.value, id.value, ".", "anonymous-local.md", &file_id.value),
        VXCORE_OK);
    ASSERT_FALSE(Inspect("anonymous-local.md").value("exists", false));
  } else {
    if (mode == "clone-union") {
      // A never-attached device can have ordinary files but no sync history.
      // Erasing only this fresh clone's clean baseline models that input.
      fs::remove_all(root / PathFromUtf8(kPrivatePath));
      fs::remove(root / "note.md");
      WriteBytes(root / "union-local.md", "local without a baseline");
    } else {
      WriteBytes(root / "note.md", "edited by device B\r\n");
      WriteBytes(root / "unindexed-B.md", "external unindexed bytes\r\n");
      fs::remove(root / "recycle/deleted.md");
    }
    ASSERT_EQ(vxcore_sync_enable(context.value, id.value, config_json.c_str(), credentials.c_str()),
              VXCORE_OK);
    ASSERT_EQ(vxcore_sync_trigger(context.value, id.value), VXCORE_OK);
    if (mode == "clone-union") ASSERT_EQ(ReadBytes(root / "note.md"), "indexed device A\r\n");
  }
  WriteBytes(control / "result.json", Json{{"result", VXCORE_OK}, {"id", id.value}}.dump());
  return 0;
}

int ChildSync(const std::string &mode, const fs::path &root, const fs::path &control) {
  Context context;
  CoreString id;
  ASSERT_EQ(vxcore_notebook_open(context.value, PathToUtf8(root).c_str(), &id.value), VXCORE_OK);
  const auto config = Configuration();
  WebDavSyncBackend backend(config, Provider());
  ASSERT_EQ(backend.Initialize(PathToUtf8(root), config), VXCORE_OK);
  ASSERT_EQ(backend.StageAndCommit(nullptr), VXCORE_OK);
  if (mode == "apply-crash") ASSERT_EQ(backend.FetchRebasePush(), VXCORE_OK);
  ASSERT_TRUE(ChildReady(control));
  auto result = mode == "apply-crash" ? VXCORE_OK : backend.FetchRebasePush();
  std::vector<std::string> changed;
  if (result == VXCORE_OK) result = backend.ApplySync({}, changed);
  WriteBytes(control / "result.json",
             Json{{"result", result}, {"changed", changed}, {"id", id.value}}.dump());
  if (mode == "apply-crash") {
    // The parent observes the actual installed bytes and journal before killing
    // this process. No destructor or in-process restart performs recovery.
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  ASSERT_EQ(result, VXCORE_OK);
  return 0;
}

int DispatchChild(const std::string &mode, const fs::path &root, const fs::path &control) {
  if (mode.compare(0, 6, "clone-") == 0) return ChildClone(mode, root, control);
  if (mode == "sync" || mode == "apply-crash") return ChildSync(mode, root, control);
  throw std::runtime_error("Unknown WebDAV child mode");
}

int PopulateRichNotebook(Client &client) {
  CoreString note;
  ASSERT_EQ(
      vxcore_file_create(client.context.value, client.id.c_str(), ".", "note.md", &note.value),
      VXCORE_OK);
  client.Write("note.md", "indexed device A\r\n");
  client.Write(u8"nested/笔记 space # %.md", "UTF-8 path\r\n");
  client.Write("assets/attachment.bin", BinaryPayload());
  client.Write("assets/comments/note.json", "{\"comments\":[\"sidecar\"]}\r\n");
  client.Write("recycle/deleted.md", "recycled original\n");
  fs::create_directories(client.Path("nested/empty"));
  return 0;
}

int TestTwoDeviceCloneAndPublication() {
  for (const auto *mode : {"clone-edit", "clone-union", "clone-anonymous"}) {
    Control({{"action", "reset"}});
    Client client;
    ASSERT_EQ(PopulateRichNotebook(client), 0);
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    if (std::string(mode) == "clone-anonymous")
      Control({{"action", "configure"}, {"anonymous_read", true}});
    Workspace second_device;
    const auto root = second_device.path / "device-B";
    fs::create_directory(root);
    Child child(mode, root);
    ASSERT_TRUE(child.WaitFile("ready.json"));
    const auto child_temp =
        ReadJson(child.control.path / "ready.json").at("temp").get<std::string>();
    ASSERT_TRUE(fs::canonical(PathFromUtf8(child_temp)) !=
                fs::canonical(fs::temp_directory_path()));
    child.Continue();
    ASSERT_EQ(child.Wait(), 0);
    ASSERT_EQ(child.Report().at("id"), client.id);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    if (std::string(mode) == "clone-edit") {
      ASSERT_EQ(client.Read("note.md"), "edited by device B\r\n");
      ASSERT_EQ(client.Read("unindexed-B.md"), "external unindexed bytes\r\n");
      ASSERT_FALSE(fs::exists(client.Path("recycle/deleted.md")));
      const auto index = ReadJson(client.Path("vx_notebook/contents/vx.json")).dump();
      ASSERT_TRUE(index.find("unindexed-B.md") == std::string::npos);
    } else if (std::string(mode) == "clone-union") {
      ASSERT_EQ(client.Read("note.md"), "indexed device A\r\n");
      ASSERT_EQ(client.Read("union-local.md"), "local without a baseline");
    }
    ASSERT_TRUE(CommonFile(client, "assets/attachment.bin"));
    ASSERT_EQ(Json::parse(*RemoteBytes(kConfigPath)).at("id"), client.id);
    const auto published_tree = RemoteTree();
    for (const auto &item : published_tree.items()) {
      ASSERT_TRUE(item.key().find(".git") == std::string::npos);
      ASSERT_TRUE(item.key().find("vx_notebook/vx_sync") == std::string::npos);
      ASSERT_TRUE(item.key().find("vx_notebook/vx_transfer") == std::string::npos);
      ASSERT_TRUE(item.key().find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) == std::string::npos);
      if (item.value().at("kind") != "file") continue;
      const auto bytes = *RemoteBytes(item.key());
      ASSERT_TRUE(bytes.find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) == std::string::npos);
      ASSERT_TRUE(bytes.find(Environment("VXCORE_WEBDAV_TEST_USERNAME")) == std::string::npos);
    }
  }
  return 0;
}

int TestCloneRevalidatesAndCancellation() {
  for (bool new_resource : {false, true}) {
    Control({{"action", "reset"}});
    Client client;
    ASSERT_EQ(PopulateRichNotebook(client), 0);
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    Workspace clone;
    FaultScope cleanup;
    Child child("clone-unstable", clone.path);
    ASSERT_TRUE(child.WaitFile("ready.json"));
    Control({{"action", "barrier"},
             {"id", "clone-read"},
             {"method", "GET"},
             {"path", "note.md"},
             {"phase", "after"}});
    child.Continue();
    ASSERT_TRUE(WaitBarrier("clone-read"));
    if (new_resource)
      PutRemote("added-during-clone.md", "not in original enumeration");
    else
      PutRemote("note.md", "edited during clone");
    Control({{"action", "release"}, {"id", "clone-read"}});
    ASSERT_EQ(child.Wait(), 0);
    ASSERT_NE(child.Report().at("result").get<int>(), int(VXCORE_OK));
    ASSERT_EQ(*RemoteBytes(new_resource ? "added-during-clone.md" : "note.md"),
              new_resource ? "not in original enumeration" : "edited during clone");
  }
  Control({{"action", "reset"}});
  Client client;
  ASSERT_EQ(PopulateRichNotebook(client), 0);
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  Workspace target;
  auto token = std::make_shared<SyncCancellation>();
  WebDavSyncBackend clone(client.config, Provider());
  clone.SetCancellation(token);
  FaultScope cleanup;
  Control({{"action", "barrier"},
           {"id", "cancel-clone"},
           {"method", "GET"},
           {"path", "assets/attachment.bin"},
           {"phase", "after_headers"}});
  auto work = std::async(std::launch::async,
                         [&] { return clone.Clone(PathToUtf8(target.path), client.config); });
  const bool reached = WaitBarrier("cancel-clone");
  token->Cancel();
  const bool timely = work.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  Control({{"action", "release"}, {"id", "cancel-clone"}});
  ASSERT_TRUE(reached && timely);
  ASSERT_EQ(work.get(), VXCORE_ERR_CANCELLED);
  ASSERT_FALSE(fs::exists(target.path / "assets/attachment.bin"));
  ASSERT_FALSE(clone.IsInitialized());
  ASSERT_EQ(*RemoteBytes("assets/attachment.bin"), BinaryPayload());
  return 0;
}

int TestLostAcknowledgementsAreNotReplayed() {
  for (bool deletion : {false, true}) {
    Control({{"action", "reset"}});
    Client client;
    client.Write("uncertain.md", "old recoverable revision");
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    if (deletion)
      fs::remove(client.Path("uncertain.md"));
    else
      client.Write("uncertain.md", "intended revision");
    FaultScope cleanup;
    Control({{"action", "fault"},
             {"method", deletion ? "DELETE" : "MOVE"},
             {"path", deletion ? "uncertain.md" : "*"},
             {"phase", "after"},
             {"effect", "drop"}});
    const auto first_result = client.Sync();
    ASSERT_EQ(first_result, VXCORE_ERR_SYNC_NETWORK);
    ASSERT_TRUE(PendingHasOperations(client.root));
    ASSERT_TRUE(SafeJournal(client));
    ASSERT_TRUE(SnapshotContains(client, "old recoverable revision"));
    if (deletion)
      ASSERT_FALSE(RemoteBytes("uncertain.md").has_value());
    else
      ASSERT_EQ(*RemoteBytes("uncertain.md"), "intended revision");
    Control({{"action", "clear_faults"}});
    client.backend.reset();
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    const auto cursor = RequestCursor();
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    // The mutation already happened. Recovery verifies it, never repeats it.
    ASSERT_TRUE(NoMutationsSince(cursor));
    ASSERT_FALSE(PendingHasOperations(client.root));
    if (deletion)
      ASSERT_FALSE(client.State().at("entries").contains("uncertain.md"));
    else
      ASSERT_TRUE(CommonFile(client, "uncertain.md"));
  }
  return 0;
}

int TestProcessCrashJournalBoundaries() {
  struct Boundary {
    const char *method;
    const char *phase;
    bool deletion;
  };
  const std::array<Boundary, 4> boundaries{{
      {"PUT", "before", false},  // Durable intent, no upload.
      {"PUT", "after", false},   // Complete owned scratch, no acknowledgement.
      {"MOVE", "after", false},  // Published destination, no acknowledgement.
      {"DELETE", "after", true}  // Deleted destination, no acknowledgement.
  }};
  for (const auto &boundary : boundaries) {
    Control({{"action", "reset"}});
    Client client;
    client.Write("crash.md", "old version retained until acknowledgement");
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    if (boundary.deletion)
      fs::remove(client.Path("crash.md"));
    else
      client.Write("crash.md", "complete new version after crash");
    client.backend.reset();
    ASSERT_EQ(vxcore_notebook_close(client.context.value, client.id.c_str()), VXCORE_OK);
    FaultScope cleanup;
    {
      Child child("sync", client.root);
      ASSERT_TRUE(child.WaitFile("ready.json"));
      Control({{"action", "barrier"},
               {"id", "crash-boundary"},
               {"method", boundary.method},
               {"path", boundary.deletion ? "crash.md" : "*"},
               {"phase", boundary.phase}});
      child.Continue();
      ASSERT_TRUE(WaitBarrier("crash-boundary"));
      ASSERT_TRUE(PendingHasOperations(client.root));
      ASSERT_TRUE(SafeJournal(client));
      ASSERT_TRUE(SnapshotContains(client, "old version retained until acknowledgement"));
      if (!boundary.deletion)
        ASSERT_TRUE(SnapshotContains(client, "complete new version after crash"));
      if (std::string(boundary.method) == "PUT" && std::string(boundary.phase) == "after") {
        bool owned_upload = false;
        const auto scratch_tree = RemoteTree();
        for (const auto &entry : scratch_tree.items()) {
          if (entry.key().find(".vnote-webdav-tmp-") != 0 || entry.value().at("kind") != "file")
            continue;
          if (entry.value().at("sha256") == Hash("complete new version after crash"))
            owned_upload = true;
        }
        ASSERT_TRUE(owned_upload);
      }
      ASSERT_TRUE(child.KillClient());
      ASSERT_NE(child.Wait(), 0);
      ASSERT_FALSE(fs::exists(child.control.path / "result.json"));
      Control({{"action", "release"}, {"id", "crash-boundary"}});
    }
    Control({{"action", "clear_faults"}});
    Child restarted("sync", client.root);
    ASSERT_TRUE(restarted.WaitFile("ready.json"));
    restarted.Continue();
    ASSERT_EQ(restarted.Wait(), 0);
    ASSERT_EQ(restarted.Report().at("id"), client.id);
    ASSERT_FALSE(PendingHasOperations(client.root));
    if (boundary.deletion) {
      ASSERT_FALSE(fs::exists(client.Path("crash.md")));
      ASSERT_FALSE(RemoteBytes("crash.md").has_value());
      ASSERT_FALSE(client.State().at("entries").contains("crash.md"));
    } else {
      ASSERT_EQ(client.Read("crash.md"), "complete new version after crash");
      ASSERT_TRUE(CommonFile(client, "crash.md"));
    }
  }
  return 0;
}

int TestCrashAfterLocalReplacementBeforeBaseline() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("installed.md", "old local revision");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  PutRemote("installed.md", "remote revision to install");
  client.backend.reset();
  ASSERT_EQ(vxcore_notebook_close(client.context.value, client.id.c_str()), VXCORE_OK);
  const auto state_file = client.Path(kStatePath);
  const auto held_state = client.Path(std::string(kPrivatePath) + "/state-held-by-test");
  Child child("apply-crash", client.root);
  ASSERT_TRUE(child.WaitFile("ready.json"));
  ASSERT_TRUE(PendingHasOperations(client.root));
  ASSERT_EQ(client.Read("installed.md"), "old local revision");
  // A nonempty directory at the final publication target makes the real atomic
  // baseline rename fail on Windows and POSIX. It does not corrupt journal data.
  fs::rename(state_file, held_state);
  fs::create_directory(state_file);
  WriteBytes(state_file / "owned-obstruction", "prevent baseline commit");
  child.Continue();
  ASSERT_TRUE(child.WaitFile("result.json"));
  ASSERT_NE(child.Report().at("result").get<int>(), int(VXCORE_OK));
  ASSERT_EQ(client.Read("installed.md"), "remote revision to install");
  const auto changed = child.Report().at("changed").get<std::vector<std::string>>();
  ASSERT_TRUE(std::find(changed.begin(), changed.end(), "installed.md") != changed.end());
  ASSERT_TRUE(PendingHasOperations(client.root));
  ASSERT_TRUE(SnapshotContains(client, "old local revision"));
  ASSERT_TRUE(SnapshotContains(client, "remote revision to install"));
  ASSERT_TRUE(child.KillClient());
  ASSERT_NE(child.Wait(), 0);
  fs::remove(state_file / "owned-obstruction");
  fs::remove(state_file);
  fs::rename(held_state, state_file);
  Child restarted("sync", client.root);
  ASSERT_TRUE(restarted.WaitFile("ready.json"));
  restarted.Continue();
  ASSERT_EQ(restarted.Wait(), 0);
  ASSERT_TRUE(CommonFile(client, "installed.md"));
  ASSERT_FALSE(PendingHasOperations(client.root));
  return 0;
}

int TestRecoveryPreservesDivergentVersion() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("recovery.md", "old common");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  client.Write("recovery.md", "original intended local");
  FaultScope cleanup;
  Control({{"action", "fault"}, {"method", "MOVE"}, {"phase", "after"}, {"effect", "drop"}});
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_NETWORK);
  ASSERT_TRUE(PendingHasOperations(client.root));
  PutRemote("recovery.md", "external newer version after lost acknowledgement");
  Control({{"action", "clear_faults"}});
  client.backend.reset();
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  const auto before = RemoteTree();
  ASSERT_EQ(client.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_TRUE(RemoteTree() == before);
  ASSERT_EQ(client.Read("recovery.md"), "original intended local");
  ASSERT_TRUE(ConflictPaths(*client.backend).count("recovery.md"));
  ASSERT_TRUE(SnapshotContains(client, "old common"));
  ASSERT_TRUE(SnapshotContains(client, "original intended local"));
  ASSERT_TRUE(SnapshotContains(client, "external newer version after lost acknowledgement"));
  return 0;
}

int TestDamagedStateAndJournalFailClosed() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("preserved.md", "baseline bytes");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  client.backend.reset();
  const auto original = client.Read(kStatePath);
  const auto parsed = Json::parse(original);
  std::vector<std::string> damaged{"{not valid json"};
  for (const auto &change : std::vector<std::pair<std::string, Json>>{
           {"version", 2},
           {"notebookId", "a2c2fb95-c708-4d55-a789-37917f007590"},
           {"remoteUrl", "https://different.invalid/notebook/"},
           {"usernameHash", "bad hash"}}) {
    auto state = parsed;
    state[change.first] = change.second;
    damaged.push_back(state.dump());
  }
  auto unsafe = parsed;
  unsafe["entries"]["../../outside.md"] = unsafe.at("entries").at("preserved.md");
  damaged.push_back(unsafe.dump());
  for (const auto &bytes : damaged) {
    client.Write(kStatePath, bytes);
    const auto before = RemoteTree();
    ASSERT_NE(client.Attach(), VXCORE_OK);
    ASSERT_FALSE(client.backend->IsInitialized());
    ASSERT_EQ(client.Read(kStatePath), bytes);
    ASSERT_EQ(client.Read("preserved.md"), "baseline bytes");
    ASSERT_TRUE(RemoteTree() == before);
    client.backend.reset();
  }
  client.Write(kStatePath, original);
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  PutRemote("preserved.md", "incoming bytes");
  ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
  ASSERT_EQ(client.backend->FetchRebasePush(), VXCORE_OK);
  ASSERT_TRUE(PendingHasOperations(client.root));
  const auto pending = ReadJson(client.Path(kPendingPath));
  client.backend.reset();
  Workspace outside;
  WriteBytes(outside.path / "sentinel", "outside notebook must survive");
  std::vector<Json> invalid;
  for (const auto &field : {"path", "sourceSnapshot", "previousSnapshot", "scratchUrl"}) {
    auto changed = pending;
    changed["operations"][0][field] = field == std::string("scratchUrl")
                                          ? "https://other.invalid/notebook/owned"
                                          : PathToUtf8(outside.path / "sentinel");
    invalid.push_back(std::move(changed));
  }
  {
    auto changed = pending;
    changed["operations"][0]["path"] = "../escape";
    invalid.push_back(std::move(changed));
  }
  {
    auto changed = pending;
    changed["operations"][0]["sourceSnapshot"] = "snapshots/../state.json";
    invalid.push_back(std::move(changed));
  }
  {
    auto changed = pending;
    changed["operations"][0]["stage"] = "unconditionallyOverwrite";
    invalid.push_back(std::move(changed));
  }
  for (const auto &value : invalid) {
    client.Write(kPendingPath, value.dump());
    const auto before = RemoteTree();
    ASSERT_NE(client.Attach(), VXCORE_OK);
    ASSERT_FALSE(client.backend->IsInitialized());
    ASSERT_EQ(ReadJson(client.Path(kPendingPath)), value);
    ASSERT_EQ(ReadBytes(outside.path / "sentinel"), "outside notebook must survive");
    ASSERT_EQ(client.Read("preserved.md"), "baseline bytes");
    ASSERT_TRUE(RemoteTree() == before);
    client.backend.reset();
  }
  client.Write(kPendingPath, "{broken pending");
  ASSERT_NE(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Read(kPendingPath), "{broken pending");
  client.Write(kPendingPath, pending.dump());
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(CommonFile(client, "preserved.md"));
  return 0;
}

int TestCapiDeferredApplyAndErrorOutputs() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("first.md", "old first");
  client.Write("protected.md", "old protected");
  const auto config = client.config.ToJson().dump();
  const auto credentials = CredentialsJson();
  ASSERT_EQ(vxcore_sync_enable(client.context.value, client.id.c_str(), config.c_str(),
                               credentials.c_str()),
            VXCORE_OK);
  uint32_t capabilities = 0;
  ASSERT_EQ(vxcore_sync_get_capabilities(client.context.value, client.id.c_str(), &capabilities),
            VXCORE_OK);
  ASSERT_TRUE(capabilities & static_cast<uint32_t>(SyncCapability::DeferredLocalApply));
  ASSERT_EQ(vxcore_sync_trigger(client.context.value, client.id.c_str()), VXCORE_OK);
  int64_t last_success = 0;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(client.context.value, client.id.c_str(), &last_success),
            VXCORE_OK);
  ASSERT_TRUE(last_success > 0);
  PutRemote("first.md", "incoming first");
  PutRemote("protected.md", "incoming protected");
  int committed = 1;
  ASSERT_EQ(vxcore_sync_stage_only(client.context.value, client.id.c_str(), nullptr, &committed),
            VXCORE_OK);
  ASSERT_EQ(committed, 0);
  ASSERT_EQ(vxcore_sync_network_phase(client.context.value, client.id.c_str(), nullptr), VXCORE_OK);
  ASSERT_EQ(client.Read("first.md"), "old first");
  for (const auto *input : {"{bad", "{}", "[7]", "[\"../outside\"]", "[\"/absolute\"]",
                            "[\"C:/drive\"]", "[\"a\\\\b\"]", "[\"a\\u0000b\"]"}) {
    char *output = reinterpret_cast<char *>(uintptr_t(1));
    ASSERT_EQ(
        vxcore_sync_apply_phase(client.context.value, client.id.c_str(), nullptr, input, &output),
        VXCORE_ERR_INVALID_PARAM);
    ASSERT_NULL(output);
    ASSERT_EQ(client.Read("first.md"), "old first");
  }
  ASSERT_EQ(
      vxcore_sync_apply_phase(client.context.value, client.id.c_str(), nullptr, nullptr, nullptr),
      VXCORE_ERR_NULL_POINTER);
  CoreString changed;
  const auto cursor = RequestCursor();
  ASSERT_EQ(vxcore_sync_apply_phase(client.context.value, client.id.c_str(), nullptr,
                                    "[\"protected.md\"]", &changed.value),
            VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_NOT_NULL(changed.value);
  ASSERT_TRUE(Json::parse(changed.value).empty());
  ASSERT_EQ(RequestCursor(), cursor);
  ASSERT_EQ(client.Read("first.md"), "old first");
  ASSERT_EQ(client.Read("protected.md"), "old protected");
  CoreString conflicts;
  ASSERT_EQ(vxcore_sync_get_conflicts(client.context.value, client.id.c_str(), &conflicts.value),
            VXCORE_OK);
  const auto conflict_json = Json::parse(conflicts.value);
  bool saw_protected = false;
  for (const auto &conflict : conflict_json.at("conflicts")) {
    if (conflict.at("path") != "protected.md") continue;
    saw_protected = true;
    ASSERT_TRUE(conflict.at(kJsonKeyCanKeepBoth).get<bool>());
  }
  ASSERT_TRUE(saw_protected);
  int64_t after_failure = 0;
  ASSERT_EQ(vxcore_sync_get_last_sync_utc(client.context.value, client.id.c_str(), &after_failure),
            VXCORE_OK);
  ASSERT_EQ(after_failure, last_success);
  ASSERT_EQ(vxcore_sync_resolve_conflict(client.context.value, client.id.c_str(), "protected.md",
                                         "keep_local"),
            VXCORE_OK);
  ASSERT_EQ(vxcore_sync_trigger(client.context.value, client.id.c_str()), VXCORE_OK);
  ASSERT_EQ(client.Read("first.md"), "incoming first");
  ASSERT_EQ(*RemoteBytes("protected.md"), "old protected");
  client.Write("collision", "local file");
  PutRemote("collision/child.md", "remote child");
  const auto before = RemoteTree();
  ASSERT_EQ(vxcore_sync_trigger(client.context.value, client.id.c_str()), VXCORE_ERR_UNSUPPORTED);
  const char *error = nullptr;
  ASSERT_EQ(vxcore_context_get_last_error(client.context.value, &error), VXCORE_OK);
  ASSERT_NOT_NULL(error);
  ASSERT_EQ(std::string(error), kCollisionMessage);
  ASSERT_TRUE(RemoteTree() == before);
  return 0;
}

int TestCapiCloneCancellationClearsOutput() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("remote.md", "remote");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  Workspace target;
  VxCoreSyncCancellation *token = vxcore_sync_create_cancellation();
  ASSERT_NOT_NULL(token);
  vxcore_sync_cancel(token);
  char *id = reinterpret_cast<char *>(uintptr_t(1));
  const auto config = client.config.ToJson().dump();
  const auto credentials = CredentialsJson();
  const auto cursor = RequestCursor();
  const auto result =
      vxcore_sync_clone_cancellable(client.context.value, PathToUtf8(target.path).c_str(),
                                    config.c_str(), credentials.c_str(), token, &id);
  vxcore_sync_free_cancellation(token);
  ASSERT_EQ(result, VXCORE_ERR_CANCELLED);
  ASSERT_NULL(id);
  ASSERT_EQ(RequestCursor(), cursor);
  ASSERT_TRUE(fs::is_empty(target.path));
  return 0;
}

int TestUnstableRemoteDownloadNeverPublishesPartialBytes() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("attachment.bin", BinaryPayload());
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  const auto baseline = client.Read(kStatePath);
  PutRemote("attachment.bin", std::string(256 * 1024, 'r'));
  FaultScope cleanup;
  Control({{"action", "fault"},
           {"method", "GET"},
           {"path", "attachment.bin"},
           {"effect", "truncate"},
           {"bytes", 1024}});
  ASSERT_NE(client.Sync(), VXCORE_OK);
  ASSERT_EQ(client.Read("attachment.bin"), BinaryPayload());
  ASSERT_EQ(client.Read(kStatePath), baseline);
  Control({{"action", "clear_faults"}});
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_EQ(client.Read("attachment.bin"), std::string(256 * 1024, 'r'));
  ASSERT_TRUE(CommonFile(client, "attachment.bin"));
  return 0;
}

int TestMetadataValidationPrecedesPublication() {
  for (const auto *path : {kConfigPath, "vx_notebook/contents/vx.json"}) {
    Control({{"action", "reset"}});
    Client client;
    client.Write("a-payload.md", "old local payload");
    ASSERT_EQ(client.Attach(), VXCORE_OK);
    ASSERT_EQ(client.Sync(), VXCORE_OK);
    const auto original = client.Read(path);
    const auto baseline = client.Read(kStatePath);
    PutRemote("a-payload.md", "incoming payload must remain staged");
    PutRemote(path, "{invalid metadata");
    ASSERT_NE(client.Sync(), VXCORE_OK);
    ASSERT_EQ(client.Read(path), original);
    ASSERT_EQ(client.Read("a-payload.md"), "old local payload");
    ASSERT_EQ(client.Read(kStatePath), baseline);
  }
  return 0;
}

int TestIncomingDeletionChecksLocalFingerprint() {
  Control({{"action", "reset"}});
  Client client;
  client.Write("delete.md", "baseline");
  client.Write("another.md", "other baseline");
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  Control({{"action", "delete"}, {"path", "delete.md"}});
  PutRemote("another.md", "incoming other");
  ASSERT_EQ(client.backend->StageAndCommit(nullptr), VXCORE_OK);
  ASSERT_EQ(client.backend->FetchRebasePush(), VXCORE_OK);
  client.Write("delete.md", "save completed during network");
  std::vector<std::string> changed;
  ASSERT_EQ(client.backend->ApplySync({}, changed), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_TRUE(changed.empty());
  ASSERT_EQ(client.Read("delete.md"), "save completed during network");
  ASSERT_EQ(client.Read("another.md"), "other baseline");
  ASSERT_TRUE(ConflictPaths(*client.backend).count("delete.md"));
  ASSERT_FALSE(RemoteBytes("delete.md").has_value());
  ASSERT_EQ(client.backend->ResolveConflict("delete.md", SyncConflictResolution::kKeepLocal),
            VXCORE_OK);
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_EQ(*RemoteBytes("delete.md"), "save completed during network");
  ASSERT_EQ(client.Read("another.md"), "incoming other");
  return 0;
}

int TestFailedProbeCleanupIsRecoverable() {
  Control({{"action", "reset"}});
  PutRemote(".vnote-webdav-tmp-other-client", "never garbage collect another client's scratch");
  Client client;
  client.Write("unpublished.md", "not published by Initialize");
  client.backend = std::make_unique<WebDavSyncBackend>(client.config, Provider());
  auto token = std::make_shared<SyncCancellation>();
  client.backend->SetCancellation(token);
  FaultScope cleanup;
  Control(
      {{"action", "barrier"}, {"id", "probe-delete"}, {"method", "DELETE"}, {"phase", "before"}});
  auto initialize = std::async(std::launch::async, [&] {
    return client.backend->Initialize(PathToUtf8(client.root), client.config);
  });
  const bool reached = WaitBarrier("probe-delete");
  // The blocked request already captured its fixture rules. This fault applies
  // to the next DELETE (cleanup), not the deliberately incorrect precondition.
  Control({{"action", "fault"},
           {"method", "DELETE"},
           {"effect", "status"},
           {"status", 507},
           {"count", -1}});
  Control({{"action", "release"}, {"id", "probe-delete"}});
  const bool completed = initialize.wait_for(std::chrono::seconds(15)) == std::future_status::ready;
  if (!completed) token->Cancel();
  const auto result = initialize.get();
  ASSERT_TRUE(reached && completed);
  ASSERT_NE(result, VXCORE_OK);
  ASSERT_FALSE(client.backend->IsInitialized());
  ASSERT_FALSE(client.backend->GetLastError().empty());
  ASSERT_FALSE(RemoteBytes("unpublished.md").has_value());
  ASSERT_FALSE(RemoteBytes(kConfigPath).has_value());
  const auto remaining = RemoteTree();
  std::set<std::string> owned;
  for (const auto &entry : remaining.items()) {
    if (entry.key().find(".vnote-webdav-tmp-") == 0 &&
        entry.key() != ".vnote-webdav-tmp-other-client")
      owned.insert(entry.key());
  }
  ASSERT_FALSE(owned.empty());
  bool journaled = false;
  for (const auto &entry : fs::recursive_directory_iterator(client.Path(kPrivatePath))) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
    const auto bytes = ReadBytes(entry.path());
    for (const auto &scratch : owned)
      journaled = journaled || bytes.find(scratch) != std::string::npos;
    ASSERT_TRUE(bytes.find(Environment("VXCORE_WEBDAV_TEST_PASSWORD")) == std::string::npos);
  }
  ASSERT_TRUE(journaled);
  Control({{"action", "clear_faults"}});
  client.backend.reset();
  ASSERT_EQ(client.Attach(), VXCORE_OK);
  for (const auto &scratch : owned) ASSERT_FALSE(Inspect(scratch).value("exists", false));
  ASSERT_EQ(*RemoteBytes(".vnote-webdav-tmp-other-client"),
            "never garbage collect another client's scratch");
  ASSERT_FALSE(RemoteBytes("unpublished.md").has_value());
  ASSERT_EQ(client.Sync(), VXCORE_OK);
  ASSERT_TRUE(CommonFile(client, "unpublished.md"));
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  vxcore_set_test_mode(1);
  if (sodium_init() < 0 || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    std::cerr << "WebDAV test runtime initialization failed\n";
    return 1;
  }
  try {
    for (const auto *name : {"VXCORE_WEBDAV_TEST_URL", "VXCORE_WEBDAV_TEST_USERNAME",
                             "VXCORE_WEBDAV_TEST_PASSWORD", "VXCORE_WEBDAV_TEST_CONTROL_URL",
                             "VXCORE_WEBDAV_TEST_CONTROL_TOKEN", "VXCORE_WEBDAV_TEST_CLIENT_ROOT",
                             "VXCORE_WEBDAV_TEST_RUNNER", "VXCORE_WEBDAV_TEST_PYTHON"}) {
      if (Environment(name).empty())
        throw std::runtime_error("Run test_webdav_sync through run_webdav_test.py");
    }
    executable = PathToUtf8(fs::absolute(PathFromUtf8(argv[0])));
    if (argc == 5 && std::string(argv[1]) == "--child")
      return DispatchChild(argv[2], PathFromUtf8(argv[3]), PathFromUtf8(argv[4]));
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--case"))
      throw std::runtime_error("Unknown WebDAV test arguments");
    const std::string selected = argc == 3 ? argv[2] : "";
    struct TestCase {
      const char *name;
      int (*run)();
    };
    const TestCase cases[] = {
        {"bootstrap", TestBootstrapOwnershipAndProjection},
        {"probes", TestBootstrapRaceAndConditionalProbes},
        {"setup-diagnostics", TestSetupFailureDiagnostics},
        {"probe-recovery", TestFailedProbeCleanupIsRecoverable},
        {"exclusions", TestExcludedAndUnsupportedLocalNames},
        {"complete-scan", TestCompleteScanRequiredForDeletions},
        {"conflict-matrix", TestConflictMatrixAndDurableChoices},
        {"stale-choice", TestStaleChoicesAndSameBytesEtag},
        {"metadata-ciphertext", TestMetadataAndCiphertextConflictPolicy},
        {"type-collision", TestTypeCollisionsAndRetainedCollections},
        {"routing-projection", TestIncomingConfigRestoresRouting},
        {"apply-preflight", TestDeferredApplyProtectsWholeCohort},
        {"conditional-race", TestConditionalRacesPreserveThirdPartyVersion},
        {"cancellation", TestCancellationAndConcurrentSnapshots},
        {"credentials-progress", TestProviderRotationAndCompositeProgress},
        {"two-device", TestTwoDeviceCloneAndPublication},
        {"clone-stability", TestCloneRevalidatesAndCancellation},
        {"lost-ack", TestLostAcknowledgementsAreNotReplayed},
        {"process-crashes", TestProcessCrashJournalBoundaries},
        {"local-crash", TestCrashAfterLocalReplacementBeforeBaseline},
        {"divergent-recovery", TestRecoveryPreservesDivergentVersion},
        {"damaged-state", TestDamagedStateAndJournalFailClosed},
        {"capi-apply", TestCapiDeferredApplyAndErrorOutputs},
        {"capi-cancel", TestCapiCloneCancellationClearsOutput},
        {"truncated-body", TestUnstableRemoteDownloadNeverPublishesPartialBytes},
        {"invalid-metadata", TestMetadataValidationPrecedesPublication},
        {"stale-deletion", TestIncomingDeletionChecksLocalFingerprint},
    };
    bool found = false;
    for (const auto &test : cases) {
      if (!selected.empty() && selected != test.name) continue;
      found = true;
      std::cout << "WebDAV sync: " << test.name << std::endl;
      RUN_TEST(test.run);
    }
    if (!found) throw std::runtime_error("Unknown WebDAV test case");
    curl_global_cleanup();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "WebDAV sync test failed: " << error.what() << '\n';
    return 1;
  }
}
