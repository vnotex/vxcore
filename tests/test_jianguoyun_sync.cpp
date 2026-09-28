// Managed-store regressions. Run through run_webdav_test.py --profile jianguoyun.
// All remote mutations below use the authenticated TLS fixture or production backend.
#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/notebook.h"
#include "sync/credential_provider.h"
#include "sync/jianguoyun/jianguoyun_state.h"
#include "sync/jianguoyun/jianguoyun_sync_backend.h"
#include "sync/sync_json_keys.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"
#include "vxcore/vxcore.h"

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace {
using namespace vxcore;
namespace fs = std::filesystem;
namespace managed = vxcore::jianguoyun;
namespace key = vxcore::jianguoyun::key;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr const char *kConfigPath = managed::kConfigPath;
const std::string kStatePath = std::string(managed::kPrivatePath) + "/state.json";
const std::string kPendingPath = std::string(managed::kPrivatePath) + "/pending.json";
std::string executable;

std::string Environment(const char *name) {
  const auto *value = std::getenv(name);
  return value ? value : "";
}

std::string Identity() {
  std::string result;
  if (NotebookEncryption::GenerateIdentity(result) != VXCORE_OK)
    throw std::runtime_error("Could not generate fixture identity");
  return result;
}

std::string Hash(const std::string &bytes) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char *>(bytes.data()),
                     bytes.size());
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  return hex.data();
}

std::string HashFile(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Could not open streamed fixture file");
  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);
  std::array<char, 65536> buffer{};
  while (stream) {
    stream.read(buffer.data(), buffer.size());
    crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char *>(buffer.data()),
                              static_cast<unsigned long long>(stream.gcount()));
  }
  if (!stream.eof()) throw std::runtime_error("Could not hash streamed fixture file");
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  crypto_hash_sha256_final(&state, digest.data());
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  return hex.data();
}

void WritePattern(const fs::path &path, uint64_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  std::array<char, 65536> buffer{};
  for (size_t i = 0; i < buffer.size(); ++i)
    buffer[i] = static_cast<char>((i * 31 + i / 251) % 256);
  while (size) {
    const auto count = static_cast<size_t>(std::min<uint64_t>(size, buffer.size()));
    stream.write(buffer.data(), count);
    size -= count;
  }
  stream.close();
  if (!stream) throw std::runtime_error("Could not write deterministic streamed fixture");
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
    throw std::runtime_error("Invalid fixture base64");
  out.resize(length);
  return out;
}

std::string ReadBytes(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Could not read owned fixture file");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void WriteBytes(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  if (WriteFileAtomic(path, bytes) != VXCORE_OK)
    throw std::runtime_error("Fixture atomic write failed");
}
Json ReadJson(const fs::path &path) { return Json::parse(ReadBytes(path)); }

size_t ControlWrite(char *bytes, size_t size, size_t count, void *userdata) {
  auto &out = *static_cast<std::string *>(userdata);
  const auto length = size * count;
  if (length > 8 * 1024 * 1024 - out.size()) return 0;
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
  curl_easy_setopt(handle, CURLOPT_CAINFO, ca.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, ControlWrite);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  const auto code = curl_easy_perform(handle);
  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  const auto result = Json::parse(response, nullptr, false);
  if (code != CURLE_OK || status != 200 || !result.is_object() || !result.value("ok", false))
    throw std::runtime_error("Authenticated fixture control failed (details redacted)");
  return result;
}
void PutRemote(const std::string &path, const std::string &bytes) {
  Control(
      {{"action", "put"}, {"path", path}, {"content_base64", Encode(bytes)}, {"parents", true}});
}
Json Inspect(const std::string &path, bool content = false) {
  return Control({{"action", "inspect"}, {"path", path}, {"content", content}});
}
std::string RemoteBytes(const std::string &path) {
  return Decode(Inspect(path, true).at("content_base64").get<std::string>());
}
Json Head() { return Json::parse(RemoteBytes(managed::kHeadPath)); }
Json Manifest() {
  return Json::parse(
      RemoteBytes(managed::CommitPath(Head().at(key::kCommitHash).get<std::string>())));
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
Json RequestsSince(uint64_t cursor) {
  const auto page = Control({{"action", "requests"}, {"since", cursor}, {"limit", 10000}});
  if (page.at("oldest").get<uint64_t>() > cursor + 1)
    throw std::runtime_error("Fixture request history overflow; cannot prove protocol invariant");
  return page.at("requests");
}
bool NoUnsafeRequests(uint64_t cursor, bool no_inventory = true) {
  for (const auto &request : RequestsSince(cursor)) {
    const auto method = request.at("method").get<std::string>();
    const auto path = request.at("path").get<std::string>();
    if (method == "DELETE" || method == "LOCK") return false;
    if (method == "PUT" &&
        (path.find(".vnote-sync/objects/") == 0 || path.find(".vnote-sync/commits/") == 0))
      return false;
    if (no_inventory && method == "PROPFIND" && request.value("depth", "") != "0") return false;
  }
  return true;
}
bool WaitBarrier(const std::string &id) {
  return Control({{"action", "wait"}, {"id", id}, {"timeout_ms", 15000}}).value("reached", false);
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
  config.backend = "jianguoyun";
  config.remote_url = Environment("VXCORE_WEBDAV_TEST_URL");
  config.auto_sync_enabled = false;
  return config;
}
std::shared_ptr<ICredentialProvider> Provider() {
  SyncCredentials credentials;
  credentials.extra = {{kJsonKeyUsername, Environment("VXCORE_WEBDAV_TEST_USERNAME")},
                       {kJsonKeyPassword, Environment("VXCORE_WEBDAV_TEST_PASSWORD")}};
  return std::make_shared<InMemoryCredentialProvider>(credentials);
}
struct Workspace {
  fs::path path = PathFromUtf8(Environment("VXCORE_WEBDAV_TEST_CLIENT_ROOT")) / Identity();
  Workspace() { fs::create_directories(path); }
  ~Workspace() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
struct Client {
  Workspace workspace;
  fs::path root = workspace.path / "notebook";
  std::string id;
  SyncConfig config = Configuration();
  std::unique_ptr<JianguoyunSyncBackend> backend;
  explicit Client(bool create = true) {
    fs::create_directories(root);
    if (!create) return;
    VxCoreContextHandle context = nullptr;
    if (vxcore_context_create(nullptr, &context) != VXCORE_OK)
      throw std::runtime_error("Could not create isolated notebook context");
    char *created = nullptr;
    const auto result = vxcore_notebook_create(
        context, PathToUtf8(root).c_str(),
        "{\"name\":\"Managed fixture "
        "notebook\",\"assetsFolder\":\"assets\",\"recycleBinFolder\":\"recycle\"}",
        VXCORE_NOTEBOOK_BUNDLED, &created);
    if (created) {
      id = created;
      vxcore_string_free(created);
    }
    const auto closed =
        result == VXCORE_OK ? vxcore_notebook_close(context, id.c_str()) : VXCORE_OK;
    vxcore_context_destroy(context);
    if (result != VXCORE_OK || closed != VXCORE_OK)
      throw std::runtime_error("Could not create and retire isolated notebook context");
  }
  fs::path Path(const std::string &path) const { return root / PathFromUtf8(path); }
  void Write(const std::string &path, const std::string &bytes) { WriteBytes(Path(path), bytes); }
  std::string Read(const std::string &path) const { return ReadBytes(Path(path)); }
  Json State() const { return ReadJson(Path(kStatePath)); }
  VxCoreError Attach() {
    backend = std::make_unique<JianguoyunSyncBackend>(config, Provider());
    return backend->Initialize(PathToUtf8(root), config);
  }
  VxCoreError Clone() {
    backend = std::make_unique<JianguoyunSyncBackend>(config, Provider());
    auto result = backend->Clone(PathToUtf8(root), config);
    if (result != VXCORE_OK) return result;
    id = ReadJson(Path(kConfigPath)).at("id").get<std::string>();
    return Attach();
  }
  VxCoreError Sync() { return backend->Sync(nullptr, nullptr); }
  void CopyUnbased(const Client &source) {
    fs::copy(source.root, root, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::remove_all(Path(managed::kPrivatePath));
    id = source.id;
  }
};
std::set<std::string> Conflicts(JianguoyunSyncBackend &backend) {
  std::vector<SyncConflictInfo> found;
  if (backend.GetConflicts(found) != VXCORE_OK)
    throw std::runtime_error("Could not inspect conflicts");
  std::set<std::string> result;
  for (const auto &conflict : found) result.insert(conflict.path);
  return result;
}
bool Pending(const Client &client) {
  managed::RecoveryStatus status;
  if (managed::InspectRecovery(client.Path(managed::kPrivatePath), status) != VXCORE_OK)
    throw std::runtime_error("Recovery state failed offline validation");
  return status.any_pending;
}
std::string LogicalBytes(const std::string &path) {
  const auto entry = Manifest().at(key::kEntries).at(path);
  if (entry.at(kJsonKeyKind) != "file" || entry.at(key::kSize).get<uint64_t>() > 1024 * 1024)
    throw std::runtime_error("LogicalBytes is limited to small ordinary files");
  std::string bytes;
  for (const auto &chunk : entry.at(key::kChunks))
    bytes += RemoteBytes(managed::ObjectPath(chunk.at(key::kSha256).get<std::string>()));
  if (Hash(bytes) != entry.at(key::kSha256))
    throw std::runtime_error("Corrupt remote logical file");
  return bytes;
}
void PublishManifest(Json manifest) {
  auto head = Head();
  manifest[kJsonKeyParent] = head.at(key::kCommitHash);
  manifest[key::kGeneration] = head.at(key::kGeneration).get<uint64_t>() + 1;
  manifest[key::kOperationId] = Identity();
  const auto bytes = manifest.dump();
  const auto hash = Hash(bytes);
  PutRemote(managed::CommitPath(hash), bytes);
  head[key::kCommitHash] = hash;
  head[key::kGeneration] = manifest.at(key::kGeneration);
  PutRemote(managed::kHeadPath, head.dump());
}
struct NetworkCall {
  JianguoyunSyncBackend &backend;
  SyncCancellationPtr token = std::make_shared<SyncCancellation>();
  std::future<VxCoreError> result;
  explicit NetworkCall(JianguoyunSyncBackend &value) : backend(value) {
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

int TestBootstrap() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("note.md", "ordinary local bytes\r\n");
  auto local_config = ReadJson(a.Path(kConfigPath));
  local_config[kJsonKeySyncEnabled] = true;
  local_config[kJsonKeySyncBackend] = "jianguoyun";
  local_config[kJsonKeySyncRemoteUrl] = a.config.remote_url;
  local_config[kJsonKeyAutoSyncEnabled] = false;
  a.Write(kConfigPath, local_config.dump());
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_FALSE(Inspect(managed::kHeadPath).value("exists", false));
  ASSERT_FALSE(Inspect("note.md").value("exists", false));
  Client missing_head(false);
  ASSERT_NE(missing_head.Clone(), VXCORE_OK);
  ASSERT_FALSE(Inspect(managed::kHeadPath).value("exists", false));
  const auto cursor = RequestCursor();
  std::vector<SyncFileInfo> status;
  ASSERT_EQ(a.backend->GetStatus(status), VXCORE_OK);
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  ASSERT_EQ(RequestCursor(), cursor);
  ASSERT_FALSE(Inspect(managed::kHeadPath).value("exists", false));
  ASSERT_EQ(a.backend->FetchRebasePush(), VXCORE_OK);
  const auto before_apply = RequestCursor();
  std::vector<std::string> changed;
  ASSERT_EQ(a.backend->ApplySync({}, changed), VXCORE_OK);
  ASSERT_EQ(RequestCursor(), before_apply);
  ASSERT_EQ(LogicalBytes("note.md"), a.Read("note.md"));
  const auto remote_config = Json::parse(LogicalBytes(kConfigPath));
  for (const auto *field :
       {kJsonKeySyncEnabled, kJsonKeySyncBackend, kJsonKeySyncRemoteUrl, kJsonKeyAutoSyncEnabled})
    ASSERT_FALSE(remote_config.contains(field));
  ASSERT_EQ(ReadJson(a.Path(kConfigPath)), local_config);
  ASSERT_TRUE(NoUnsafeRequests(cursor));
  Client stranger;
  const auto head = RemoteBytes(managed::kHeadPath);
  ASSERT_NE(stranger.Attach(), VXCORE_OK);
  ASSERT_EQ(RemoteBytes(managed::kHeadPath), head);
  Control({{"action", "reset"}});
  PutRemote(kConfigPath, local_config.dump());
  PutRemote("ordinary.md", "an existing ordinary WebDAV notebook");
  Client ordinary;
  ASSERT_NE(ordinary.Attach(), VXCORE_OK);
  ASSERT_FALSE(Inspect(managed::kHeadPath).value("exists", false));
  ASSERT_EQ(RemoteBytes("ordinary.md"), "an existing ordinary WebDAV notebook");
  return 0;
}

int BootstrapRace(bool same_notebook) {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a, b(!same_notebook);
  if (same_notebook) b.CopyUnbased(a);
  a.Write("a.md", "first device");
  b.Write("b.md", "second device");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(b.Attach(), VXCORE_OK);
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  Control({{"action", "barrier"},
           {"id", "genesis"},
           {"method", "MOVE"},
           {"destination", managed::kHeadPath},
           {"phase", "before"}});
  NetworkCall first(*a.backend);
  ASSERT_TRUE(WaitBarrier("genesis"));
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  const auto winner = Head();
  Control({{"action", "release"}, {"id", "genesis"}});
  const auto result = first.result.get();
  if (same_notebook) {
    ASSERT_TRUE(result == VXCORE_OK || result == VXCORE_ERR_SYNC_CONFLICT);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Read("b.md"), "second device");
    ASSERT_EQ(b.Read("a.md"), "first device");
    ASSERT_EQ(Head().at(key::kRepositoryId), winner.at(key::kRepositoryId));
  } else {
    ASSERT_NE(result, VXCORE_OK);
    ASSERT_EQ(Head(), winner);
    ASSERT_EQ(a.Read("a.md"), "first device");
    ASSERT_FALSE(Manifest().at(key::kEntries).contains("a.md"));
  }
  return 0;
}
int TestBootstrapSame() { return BootstrapRace(true); }
int TestBootstrapDifferent() { return BootstrapRace(false); }

int TestTwoDevice() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("note.md", "device A");
  a.Write("delete.md", "delete after clone");
  a.Write("assets/comments/note.json", "{\"comments\":[\"sidecar\"]}");
  a.Write("nested/empty.bin", "");
  fs::create_directories(a.Path("nested/empty-folder"));
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  ASSERT_EQ(b.Read("note.md"), "device A");
  ASSERT_EQ(b.Read("nested/empty.bin"), "");
  ASSERT_TRUE(fs::is_directory(b.Path("nested/empty-folder")));
  ASSERT_EQ(b.Read("assets/comments/note.json"), a.Read("assets/comments/note.json"));
  b.Write("note.md", "device B\r\n");
  fs::remove(b.Path("delete.md"));
  const auto cursor = RequestCursor();
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("note.md"), "device B\r\n");
  ASSERT_FALSE(fs::exists(a.Path("delete.md")));
  ASSERT_EQ(Manifest().at(key::kEntries).at("delete.md").at(kJsonKeyKind), "tombstone");
  ASSERT_TRUE(NoUnsafeRequests(cursor));
  return 0;
}

int TestUnbasedTombstone() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("offline.md", "offline survivor");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  b.CopyUnbased(a);
  fs::remove(a.Path("offline.md"));
  a.Write("remote-only.md", "remote union member");
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  b.Write("local-only.md", "local union member");
  ASSERT_EQ(b.Attach(), VXCORE_OK);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(b.Read("offline.md"), "offline survivor");
  ASSERT_EQ(b.Read("remote-only.md"), "remote union member");
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("offline.md"), "offline survivor");
  ASSERT_EQ(a.Read("local-only.md"), "local union member");
  return 0;
}

int TestConflictRestart() {
  for (const auto choice : {SyncConflictResolution::kKeepLocal, SyncConflictResolution::kKeepRemote,
                            SyncConflictResolution::kKeepBoth}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("same.bin", std::string("base\0bytes", 10));
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    Client b(false);
    ASSERT_EQ(b.Clone(), VXCORE_OK);
    a.Write("same.bin", "local whole revision");
    b.Write("same.bin", "remote whole revision");
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_ERR_SYNC_CONFLICT);
    ASSERT_TRUE(Conflicts(*a.backend).count("same.bin"));
    ASSERT_EQ(a.backend->ResolveConflict("same.bin", choice), VXCORE_OK);
    a.backend.reset();
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    ASSERT_TRUE(Conflicts(*a.backend).empty());
    ASSERT_EQ(a.Read("same.bin"), b.Read("same.bin"));
    std::set<std::string> revisions;
    for (const auto &file : fs::directory_iterator(a.root))
      if (file.is_regular_file()) revisions.insert(ReadBytes(file.path()));
    if (choice == SyncConflictResolution::kKeepBoth) {
      ASSERT_TRUE(revisions.count("local whole revision"));
      ASSERT_TRUE(revisions.count("remote whole revision"));
    } else {
      ASSERT_EQ(a.Read("same.bin"), choice == SyncConflictResolution::kKeepLocal
                                        ? "local whole revision"
                                        : "remote whole revision");
    }
  }
  return 0;
}

int TestStaleChoice() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("same.md", "base");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  a.Write("same.md", "local");
  b.Write("same.md", "remote first");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(a.backend->ResolveConflict("same.md", SyncConflictResolution::kKeepLocal), VXCORE_OK);
  b.Write("same.md", "remote successor");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  a.backend.reset();
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(a.Read("same.md"), "local");
  ASSERT_EQ(LogicalBytes("same.md"), "remote successor");
  ASSERT_TRUE(Conflicts(*a.backend).count("same.md"));
  return 0;
}

int TestDisjoint() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("a.md", "base A");
  a.Write("b.md", "base B");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  a.Write("a.md", "edited A");
  b.Write("b.md", "edited B");
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("b.md"), "edited B");
  ASSERT_EQ(b.Read("a.md"), "edited A");
  ASSERT_TRUE(Conflicts(*a.backend).empty());
  ASSERT_TRUE(Conflicts(*b.backend).empty());
  return 0;
}

struct EncryptedRevision {
  std::string envelope, ciphertext;
};
EncryptedRevision Encrypted(const std::string &notebook_id, const std::string &document_id,
                            const std::string &plaintext,
                            const EncryptedRevision *cohort = nullptr) {
  Workspace staging;
  NotebookEncryption::KeyEnvelope envelope;
  NotebookEncryption::Key master, notebook, note;
  NotebookEncryption::ObjectHeader header;
  const std::string passphrase = "disposable fixture encryption passphrase";
  auto require = [](VxCoreError result) {
    if (result != VXCORE_OK)
      throw std::runtime_error("Could not construct authenticated ciphertext");
  };
  if (cohort) {
    require(NotebookEncryption::DecodeKeyEnvelope(cohort->envelope.data(), cohort->envelope.size(),
                                                  envelope));
    require(NotebookEncryption::UnlockKeys(envelope, notebook_id, passphrase.data(),
                                           passphrase.size(), master, notebook));
  } else {
    require(NotebookEncryption::PrepareNewKeys(notebook_id, passphrase.data(), passphrase.size(),
                                               envelope, master, notebook));
  }
  require(NotebookEncryption::GenerateKey(note));
  const std::vector<uint8_t> body(plaintext.begin(), plaintext.end());
  require(NotebookEncryption::WriteNoteSnapshot(staging.path / "secret.vne", envelope, notebook,
                                                note, document_id, body,
                                                Json{{"editorType", "markdown"}}, &header));
  EncryptedRevision result;
  require(NotebookEncryption::EncodeKeyEnvelope(envelope, result.envelope));
  result.ciphertext = ReadBytes(staging.path / "secret.vne");
  return result;
}
int TestMetadataCiphertext() {
  Control({{"action", "reset"}});
  Client a;
  const auto document = Identity();
  const auto base = Encrypted(a.id, document, "private baseline");
  const auto local = Encrypted(a.id, document, "private local");
  const auto remote = Encrypted(a.id, document, "private remote");
  a.Write("secret.vne", base.ciphertext);
  a.Write(managed::kEncryptionPath, base.envelope);
  auto config = ReadJson(a.Path(kConfigPath));
  config[kJsonKeyEncryptionInitialized] = true;
  a.Write(kConfigPath, config.dump());
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  for (const auto *path : {kConfigPath, "vx_notebook/contents/vx.json"}) {
    auto first = Json::parse(a.Read(path)), second = first;
    first["metadata"]["side"] = "local";
    second["metadata"]["side"] = "remote";
    a.Write(path, first.dump());
    b.Write(path, second.dump());
  }
  a.Write("secret.vne", local.ciphertext);
  b.Write("secret.vne", remote.ciphertext);
  a.Write(managed::kEncryptionPath, local.envelope);
  b.Write(managed::kEncryptionPath, remote.envelope);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  std::vector<SyncConflictInfo> conflicts;
  ASSERT_EQ(a.backend->GetConflicts(conflicts), VXCORE_OK);
  std::set<std::string> paths;
  for (const auto &conflict : conflicts) {
    paths.insert(conflict.path);
    ASSERT_FALSE(conflict.can_keep_both);
    ASSERT_EQ(a.backend->ResolveConflict(conflict.path, SyncConflictResolution::kKeepBoth),
              VXCORE_ERR_UNSUPPORTED);
  }
  ASSERT_TRUE(paths.count(kConfigPath));
  ASSERT_TRUE(paths.count("vx_notebook/contents/vx.json"));
  ASSERT_TRUE(paths.count("secret.vne"));
  ASSERT_TRUE(paths.count(managed::kEncryptionPath));
  ASSERT_EQ(a.Read("secret.vne"), local.ciphertext);
  ASSERT_EQ(LogicalBytes("secret.vne"), remote.ciphertext);
  managed::RecoveryStatus recovery;
  ASSERT_EQ(managed::InspectRecovery(a.Path(managed::kPrivatePath), recovery), VXCORE_OK);
  ASSERT_TRUE(recovery.any_conflict);
  ASSERT_TRUE(recovery.encryption_conflict);
  return 0;
}

int TestEncryptedNoteDoesNotBlockKeyCohort() {
  Control({{"action", "reset"}});
  Client a;
  const auto document = Identity();
  const auto base = Encrypted(a.id, document, "baseline note");
  a.Write("secret.vne", base.ciphertext);
  a.Write(managed::kEncryptionPath, base.envelope);
  auto config = ReadJson(a.Path(kConfigPath));
  config[kJsonKeyEncryptionInitialized] = true;
  a.Write(kConfigPath, config.dump());
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  a.Write("secret.vne", Encrypted(a.id, document, "local note", &base).ciphertext);
  b.Write("secret.vne", Encrypted(a.id, document, "remote note", &base).ciphertext);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_ERR_SYNC_CONFLICT);
  managed::RecoveryStatus recovery;
  ASSERT_EQ(managed::InspectRecovery(a.Path(managed::kPrivatePath), recovery), VXCORE_OK);
  ASSERT_EQ(a.Read(managed::kEncryptionPath), base.envelope);
  ASSERT_EQ(LogicalBytes(managed::kEncryptionPath), base.envelope);
  ASSERT_TRUE(recovery.any_conflict);
  ASSERT_FALSE(recovery.encryption_conflict);
  ASSERT_FALSE(recovery.encryption_pending);
  ASSERT_FALSE(recovery.replaces_encryption);
  ASSERT_EQ(a.backend->ResolveConflict("secret.vne", SyncConflictResolution::kKeepBoth),
            VXCORE_ERR_UNSUPPORTED);
  return 0;
}

int TestImmutableStaging() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("original.md", "already published immutable content");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  const auto original = RemoteTree();
  const std::string shared = "same new object uploaded by competing devices";
  a.Write("new-a.bin", shared);
  b.Write("new-b.bin", shared);
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  Control({{"action", "barrier"},
           {"id", "staged"},
           {"method", "PUT"},
           {"path_prefix", ".vnote-sync/staging/"},
           {"phase", "after"}});
  const auto cursor = RequestCursor();
  NetworkCall first(*a.backend);
  ASSERT_TRUE(WaitBarrier("staged"));
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  const auto object = managed::ObjectPath(Hash(shared));
  ASSERT_EQ(RemoteBytes(object), shared);
  const auto winner_tag = Inspect(object).at("etag");
  Control({{"action", "release"}, {"id", "staged"}});
  const auto result = first.result.get();
  ASSERT_TRUE(result == VXCORE_OK || result == VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("new-b.bin"), shared);
  ASSERT_EQ(b.Read("new-a.bin"), shared);
  ASSERT_EQ(Inspect(object).at("etag"), winner_tag);
  for (const auto &item : original.items()) {
    if (item.key().find(".vnote-sync/objects/") == 0 && item.value().at("kind") == "file")
      ASSERT_EQ(Inspect(item.key()).at("sha256"), item.value().at("sha256"));
  }
  ASSERT_TRUE(NoUnsafeRequests(cursor));
  return 0;
}

int TestCorruptObjects() {
  for (const bool missing : {false, true}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("kept.md", "trusted local baseline");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    Client b(false);
    ASSERT_EQ(b.Clone(), VXCORE_OK);
    b.Write("kept.md", "incoming remote revision");
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    const auto object = managed::ObjectPath(Hash("incoming remote revision"));
    if (missing)
      Control({{"action", "delete"}, {"path", object}});
    else
      PutRemote(object, "corruption of immutable object");
    const auto baseline = a.Read(kStatePath);
    const auto head = Head();
    ASSERT_NE(a.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Read("kept.md"), "trusted local baseline");
    ASSERT_EQ(a.Read(kStatePath), baseline);
    ASSERT_EQ(Head(), head);
    Client clone(false);
    ASSERT_NE(clone.Clone(), VXCORE_OK);
    ASSERT_FALSE(fs::exists(clone.Path("kept.md")));
  }
  Control({{"action", "reset"}});
  Client a;
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  const std::string new_bytes = "never overwrite an occupied corrupt canonical name";
  const auto canonical = managed::ObjectPath(Hash(new_bytes));
  PutRemote(canonical, "wrong bytes at claimed hash");
  a.Write("new.bin", new_bytes);
  const auto head = Head();
  ASSERT_NE(a.Sync(), VXCORE_OK);
  ASSERT_EQ(RemoteBytes(canonical), "wrong bytes at claimed hash");
  ASSERT_EQ(Head(), head);
  return 0;
}

int StreamedRoundTrip(uint64_t size) {
  Control({{"action", "reset"}});
  Client a;
  WritePattern(a.Path("assets/large.bin"), size);
  const auto expected = HashFile(a.Path("assets/large.bin"));
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  const auto entry = Manifest().at(key::kEntries).at("assets/large.bin");
  ASSERT_EQ(entry.at(key::kSize).get<uint64_t>(), size);
  ASSERT_EQ(entry.at(key::kSha256), expected);
  uint64_t total = 0;
  for (const auto &chunk : entry.at(key::kChunks)) {
    const auto bytes = chunk.at(key::kSize).get<uint64_t>();
    ASSERT_TRUE(bytes > 0 && bytes <= managed::kChunkBytes);
    total += bytes;
    const auto metadata = Inspect(managed::ObjectPath(chunk.at(key::kSha256).get<std::string>()));
    ASSERT_EQ(metadata.at("size").get<uint64_t>(), bytes);
    ASSERT_EQ(metadata.at("sha256"), chunk.at(key::kSha256));
  }
  ASSERT_EQ(total, size);
  ASSERT_TRUE(entry.at(key::kChunks).size() > 1);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  ASSERT_EQ(fs::file_size(b.Path("assets/large.bin")), size);
  ASSERT_EQ(HashFile(b.Path("assets/large.bin")), expected);
  return 0;
}
int TestChunkBoundary() { return StreamedRoundTrip(managed::kChunkBytes + 65539); }
int TestLargeLogicalFile() { return StreamedRoundTrip(501ULL * 1024 * 1024 + 13); }

int TestManifestBeyondListingLimit() {
  Control({{"action", "reset"}});
  Client a;
  for (int i = 0; i < 801; ++i)
    a.Write("many/note-" + std::to_string(i) + ".md", "note " + std::to_string(i));
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_TRUE(Manifest().at(key::kEntries).size() > 750);
  // A manifest clone must still work if every collection-inventory attempt fails.
  FaultScope cleanup;
  Control({{"action", "fault"},
           {"method", "PROPFIND"},
           {"depth", "1"},
           {"effect", "status"},
           {"status", 403},
           {"count", -1}});
  const auto cursor = RequestCursor();
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  for (int i = 0; i < 801; ++i)
    ASSERT_EQ(b.Read("many/note-" + std::to_string(i) + ".md"), "note " + std::to_string(i));
  b.Write("many/note-800.md", "edited beyond listing boundary");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("many/note-800.md"), "edited beyond listing boundary");
  ASSERT_EQ(a.Read("many/note-0.md"), "note 0");
  ASSERT_EQ(Manifest().at(key::kEntries).at("many/note-0.md").at(kJsonKeyKind), "file");
  ASSERT_TRUE(NoUnsafeRequests(cursor));
  return 0;
}

int TestRawCasConcurrency() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("a.md", "base a");
  a.Write("b.md", "base b");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  a.Write("a.md", "CAS device a");
  b.Write("b.md", "CAS device b");
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  Control({{"action", "barrier"},
           {"id", "cas"},
           {"method", "PUT"},
           {"path", managed::kHeadPath},
           {"phase", "before"}});
  const auto cursor = RequestCursor();
  NetworkCall first(*a.backend);
  ASSERT_TRUE(WaitBarrier("cas"));
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  Control({{"action", "release"}, {"id", "cas"}});
  const auto result = first.result.get();
  ASSERT_TRUE(result == VXCORE_OK || result == VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("b.md"), "CAS device b");
  ASSERT_EQ(b.Read("a.md"), "CAS device a");
  bool rejected_stale = false;
  for (const auto &request : RequestsSince(cursor))
    if (request.at("method") == "PUT" && request.at("path") == managed::kHeadPath &&
        request.at("status") == 412)
      rejected_stale = true;
  ASSERT_TRUE(rejected_stale);
  ASSERT_TRUE(NoUnsafeRequests(cursor));
  return 0;
}

int TestLostAcknowledgement() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("ack.md", "old");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  a.Write("ack.md", "published once despite lost response");
  Control({{"action", "fault"},
           {"method", "PUT"},
           {"path", managed::kHeadPath},
           {"phase", "after"},
           {"effect", "drop"}});
  const auto result = a.Sync();
  ASSERT_TRUE(result == VXCORE_OK || result == VXCORE_ERR_SYNC_NETWORK);
  ASSERT_EQ(LogicalBytes("ack.md"), "published once despite lost response");
  const auto published = Head();
  Control({{"action", "clear_faults"}});
  a.backend.reset();
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(Head(), published);
  ASSERT_EQ(a.Read("ack.md"), "published once despite lost response");
  ASSERT_FALSE(Pending(a));
  return 0;
}

int TestDescendantAcknowledgement() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("a.md", "base a");
  a.Write("b.md", "base b");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  a.Write("a.md", "ancestor publication");
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  Control({{"action", "barrier"},
           {"id", "ack"},
           {"method", "PUT"},
           {"path", managed::kHeadPath},
           {"phase", "after"},
           {"drop_after_release", true}});
  NetworkCall first(*a.backend);
  const bool reached = WaitBarrier("ack");
  if (!reached) {
    first.token->Cancel();
    first.result.get();
    throw std::runtime_error("Publication acknowledgement boundary was not reached");
  }
  const auto intended = Head();
  b.Write("b.md", "successor publication");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  const auto descendant = Head();
  ASSERT_NE(descendant.at(key::kCommitHash), intended.at(key::kCommitHash));
  Control({{"action", "release"}, {"id", "ack"}});
  const auto result = first.result.get();
  ASSERT_TRUE(result == VXCORE_OK || result == VXCORE_ERR_SYNC_NETWORK ||
              result == VXCORE_ERR_SYNC_CONFLICT);
  Control({{"action", "clear_faults"}});
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("b.md"), "successor publication");
  ASSERT_EQ(a.Read("a.md"), "ancestor publication");
  ASSERT_EQ(Head(), descendant);
  ASSERT_EQ(a.State().at("head").at(key::kCommitHash), descendant.at(key::kCommitHash));
  return 0;
}

int TestCancelResume() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("old.md", "baseline");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  for (int i = 0; i < 12; ++i)
    a.Write("upload/" + std::to_string(i) + ".bin", "snapshot " + std::to_string(i));
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  Control({{"action", "barrier"},
           {"id", "cancel"},
           {"method", "PUT"},
           {"path_prefix", ".vnote-sync/staging/"},
           {"phase", "after"},
           {"skip", 3}});
  const auto before = Head();
  {
    NetworkCall operation(*a.backend);
    ASSERT_TRUE(WaitBarrier("cancel"));
    operation.token->Cancel();
    Control({{"action", "release"}, {"id", "cancel"}});
    ASSERT_EQ(operation.result.get(), VXCORE_ERR_CANCELLED);
  }
  ASSERT_TRUE(Pending(a));
  ASSERT_EQ(Head(), before);
  Control({{"action", "clear_faults"}});
  a.backend.reset();
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  for (int i = 0; i < 12; ++i)
    ASSERT_EQ(b.Read("upload/" + std::to_string(i) + ".bin"), "snapshot " + std::to_string(i));
  ASSERT_FALSE(Pending(a));
  return 0;
}

int TestQuotaResume() {
  for (const int status : {429, 507}) {
    Control({{"action", "reset"}});
    FaultScope cleanup;
    Client a;
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    for (int i = 0; i < 48; ++i)
      a.Write("quota/" + std::to_string(i) + ".md", "unique quota payload " + std::to_string(i));
    int failures = 0;
    bool completed = false;
    std::set<std::string> verified;
    for (int window = 0; window < 12 && !completed; ++window) {
      Control({{"action", "clear_faults"}});
      if (window) {
        a.backend.reset();
        ASSERT_EQ(a.Attach(), VXCORE_OK);
      }
      Control({{"action", "fault"},
               {"effect", "status"},
               {"status", status},
               {"skip", 64},
               {"count", -1},
               {"headers", {{"Retry-After", "1"}}}});
      const auto result = a.Sync();
      completed = result == VXCORE_OK;
      if (!completed) {
        ++failures;
        ASSERT_TRUE(Pending(a));
      }
      for (const auto &path : verified) ASSERT_TRUE(Inspect(path).value("exists", false));
      const auto tree = RemoteTree();
      for (const auto &item : tree.items())
        if (item.key().find(".vnote-sync/objects/") == 0 && item.value().at("kind") == "file")
          verified.insert(item.key());
    }
    ASSERT_TRUE(completed);
    ASSERT_TRUE(failures >= 2);
    Control({{"action", "clear_faults"}});
    Client b(false);
    ASSERT_EQ(b.Clone(), VXCORE_OK);
    for (int i = 0; i < 48; ++i)
      ASSERT_EQ(b.Read("quota/" + std::to_string(i) + ".md"),
                "unique quota payload " + std::to_string(i));
    ASSERT_FALSE(Pending(a));
  }
  return 0;
}

// Real child process ownership matches the strict fixture's isolated-runner pattern.
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
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
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
      throw std::runtime_error("Could not launch isolated client");
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
    if (process < 0) throw std::runtime_error("Could not launch isolated client");
#endif
    running = true;
  }
  bool WaitFile(const char *name) const {
    const auto deadline = Clock::now() + std::chrono::seconds(90);
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
    if (WaitForSingleObject(process.hProcess, 300000) != WAIT_OBJECT_0)
      throw std::runtime_error("Isolated client timed out");
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    process.hProcess = nullptr;
    running = false;
    return static_cast<int>(result);
#else
    const auto deadline = Clock::now() + std::chrono::seconds(300);
    while (Clock::now() < deadline) {
      int status = 0;
      if (waitpid(process, &status, WNOHANG) == process) {
        running = false;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Isolated client timed out");
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
  WriteBytes(control / "ready.json", Json{{"pid", pid}}.dump());
  const auto deadline = Clock::now() + std::chrono::seconds(90);
  while (Clock::now() < deadline) {
    if (fs::is_regular_file(control / "go")) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
int DispatchChild(const std::string &mode, const fs::path &root, const fs::path &control) {
  const auto config = Configuration();
  JianguoyunSyncBackend backend(config, Provider());
  if (mode == "clone-edit") {
    ASSERT_TRUE(ChildReady(control));
    ASSERT_EQ(backend.Clone(PathToUtf8(root), config), VXCORE_OK);
    ASSERT_EQ(ReadBytes(root / "shared.md"), "parent publication");
    ASSERT_EQ(backend.Initialize(PathToUtf8(root), config), VXCORE_OK);
    WriteBytes(root / "shared.md", "isolated child publication");
    ASSERT_EQ(backend.Sync(nullptr, nullptr), VXCORE_OK);
    WriteBytes(control / "result.json", Json{{"result", VXCORE_OK}}.dump());
    return 0;
  }
  if (mode != "sync" && mode != "apply-crash") throw std::runtime_error("Unknown child mode");
  ASSERT_EQ(backend.Initialize(PathToUtf8(root), config), VXCORE_OK);
  ASSERT_EQ(backend.StageAndCommit(nullptr), VXCORE_OK);
  if (mode == "apply-crash") ASSERT_EQ(backend.FetchRebasePush(), VXCORE_OK);
  ASSERT_TRUE(ChildReady(control));
  auto result = mode == "apply-crash" ? VXCORE_OK : backend.FetchRebasePush();
  std::vector<std::string> changed;
  if (result == VXCORE_OK) result = backend.ApplySync({}, changed);
  WriteBytes(control / "result.json", Json{{"result", result}, {"changed", changed}}.dump());
  if (mode == "apply-crash")
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_EQ(result, VXCORE_OK);
  return 0;
}

int TestIsolatedClone() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("shared.md", "parent publication");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Workspace destination;
  Child child("clone-edit", destination.path);
  ASSERT_TRUE(child.WaitFile("ready.json"));
  child.Continue();
  ASSERT_EQ(child.Wait(), 0);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("shared.md"), "isolated child publication");
  return 0;
}

int TestProcessCrashes() {
  const Json boundaries[] = {
      {{"method", "PUT"}, {"path_prefix", ".vnote-sync/staging/"}, {"phase", "before"}},
      {{"method", "PUT"}, {"path_prefix", ".vnote-sync/staging/"}, {"phase", "after"}},
      {{"method", "MOVE"}, {"phase", "before"}},
      {{"method", "MOVE"}, {"phase", "after"}},
      {{"method", "PUT"}, {"path", managed::kHeadPath}, {"phase", "after"}},
      {{"method", "MOVE"},
       {"destination", managed::kHeadPath},
       {"phase", "after"},
       {"bootstrap", true}},
  };
  for (auto boundary : boundaries) {
    Control({{"action", "reset"}});
    FaultScope cleanup;
    Client a;
    a.Write("crash.md", "baseline before process death");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    if (!boundary.value("bootstrap", false)) ASSERT_EQ(a.Sync(), VXCORE_OK);
    boundary.erase("bootstrap");
    a.Write("crash.md", "complete revision survives process death");
    a.backend.reset();
    Child crashed("sync", a.root);
    ASSERT_TRUE(crashed.WaitFile("ready.json"));
    boundary["action"] = "barrier";
    boundary["id"] = "crash";
    Control(boundary);
    crashed.Continue();
    ASSERT_TRUE(WaitBarrier("crash"));
    ASSERT_TRUE(Pending(a));
    ASSERT_TRUE(crashed.KillClient());
    ASSERT_NE(crashed.Wait(), 0);
    ASSERT_FALSE(fs::exists(crashed.control.path / "result.json"));
    Control({{"action", "clear_faults"}});
    Child recovered("sync", a.root);
    ASSERT_TRUE(recovered.WaitFile("ready.json"));
    recovered.Continue();
    ASSERT_EQ(recovered.Wait(), 0);
    ASSERT_EQ(LogicalBytes("crash.md"), "complete revision survives process death");
    ASSERT_EQ(a.Read("crash.md"), "complete revision survives process death");
    ASSERT_FALSE(Pending(a));
  }
  return 0;
}

int TestLocalCrash() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("installed.md", "old local revision");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  b.Write("installed.md", "incoming installed revision");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  a.backend.reset();
  Child child("apply-crash", a.root);
  ASSERT_TRUE(child.WaitFile("ready.json"));
  ASSERT_TRUE(Pending(a));
  const auto state = a.Path(kStatePath);
  const auto held = a.workspace.path / "held-state.json";
  fs::rename(state, held);
  fs::create_directory(state);
  WriteBytes(state / "obstruction", "prevent only the final baseline rename");
  child.Continue();
  ASSERT_TRUE(child.WaitFile("result.json"));
  ASSERT_NE(child.Report().at("result").get<int>(), int(VXCORE_OK));
  ASSERT_EQ(a.Read("installed.md"), "incoming installed revision");
  const auto changed = child.Report().at("changed").get<std::vector<std::string>>();
  ASSERT_TRUE(std::find(changed.begin(), changed.end(), "installed.md") != changed.end());
  ASSERT_TRUE(fs::is_regular_file(a.Path(kPendingPath)));
  ASSERT_TRUE(child.KillClient());
  ASSERT_NE(child.Wait(), 0);
  fs::remove(state / "obstruction");
  fs::remove(state);
  fs::rename(held, state);
  Child recovered("sync", a.root);
  ASSERT_TRUE(recovered.WaitFile("ready.json"));
  recovered.Continue();
  ASSERT_EQ(recovered.Wait(), 0);
  ASSERT_EQ(a.Read("installed.md"), "incoming installed revision");
  ASSERT_FALSE(Pending(a));
  ASSERT_EQ(a.State().at("entries").at("installed.md").at(key::kSha256),
            Hash(a.Read("installed.md")));
  return 0;
}

int TestPartialApply() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("a-first.md", "old first");
  a.Write("z-second.md", "old second");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  b.Write("a-first.md", "new first");
  b.Write("z-second.md", "new second");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  ASSERT_EQ(a.backend->FetchRebasePush(), VXCORE_OK);
  const auto state = a.Path(kStatePath), held = a.workspace.path / "held-state.json";
  fs::rename(state, held);
  fs::create_directory(state);
  WriteBytes(state / "obstruction", "fail after first local installation");
  const auto cursor = RequestCursor();
  std::vector<std::string> changed;
  const auto result = a.backend->ApplySync({}, changed);
  fs::remove(state / "obstruction");
  fs::remove(state);
  fs::rename(held, state);
  ASSERT_NE(result, VXCORE_OK);
  ASSERT_EQ(RequestCursor(), cursor);
  ASSERT_EQ(a.Read("a-first.md"), "new first");
  ASSERT_EQ(a.Read("z-second.md"), "old second");
  ASSERT_TRUE(std::find(changed.begin(), changed.end(), "a-first.md") != changed.end());
  ASSERT_TRUE(std::find(changed.begin(), changed.end(), "z-second.md") == changed.end());
  ASSERT_EQ(a.State().at("entries").at("z-second.md").at(key::kSha256), Hash("old second"));
  a.backend.reset();
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("z-second.md"), "new second");
  ASSERT_FALSE(Pending(a));
  return 0;
}

int TestProtectedCohort() {
  for (const bool saved_during_network : {false, true}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("a-first.md", "base first");
    a.Write("z-protected.md", "base protected");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    Client b(false);
    ASSERT_EQ(b.Clone(), VXCORE_OK);
    b.Write("a-first.md", "incoming first");
    b.Write("z-protected.md", "incoming protected");
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
    ASSERT_EQ(a.backend->FetchRebasePush(), VXCORE_OK);
    if (saved_during_network) a.Write("z-protected.md", "editor saved after snapshot");
    const auto baseline = a.State().at("entries");
    const auto cursor = RequestCursor();
    std::vector<std::string> changed{"stale output"};
    ASSERT_EQ(
        a.backend->ApplySync(saved_during_network ? std::vector<std::string>{}
                                                  : std::vector<std::string>{"z-protected.md"},
                             changed),
        VXCORE_ERR_SYNC_CONFLICT);
    ASSERT_TRUE(changed.empty());
    ASSERT_EQ(RequestCursor(), cursor);
    ASSERT_EQ(a.Read("a-first.md"), "base first");
    ASSERT_EQ(a.Read("z-protected.md"),
              saved_during_network ? "editor saved after snapshot" : "base protected");
    ASSERT_EQ(a.State().at("entries"), baseline);
    ASSERT_TRUE(Conflicts(*a.backend).count("z-protected.md"));
    ASSERT_EQ(a.backend->ResolveConflict("z-protected.md", SyncConflictResolution::kKeepLocal),
              VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Read("a-first.md"), "incoming first");
  }
  return 0;
}

int TestDamagedState() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("preserved.md", "trusted local bytes");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  a.backend.reset();
  const auto original = a.Read(kStatePath);
  std::vector<std::string> malformed{"{not JSON"};
  for (const auto &change :
       std::vector<std::pair<std::string, Json>>{{"version", 999},
                                                 {"notebookId", Identity()},
                                                 {"remoteUrl", "https://other.invalid/dav/"},
                                                 {"usernameHash", "not-a-hash"},
                                                 {"repositoryId", Identity()}}) {
    auto state = Json::parse(original);
    state[change.first] = change.second;
    malformed.push_back(state.dump());
  }
  auto unsafe = Json::parse(original);
  unsafe["entries"]["../../escape.md"] = unsafe.at("entries").at("preserved.md");
  malformed.push_back(unsafe.dump());
  for (const auto &bytes : malformed) {
    a.Write(kStatePath, bytes);
    const auto cursor = RequestCursor();
    ASSERT_NE(a.Attach(), VXCORE_OK);
    ASSERT_FALSE(a.backend->IsInitialized());
    ASSERT_EQ(a.Read(kStatePath), bytes);
    ASSERT_EQ(a.Read("preserved.md"), "trusted local bytes");
    ASSERT_EQ(RequestCursor(), cursor);
    a.backend.reset();
  }
  a.Write(kStatePath, original);
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  b.Write("preserved.md", "incoming bytes");
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  ASSERT_EQ(a.backend->StageAndCommit(nullptr), VXCORE_OK);
  ASSERT_EQ(a.backend->FetchRebasePush(), VXCORE_OK);
  ASSERT_TRUE(Pending(a));
  a.backend.reset();
  const auto pending = a.Read(kPendingPath);
  std::vector<std::string> invalid_pending{"{broken journal"};
  for (const auto &change :
       std::vector<std::pair<std::string, Json>>{{"version", 0},
                                                 {"notebookId", Identity()},
                                                 {"remoteUrl", "https://wrong.invalid/"},
                                                 {"usernameHash", Hash("another identity")},
                                                 {"repositoryId", Identity()},
                                                 {"operationId", "../escape"},
                                                 {"phase", "unconditional-overwrite"}}) {
    auto item = Json::parse(pending);
    item[change.first] = change.second;
    invalid_pending.push_back(item.dump());
  }
  Workspace outside;
  WriteBytes(outside.path / "sentinel", "outside notebook must remain untouched");
  for (int defect = 0; defect < 4; ++defect) {
    auto item = Json::parse(pending);
    auto &operation = item.at("apply").begin().value();
    if (defect == 0) operation["desired"]["snapshot"] = "snapshots/../state.json";
    if (defect == 1) operation["backup"] = PathToUtf8(outside.path / "sentinel");
    if (defect == 2) operation["stage"] = "already-applied-without-validation";
    if (defect == 3) {
      const auto copied = operation;
      item["apply"]["../escape"] = copied;
    }
    invalid_pending.push_back(item.dump());
  }
  for (const auto &bytes : invalid_pending) {
    a.Write(kPendingPath, bytes);
    const auto cursor = RequestCursor();
    ASSERT_NE(a.Attach(), VXCORE_OK);
    ASSERT_EQ(RequestCursor(), cursor);
    ASSERT_EQ(a.Read(kPendingPath), bytes);
    ASSERT_EQ(a.Read("preserved.md"), "trusted local bytes");
    ASSERT_EQ(ReadBytes(outside.path / "sentinel"), "outside notebook must remain untouched");
    a.backend.reset();
  }
  a.Write(kPendingPath, pending);
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("preserved.md"), "incoming bytes");
  return 0;
}

int TestInvalidManifest() {
  for (const std::string invalid : {"../escape", "CON.md", "trailing.", "x\\y", "C:/escape"}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("safe.md", "safe local payload");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    auto manifest = Manifest();
    manifest[key::kEntries][invalid] = manifest.at(key::kEntries).at("safe.md");
    PublishManifest(manifest);
    const auto baseline = a.Read(kStatePath);
    ASSERT_NE(a.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Read("safe.md"), "safe local payload");
    ASSERT_EQ(a.Read(kStatePath), baseline);
  }
  for (const int kind : {0, 1, 2, 3}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("safe.md", "safe local payload");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    auto manifest = Manifest();
    if (kind == 0) manifest[key::kFormatVersion] = 999;
    if (kind == 1) manifest[key::kEntries]["safe.md"][key::kSize] = -1;
    if (kind == 2)
      manifest[key::kEntries]["safe.md/child"] = manifest.at(key::kEntries).at("safe.md");
    if (kind == 3)
      manifest[key::kEntries]["safe.md"][key::kChunks][0][key::kSize] = managed::kChunkBytes + 1;
    PublishManifest(manifest);
    const auto baseline = a.Read(kStatePath);
    ASSERT_NE(a.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Read("safe.md"), "safe local payload");
    ASSERT_EQ(a.Read(kStatePath), baseline);
  }
#ifdef _WIN32
  Control({{"action", "reset"}});
  Client a;
  a.Write("case.md", "native original");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  auto manifest = Manifest();
  manifest[key::kEntries]["CASE.md"] = manifest.at(key::kEntries).at("case.md");
  PublishManifest(manifest);
  ASSERT_NE(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("case.md"), "native original");
#endif
  return 0;
}

int TestRollbackAndRecreation() {
  Control({{"action", "reset"}});
  Client a;
  a.Write("kept.md", "old generation");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  const auto old = RemoteBytes(managed::kHeadPath);
  a.Write("kept.md", "new generation");
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  const auto current = RemoteBytes(managed::kHeadPath);
  PutRemote(managed::kHeadPath, old);
  ASSERT_NE(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("kept.md"), "new generation");
  PutRemote(managed::kHeadPath, current);
  auto manifest = Manifest();
  auto head = Head();
  const auto replacement = Identity();
  manifest[key::kRepositoryId] = replacement;
  manifest[kJsonKeyParent] = nullptr;
  manifest[key::kGeneration] = 1;
  manifest[key::kOperationId] = Identity();
  const auto bytes = manifest.dump(), hash = Hash(bytes);
  PutRemote(managed::CommitPath(hash), bytes);
  head[key::kRepositoryId] = replacement;
  head[key::kCommitHash] = hash;
  head[key::kGeneration] = 1;
  PutRemote(managed::kHeadPath, head.dump());
  a.backend.reset();
  ASSERT_NE(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Read("kept.md"), "new generation");
  return 0;
}

int TestSetupBindingRejectsRecreation() {
  Control({{"action", "reset"}});
  Client published;
  published.Write("kept.md", "retained local revision");
  ASSERT_EQ(published.Attach(), VXCORE_OK);
  ASSERT_EQ(published.Sync(), VXCORE_OK);
  Client unbased(false);
  unbased.CopyUnbased(published);
  ASSERT_EQ(unbased.Attach(), VXCORE_OK);

  auto manifest = Manifest();
  auto head = Head();
  const auto replacement = Identity();
  manifest[key::kRepositoryId] = replacement;
  manifest[kJsonKeyParent] = nullptr;
  manifest[key::kGeneration] = 1;
  manifest[key::kOperationId] = Identity();
  const auto bytes = manifest.dump();
  const auto hash = Hash(bytes);
  PutRemote(managed::CommitPath(hash), bytes);
  head[key::kRepositoryId] = replacement;
  head[key::kCommitHash] = hash;
  head[key::kGeneration] = 1;
  PutRemote(managed::kHeadPath, head.dump());
  const auto before = RemoteTree();
  ASSERT_NE(unbased.Sync(), VXCORE_OK);
  ASSERT_EQ(unbased.Read("kept.md"), "retained local revision");
  ASSERT_TRUE(RemoteTree() == before);
  return 0;
}

int TestTypeCollisionsRequireRename() {
  for (bool directory_first : {false, true}) {
    Control({{"action", "reset"}});
    Client a;
    if (directory_first)
      a.Write("collision/child.md", "original child");
    else
      a.Write("collision", "original file");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    const auto before = RemoteTree();
    fs::remove_all(a.Path("collision"));
    if (directory_first)
      a.Write("collision", "replacement file");
    else
      a.Write("collision/child.md", "replacement child");
    ASSERT_EQ(a.Sync(), VXCORE_ERR_UNSUPPORTED);
    ASSERT_TRUE(RemoteTree() == before);
    ASSERT_EQ(a.Read(directory_first ? "collision" : "collision/child.md"),
              directory_first ? "replacement file" : "replacement child");
  }
  return 0;
}

int TestTruncatedObject() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("attachment.bin", "small baseline");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  Client b(false);
  ASSERT_EQ(b.Clone(), VXCORE_OK);
  const std::string incoming(512 * 1024, 'x');
  b.Write("attachment.bin", incoming);
  ASSERT_EQ(b.Sync(), VXCORE_OK);
  Control({{"action", "fault"},
           {"method", "GET"},
           {"path", managed::ObjectPath(Hash(incoming))},
           {"effect", "truncate"},
           {"bytes", 1024}});
  const auto baseline = a.Read(kStatePath);
  ASSERT_NE(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("attachment.bin"), "small baseline");
  ASSERT_EQ(a.Read(kStatePath), baseline);
  Control({{"action", "clear_faults"}});
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("attachment.bin"), incoming);
  return 0;
}

int TestDeleteConflicts() {
  for (const bool local_deletion : {false, true}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("changed.md", "common ancestor");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    Client b(false);
    ASSERT_EQ(b.Clone(), VXCORE_OK);
    if (local_deletion) {
      fs::remove(a.Path("changed.md"));
      b.Write("changed.md", "offline remote edit");
    } else {
      a.Write("changed.md", "offline local edit");
      fs::remove(b.Path("changed.md"));
    }
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_ERR_SYNC_CONFLICT);
    ASSERT_TRUE(Conflicts(*a.backend).count("changed.md"));
    if (local_deletion)
      ASSERT_FALSE(fs::exists(a.Path("changed.md")));
    else
      ASSERT_EQ(a.Read("changed.md"), "offline local edit");
    ASSERT_EQ(a.backend->ResolveConflict("changed.md", local_deletion
                                                           ? SyncConflictResolution::kKeepRemote
                                                           : SyncConflictResolution::kKeepLocal),
              VXCORE_OK);
    a.backend.reset();
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    ASSERT_EQ(b.Sync(), VXCORE_OK);
    const auto expected = local_deletion ? "offline remote edit" : "offline local edit";
    ASSERT_EQ(a.Read("changed.md"), expected);
    ASSERT_EQ(b.Read("changed.md"), expected);
  }
  return 0;
}

int TestStagedRecoveryRechecksBytes() {
  Control({{"action", "reset"}});
  FaultScope cleanup;
  Client a;
  a.Write("preserved.md", "published baseline");
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_EQ(a.Sync(), VXCORE_OK);
  const auto old_head = Head();
  a.Write("preserved.md", "journal-owned intended bytes");
  a.backend.reset();
  Child crashed("sync", a.root);
  ASSERT_TRUE(crashed.WaitFile("ready.json"));
  Control({{"action", "barrier"},
           {"id", "canonical-move"},
           {"method", "MOVE"},
           {"destination", managed::ObjectPath(Hash("journal-owned intended bytes"))},
           {"phase", "before"},
           {"drop_after_release", true}});
  crashed.Continue();
  ASSERT_TRUE(WaitBarrier("canonical-move"));
  ASSERT_TRUE(crashed.KillClient());
  ASSERT_NE(crashed.Wait(), 0);
  const auto pending = ReadJson(a.Path(kPendingPath));
  const auto canonical = managed::ObjectPath(Hash("journal-owned intended bytes"));
  const auto staging = pending.at("uploads").at(canonical).at("staging").get<std::string>();
  ASSERT_EQ(RemoteBytes(staging), "journal-owned intended bytes");
  PutRemote(staging, "interference after staged upload acknowledgement");
  Control({{"action", "clear_faults"}});
  ASSERT_EQ(a.Attach(), VXCORE_OK);
  ASSERT_NE(a.Sync(), VXCORE_OK);
  ASSERT_EQ(a.Read("preserved.md"), "journal-owned intended bytes");
  ASSERT_EQ(Head(), old_head);
  ASSERT_FALSE(Inspect(canonical).value("exists", false));
  ASSERT_EQ(RemoteBytes(staging), "interference after staged upload acknowledgement");
  return 0;
}

void ReplaceManifestFile(Json &manifest, const std::string &path, const std::string &bytes) {
  managed::Entry entry;
  entry.kind = "file";
  entry.revision = Identity();
  entry.size = bytes.size();
  entry.sha256 = Hash(bytes);
  if (!bytes.empty()) {
    entry.chunks.push_back({entry.sha256, entry.size});
    PutRemote(managed::ObjectPath(entry.sha256), bytes);
  }
  manifest[key::kEntries][path] = managed::EncodeEntry(entry);
}
int TestMetadataCohort() {
  for (const auto *metadata : {kConfigPath, "vx_notebook/contents/vx.json"}) {
    Control({{"action", "reset"}});
    Client a;
    a.Write("a-payload.md", "old payload");
    ASSERT_EQ(a.Attach(), VXCORE_OK);
    ASSERT_EQ(a.Sync(), VXCORE_OK);
    const auto before_metadata = a.Read(metadata), before_state = a.Read(kStatePath);
    auto manifest = Manifest();
    ReplaceManifestFile(manifest, "a-payload.md", "must remain staged with invalid metadata");
    ReplaceManifestFile(manifest, metadata, "{invalid notebook metadata");
    PublishManifest(manifest);
    ASSERT_NE(a.Sync(), VXCORE_OK);
    ASSERT_EQ(a.Read("a-payload.md"), "old payload");
    ASSERT_EQ(a.Read(metadata), before_metadata);
    ASSERT_EQ(a.Read(kStatePath), before_state);
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  vxcore_set_test_mode(1);
  if (sodium_init() < 0 || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
  try {
    for (const auto *name :
         {"VXCORE_WEBDAV_TEST_URL", "VXCORE_WEBDAV_TEST_CA_FILE", "VXCORE_WEBDAV_TEST_USERNAME",
          "VXCORE_WEBDAV_TEST_PASSWORD", "VXCORE_WEBDAV_TEST_CONTROL_URL",
          "VXCORE_WEBDAV_TEST_CONTROL_TOKEN", "VXCORE_WEBDAV_TEST_CLIENT_ROOT",
          "VXCORE_WEBDAV_TEST_RUNNER", "VXCORE_WEBDAV_TEST_PYTHON"})
      if (Environment(name).empty())
        throw std::runtime_error("Run managed tests through the TLS fixture runner");
    if (Environment("VXCORE_WEBDAV_TEST_PROFILE") != "jianguoyun")
      throw std::runtime_error("Managed tests require explicit --profile jianguoyun");
    executable = PathToUtf8(fs::absolute(PathFromUtf8(argv[0])));
    if (argc == 5 && std::string(argv[1]) == "--child")
      return DispatchChild(argv[2], PathFromUtf8(argv[3]), PathFromUtf8(argv[4]));
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--case"))
      throw std::runtime_error("Unknown managed sync test arguments");
    const std::string selected = argc == 3 ? argv[2] : "";
    struct TestCase {
      const char *name;
      int (*run)();
    };
    const TestCase cases[] = {
        {"bootstrap", TestBootstrap},
        {"bootstrap-same", TestBootstrapSame},
        {"bootstrap-different", TestBootstrapDifferent},
        {"two-device", TestTwoDevice},
        {"isolated-clone", TestIsolatedClone},
        {"unbased-tombstone", TestUnbasedTombstone},
        {"conflict-restart", TestConflictRestart},
        {"delete-conflicts", TestDeleteConflicts},
        {"stale-choice", TestStaleChoice},
        {"metadata-ciphertext", TestMetadataCiphertext},
        {"encrypted-note-cohort", TestEncryptedNoteDoesNotBlockKeyCohort},
        {"disjoint", TestDisjoint},
        {"immutable-staging", TestImmutableStaging},
        {"corrupt-objects", TestCorruptObjects},
        {"staged-recovery", TestStagedRecoveryRechecksBytes},
        {"chunk-boundary", TestChunkBoundary},
        {"large-logical-file", TestLargeLogicalFile},
        {"manifest-750", TestManifestBeyondListingLimit},
        {"raw-cas", TestRawCasConcurrency},
        {"lost-ack", TestLostAcknowledgement},
        {"descendant-ack", TestDescendantAcknowledgement},
        {"cancel-resume", TestCancelResume},
        {"quota-resume", TestQuotaResume},
        {"process-crashes", TestProcessCrashes},
        {"local-crash", TestLocalCrash},
        {"partial-apply", TestPartialApply},
        {"protected-cohort", TestProtectedCohort},
        {"damaged-state", TestDamagedState},
        {"invalid-manifest", TestInvalidManifest},
        {"metadata-cohort", TestMetadataCohort},
        {"rollback", TestRollbackAndRecreation},
        {"setup-binding", TestSetupBindingRejectsRecreation},
        {"type-collision", TestTypeCollisionsRequireRename},
        {"truncated-body", TestTruncatedObject},
    };
    bool found = false;
    for (const auto &test : cases) {
      if (!selected.empty() && selected != test.name) continue;
      found = true;
      std::cout << "Jianguoyun sync: " << test.name << std::endl;
      RUN_TEST(test.run);
    }
    if (!found) throw std::runtime_error("Unknown managed sync test case");
    curl_global_cleanup();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Managed sync test failed: " << error.what() << '\n';
    return 1;
  }
}
