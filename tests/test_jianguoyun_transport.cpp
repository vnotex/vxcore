#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <curl/curl.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/config_manager.h"
#include "sync/jianguoyun/jianguoyun_transport.h"
#include "sync/sync_json_keys.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"

namespace {
using namespace vxcore;
using Json = nlohmann::json;
constexpr const char *kStage = ".vnote-sync/staging/operation/";
constexpr const char *kHead = ".vnote-sync/head.json";

std::string Environment(const char *name) {
  const auto *value = std::getenv(name);
  return value ? value : "";
}

std::string RootUrl() { return Environment("VXCORE_WEBDAV_TEST_URL"); }

SyncCredentials Credentials() {
  SyncCredentials credentials;
  credentials.extra = {{kJsonKeyUsername, Environment("VXCORE_WEBDAV_TEST_USERNAME")},
                       {kJsonKeyPassword, Environment("VXCORE_WEBDAV_TEST_PASSWORD")}};
  return credentials;
}

size_t ControlWrite(char *bytes, size_t size, size_t count, void *userdata) {
  auto &output = *static_cast<std::string *>(userdata);
  if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
  const auto length = size * count;
  if (length > 1024 * 1024 - output.size()) return 0;
  output.append(bytes, length);
  return length;
}

bool Control(const Json &input, Json *result = nullptr) {
  const auto url = Environment("VXCORE_WEBDAV_TEST_CONTROL_URL");
  const auto token = Environment("VXCORE_WEBDAV_TEST_CONTROL_TOKEN");
  const auto ca = Environment("VXCORE_WEBDAV_TEST_CA_FILE");
  const auto body = input.dump();
  auto *handle = curl_easy_init();
  if (!handle) return false;
  auto *headers = curl_slist_append(nullptr, "Content-Type: application/json");
  headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
  std::string output;
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
  if (!ca.empty()) curl_easy_setopt(handle, CURLOPT_CAINFO, ca.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, ControlWrite);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &output);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  const auto code = curl_easy_perform(handle);
  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  if (code != CURLE_OK || status != 200) {
    std::cerr << "Fixture control failed (redacted), HTTP " << status << '\n';
    return false;
  }
  const auto response = Json::parse(output, nullptr, false);
  if (response.is_discarded() || !response.value("ok", false)) return false;
  if (result) *result = response;
  return true;
}

struct EnvironmentOverride {
  std::string name, previous;
  bool present;
  EnvironmentOverride(const char *key, const char *value)
      : name(key), previous(Environment(key)), present(std::getenv(key) != nullptr) {
    Set(value);
  }
  void Set(const char *value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value ? value : "");
#else
    if (value)
      setenv(name.c_str(), value, 1);
    else
      unsetenv(name.c_str());
#endif
  }
  ~EnvironmentOverride() { Set(present ? previous.c_str() : nullptr); }
};

struct TestMode {
  bool previous = ConfigManager::IsTestMode();
  explicit TestMode(bool enabled) { ConfigManager::SetTestMode(enabled); }
  ~TestMode() { ConfigManager::SetTestMode(previous); }
};

struct LocalDirectory {
  std::filesystem::path path;
  LocalDirectory() {
    std::array<unsigned char, 12> random{};
    std::array<char, 25> hex{};
    randombytes_buf(random.data(), random.size());
    sodium_bin2hex(hex.data(), hex.size(), random.data(), random.size());
    path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           PathFromUtf8(std::string("jianguoyun-transport-") + hex.data());
    std::filesystem::create_directory(path);
  }
  ~LocalDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  bool Write(const std::string &relative, const std::string &content) {
    return WriteFileAtomic(path / PathFromUtf8(relative), content) == VXCORE_OK;
  }
  std::string Read(const std::string &relative) const {
    std::ifstream input(path / PathFromUtf8(relative), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};

std::string Hash(const std::string &bytes) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char *>(bytes.data()),
                     bytes.size());
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  return hex.data();
}

bool Containers(JianguoyunTransport &transport) {
  for (const auto *path : {".vnote-sync", ".vnote-sync/staging", ".vnote-sync/staging/operation",
                           ".vnote-sync/objects", ".vnote-sync/commits"})
    if (transport.EnsureCollection(path) != VXCORE_OK) return false;
  return true;
}

std::string ResponseXml(const std::string &href, const std::string &properties) {
  return "<d:response><d:href>" + href + "</d:href><d:propstat><d:prop>" + properties +
         "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
}

std::string Multistatus(const std::string &responses) {
  return "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">" + responses + "</d:multistatus>";
}

int TestEndpointBoundary() {
  const std::vector<std::pair<std::string, std::string>> accepted{
      {"https://dav.jianguoyun.com/dav/notebook", "https://dav.jianguoyun.com/dav/notebook/"},
      {"HTTPS://DAV.JIANGUOYUN.COM:443/dav/parent/notebook/",
       "https://dav.jianguoyun.com/dav/parent/notebook/"},
      {"https://dav.jianguoyun.com/dav/space%20name/",
       "https://dav.jianguoyun.com/dav/space%20name/"}};
  const std::vector<std::string> rejected{"https://dav.jianguoyun.com/dav/",
                                          "https://dav.jianguoyun.com/dav",
                                          "https://dav.jianguoyun.com/",
                                          "http://dav.jianguoyun.com/dav/book/",
                                          "https://dav.jianguoyun.com:8443/dav/book/",
                                          "https://other.example/dav/book/",
                                          "https://user@dav.jianguoyun.com/dav/book/",
                                          "https://dav.jianguoyun.com/dav/book/?x=1",
                                          "https://dav.jianguoyun.com/dav/book/#fragment",
                                          "https://dav.jianguoyun.com/dav//book/",
                                          "https://dav.jianguoyun.com/dav/book//",
                                          "https://dav.jianguoyun.com/dav/%2E%2E/book/",
                                          "https://dav.jianguoyun.com/dav/book%2Fchild/",
                                          "https://dav.jianguoyun.com/dav/CON/",
                                          "https://dav.jianguoyun.com/dav/trailing./",
                                          "https://dav.jianguoyun.com/dav/trailing%20/"};
  {
    TestMode production(false);
    for (const auto &entry : accepted) {
      std::string output;
      ASSERT_EQ(JianguoyunTransport::CanonicalizeUrl(entry.first, output), VXCORE_OK);
      ASSERT_EQ(output, entry.second);
    }
    for (const auto &url : rejected) {
      std::string output = "stale";
      ASSERT_EQ(JianguoyunTransport::CanonicalizeUrl(url, output), VXCORE_ERR_INVALID_PARAM);
      ASSERT_TRUE(output.empty());
    }
    // The real fixture URL, CA and credentials remain in the environment. None
    // authorize a loopback endpoint without the authoritative core test flag.
    JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
    ASSERT_EQ(transport.Initialize(), VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(transport.LastResponse().http_status, 0L);
  }
  for (const auto *url : {"https://localhost:8443/notebook/", "https://127.0.0.1:8443/notebook/",
                          "https://[::1]:8443/notebook/"}) {
    EnvironmentOverride endpoint("VXCORE_WEBDAV_TEST_URL", url);
    std::string output;
    ASSERT_EQ(JianguoyunTransport::CanonicalizeUrl(url, output), VXCORE_OK);
    ASSERT_EQ(output, std::string(url));
  }
  for (const auto *url : {"http://127.0.0.1:8443/notebook/", "https://remote.example/notebook/"}) {
    EnvironmentOverride endpoint("VXCORE_WEBDAV_TEST_URL", url);
    std::string output = "stale";
    ASSERT_EQ(JianguoyunTransport::CanonicalizeUrl(url, output), VXCORE_ERR_INVALID_PARAM);
    ASSERT_TRUE(output.empty());
  }
  std::string output;
  ASSERT_EQ(JianguoyunTransport::CanonicalizeUrl(RootUrl() + "different/", output),
            VXCORE_ERR_INVALID_PARAM);
  JianguoyunTransport initialized(RootUrl(), Credentials(), nullptr);
  ASSERT_EQ(initialized.Initialize(), VXCORE_OK);
  {
    TestMode production(false);
    JianguoyunResource resource{true, 123, "stale"};
    ASSERT_EQ(initialized.Stat("", resource), VXCORE_ERR_INVALID_PARAM);
    ASSERT_FALSE(resource.collection);
    ASSERT_EQ(resource.size, 0U);
    ASSERT_TRUE(resource.etag.empty());
    ASSERT_EQ(initialized.LastResponse().http_status, 0L);
  }
  return 0;
}

int TestRawValidatorsAndCas() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_TRUE(Containers(transport));
  const auto stage = std::string(kStage) + "head";
  ASSERT_EQ(transport.PutBytes(stage, "generation one"), VXCORE_OK);
  ASSERT_EQ(transport.LastResponse().http_status, 201L);
  ASSERT_TRUE(transport.LastResponse().etag.empty());
  std::string body, etag;
  ASSERT_EQ(transport.Get(stage, body, etag), VXCORE_OK);
  ASSERT_EQ(body, "generation one");
  ASSERT_TRUE(JianguoyunTransport::IsRawEtag(etag));
  JianguoyunResource resource;
  ASSERT_EQ(transport.Stat(stage, resource), VXCORE_OK);
  ASSERT_EQ(resource.etag, etag);
  ASSERT_EQ(resource.size, body.size());
  ASSERT_EQ(transport.MoveCreate(stage, kHead), VXCORE_OK);
  ASSERT_EQ(transport.Get(kHead, body, etag), VXCORE_OK);
  const auto first = etag;
  for (const auto &invalid : {std::string(), "\"" + first + "\"", std::string("*"),
                              first + ",other", "W/" + first, first + "\r\nX: injected"}) {
    ASSERT_EQ(transport.PutBytes(kHead, "must not win", invalid), VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(transport.LastResponse().http_status, 0L);
  }
  ASSERT_EQ(transport.PutBytes(kHead, "wrong token", "not-current"), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 412L);
  ASSERT_EQ(transport.PutBytes(kHead, "generation two", first), VXCORE_OK);
  ASSERT_EQ(transport.LastResponse().http_status, 204L);
  ASSERT_EQ(transport.PutBytes(kHead, "stale overwrite", first), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 412L);
  ASSERT_EQ(transport.Get(kHead, body, etag), VXCORE_OK);
  ASSERT_EQ(body, "generation two");
  ASSERT_NE(etag, first);
  for (const auto *effect : {"quoted_etag", "weak_etag", "missing_etag"}) {
    ASSERT_TRUE(
        Control({{"action", "fault"}, {"method", "GET"}, {"path", kHead}, {"effect", effect}}));
    body = "stale";
    etag = "stale";
    ASSERT_EQ(transport.Get(kHead, body, etag), VXCORE_ERR_UNSUPPORTED);
    ASSERT_TRUE(body.empty());
    ASSERT_TRUE(etag.empty());
    ASSERT_EQ(transport.LastResponse().http_status, 200L);
  }
  ASSERT_EQ(transport.Get("missing", body, etag), VXCORE_ERR_NOT_FOUND);
  ASSERT_TRUE(body.empty());
  ASSERT_TRUE(etag.empty());
  return 0;
}

int TestImmutablePublicationBoundary() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_TRUE(Containers(transport));
  const auto hash = Hash("winner");
  const auto object_parent = ".vnote-sync/objects/" + hash.substr(0, 2);
  const auto object = object_parent + "/" + hash;
  const auto commit = ".vnote-sync/commits/" + hash + ".json";
  ASSERT_EQ(transport.EnsureCollection(object_parent), VXCORE_OK);
  const auto stage = std::string(kStage) + "object";
  ASSERT_EQ(transport.PutBytes(stage, "winner"), VXCORE_OK);
  ASSERT_EQ(transport.MoveCreate(stage, object), VXCORE_OK);
  ASSERT_EQ(transport.LastResponse().http_status, 201L);
  ASSERT_EQ(transport.PutBytes(stage, "loser"), VXCORE_OK);
  ASSERT_EQ(transport.MoveCreate(stage, object), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 409L);
  std::string body, tag;
  ASSERT_EQ(transport.Get(object, body, tag), VXCORE_OK);
  ASSERT_EQ(body, "winner");
  ASSERT_EQ(transport.Get(stage, body, tag), VXCORE_OK);
  ASSERT_EQ(body, "loser");
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("snapshot", "corruption"));
  for (const auto &target : {object, commit, std::string(kHead), std::string("ordinary-file"),
                             std::string(".vnote-sync/staging/operation"),
                             std::string(".vnote-sync/staging/operation/extra/file")}) {
    ASSERT_EQ(transport.UploadRange(target, directory.path, "snapshot", 0, 10),
              VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(transport.LastResponse().http_status, 0L);
    ASSERT_EQ(transport.PutBytes(target, "corruption"), VXCORE_ERR_INVALID_PARAM);
    ASSERT_EQ(transport.LastResponse().http_status, 0L);
  }
  ASSERT_EQ(transport.PutBytes(object, "corruption", tag), VXCORE_ERR_INVALID_PARAM);
  for (const auto &target :
       {std::string("ordinary-file"), ".vnote-sync/objects/zz/" + hash,
        ".vnote-sync/commits/" + hash, ".vnote-sync/commits/" + hash + ".json/child"})
    ASSERT_EQ(transport.MoveCreate(stage, target), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(transport.MoveCreate(object, commit), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(transport.Get(object, body, tag), VXCORE_OK);
  ASSERT_EQ(body, "winner");
  ASSERT_EQ(transport.MoveCreate(stage, commit), VXCORE_OK);
  ASSERT_EQ(transport.Get(commit, body, tag), VXCORE_OK);
  ASSERT_EQ(body, "loser");
  return 0;
}

int TestDepthZeroValidation() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "file"}, {"text", "x"}}));
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  for (const auto *mode : {"path", "absolute", "relative"}) {
    ASSERT_TRUE(Control({{"action", "configure"}, {"href_mode", mode}, {"namespace", "default"}}));
    JianguoyunResource resource;
    ASSERT_EQ(transport.Stat("file", resource), VXCORE_OK);
    ASSERT_FALSE(resource.collection);
    ASSERT_EQ(resource.size, 1U);
    ASSERT_TRUE(JianguoyunTransport::IsRawEtag(resource.etag));
  }
  ASSERT_TRUE(Control(
      {{"action", "fault"}, {"method", "PROPFIND"}, {"path", "file"}, {"effect", "missing_etag"}}));
  JianguoyunResource resource;
  ASSERT_EQ(transport.Stat("file", resource), VXCORE_OK);
  ASSERT_EQ(resource.size, 1U);
  ASSERT_TRUE(resource.etag.empty());
  const std::string properties = "<d:resourcetype/><d:getcontentlength>1</d:getcontentlength>";
  const auto self = ResponseXml("/notebook/file", properties);
  const std::vector<std::string> malformed{
      Multistatus(self + self),
      Multistatus(ResponseXml("/notebook/other", properties)),
      Multistatus(ResponseXml("https://other.example/notebook/file", properties)),
      Multistatus(ResponseXml("/notebook/%2e%2e/file", properties)),
      Multistatus(ResponseXml("/notebook/file", "<d:getcontentlength>1</d:getcontentlength>")),
      Multistatus(ResponseXml("/notebook/file", "<d:resourcetype/>")),
      Multistatus(
          ResponseXml("/notebook/file", properties + "<d:getetag>&quot;quoted&quot;</d:getetag>")),
      Multistatus(
          ResponseXml("/notebook/file", properties + "<d:getcontentlength>2</d:getcontentlength>")),
      "<!DOCTYPE d:multistatus [<!ENTITY x SYSTEM \"file:///private\">]>" + Multistatus(self),
      Multistatus(
          "<d:response><d:href>/notebook/file</d:href><d:propstat><d:prop>" + properties +
          "</d:prop><d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat></d:response>")};
  for (const auto &xml : malformed) {
    ASSERT_TRUE(Control({{"action", "fault"},
                         {"method", "PROPFIND"},
                         {"path", "file"},
                         {"effect", "xml"},
                         {"body", xml}}));
    resource = {true, 123, "stale"};
    ASSERT_NE(transport.Stat("file", resource), VXCORE_OK);
    ASSERT_FALSE(resource.collection);
    ASSERT_EQ(resource.size, 0U);
    ASSERT_TRUE(resource.etag.empty());
    ASSERT_EQ(transport.LastResponse().http_status, 207L);
  }
  return 0;
}

int TestEnsureCollectionRaces() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_EQ(transport.EnsureCollection(""), VXCORE_OK);
  ASSERT_EQ(transport.EnsureCollection("ordinary"), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(transport.EnsureCollection(".vnote-sync"), VXCORE_OK);
  const std::string existing = ".vnote-sync/already-there";
  ASSERT_TRUE(Control({{"action", "mkdir"}, {"path", existing}}));
  // Simulate a collection created between our absent observation and MKCOL.
  // Jianguoyun returns 201 for it, so only the following Stat can verify type.
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "PROPFIND"},
                       {"path", existing},
                       {"effect", "status"},
                       {"status", 404}}));
  ASSERT_EQ(transport.EnsureCollection(existing), VXCORE_OK);
  JianguoyunResource resource;
  ASSERT_EQ(transport.Stat(existing, resource), VXCORE_OK);
  ASSERT_TRUE(resource.collection);
  const std::string collision = ".vnote-sync/file";
  ASSERT_TRUE(Control({{"action", "put"}, {"path", collision}, {"text", "preserve"}}));
  ASSERT_EQ(transport.EnsureCollection(collision), VXCORE_ERR_UNSUPPORTED);
  // A 201 acknowledgement followed by an externally replaced file is not success.
  const std::string replaced = ".vnote-sync/replaced";
  ASSERT_TRUE(Control({{"action", "barrier"},
                       {"id", "mkcol-type"},
                       {"method", "MKCOL"},
                       {"path", replaced},
                       {"phase", "after"}}));
  auto future =
      std::async(std::launch::async, [&] { return transport.EnsureCollection(replaced); });
  Json reached;
  const bool waiting =
      Control({{"action", "wait"}, {"id", "mkcol-type"}, {"timeout_ms", 5000}}, &reached);
  const bool removed = Control({{"action", "delete"}, {"path", replaced}, {"recursive", true}});
  const bool replaced_file =
      Control({{"action", "put"}, {"path", replaced}, {"text", "other owner"}});
  const bool released = Control({{"action", "release"}, {"id", "mkcol-type"}});
  const auto result = future.get();
  ASSERT_TRUE(waiting && reached.value("reached", false) && removed && replaced_file && released);
  ASSERT_EQ(result, VXCORE_ERR_UNSUPPORTED);
  std::string bytes, tag;
  ASSERT_EQ(transport.Get(collision, bytes, tag), VXCORE_OK);
  ASSERT_EQ(bytes, "preserve");
  ASSERT_EQ(transport.Get(replaced, bytes, tag), VXCORE_OK);
  ASSERT_EQ(bytes, "other owner");
  return 0;
}

int TestAtomicDownloadVerification() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  const std::string bytes = "remote content";
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "file"}, {"text", bytes}}));
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("destination", "original local bytes"));
  ASSERT_EQ(transport.Download("file", directory.path, "destination", Hash("wrong"), bytes.size()),
            VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(directory.Read("destination"), "original local bytes");
  ASSERT_EQ(
      transport.Download("file", directory.path, "destination", Hash(bytes), bytes.size() + 1),
      VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(directory.Read("destination"), "original local bytes");
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "GET"},
                       {"path", "file"},
                       {"effect", "truncate"},
                       {"bytes", 3}}));
  ASSERT_EQ(transport.Download("file", directory.path, "destination", Hash(bytes), bytes.size()),
            VXCORE_ERR_SYNC_NETWORK);
  ASSERT_EQ(directory.Read("destination"), "original local bytes");
  ASSERT_EQ(transport.Download("file", directory.path, "../escape", Hash(bytes), bytes.size()),
            VXCORE_ERR_INVALID_PARAM);
  // Immutable content verification remains independent of remote validator syntax.
  ASSERT_TRUE(Control(
      {{"action", "fault"}, {"method", "GET"}, {"path", "file"}, {"effect", "missing_etag"}}));
  ASSERT_EQ(transport.Download("file", directory.path, "destination", Hash(bytes), bytes.size()),
            VXCORE_OK);
  ASSERT_EQ(directory.Read("destination"), bytes);
  std::string body = "stale", tag = "stale";
  ASSERT_EQ(transport.Get("file", body, tag, bytes.size() - 1), VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(body.empty());
  ASSERT_TRUE(tag.empty());
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "empty"}, {"text", ""}}));
  ASSERT_EQ(transport.Download("empty", directory.path, "destination", Hash(""), 0), VXCORE_OK);
  ASSERT_TRUE(directory.Read("destination").empty());
  return 0;
}

int TestRangeAuthenticationAndSnapshotStability() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "configure"}, {"auth", "digest"}}));
  LocalDirectory directory;
  std::string range(2 * 1024 * 1024 + 19, '\0');
  for (size_t i = 0; i < range.size(); ++i) range[i] = static_cast<char>(i % 251);
  const std::string prefix(777, 'p');
  ASSERT_TRUE(directory.Write("snapshot", prefix + range + "suffix-not-uploaded"));
  JianguoyunTransport setup(RootUrl(), Credentials(), nullptr);
  ASSERT_TRUE(Containers(setup));
  // Fresh client: the first request challenges auth, and curl must rewind to
  // the range offset rather than silently upload/hash the file prefix.
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  const auto stage = std::string(kStage) + "range";
  ASSERT_EQ(transport.UploadRange(stage, directory.path, "snapshot", prefix.size(), range.size()),
            VXCORE_OK);
  ASSERT_EQ(transport.LastResponse().bytes, range.size());
  ASSERT_EQ(transport.LastResponse().sha256, Hash(range));
  Json remote;
  ASSERT_TRUE(Control({{"action", "inspect"}, {"path", stage}}, &remote));
  ASSERT_EQ(remote["sha256"].get<std::string>(), Hash(range));
  ASSERT_EQ(remote["size"].get<uint64_t>(), range.size());
  std::string body, tag;
  ASSERT_EQ(transport.Get(stage, body, tag), VXCORE_OK);
  ASSERT_EQ(body, range);
  ASSERT_EQ(transport.UploadRange(stage, directory.path, "snapshot",
                                  std::numeric_limits<uint64_t>::max(), 1),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(transport.UploadRange(stage, directory.path, "snapshot", 0, 16 * 1024 * 1024 + 1),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(transport.UploadRange(stage, directory.path, "snapshot",
                                  prefix.size() + range.size() + 1, 1024),
            VXCORE_ERR_INVALID_PARAM);
  const auto changed = std::string(kStage) + "changed";
  ASSERT_TRUE(Control({{"action", "barrier"},
                       {"id", "range-change"},
                       {"method", "PUT"},
                       {"path", changed},
                       {"phase", "after"}}));
  auto future = std::async(std::launch::async, [&] {
    return transport.UploadRange(changed, directory.path, "snapshot", prefix.size(), range.size());
  });
  Json reached;
  const bool waiting =
      Control({{"action", "wait"}, {"id", "range-change"}, {"timeout_ms", 5000}}, &reached);
  // The file already sent is no longer the snapshot on disk. An acknowledged
  // staging upload is still reported as unstable, never as publishable success.
  std::error_code ec;
  std::filesystem::resize_file(directory.path / "snapshot", 1, ec);
  const bool released = Control({{"action", "release"}, {"id", "range-change"}});
  const auto result = future.get();
  ASSERT_TRUE(waiting && reached.value("reached", false) && !ec && released);
  ASSERT_EQ(result, VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(transport.LastResponse().http_status, 201L);
  ASSERT_TRUE(Control({{"action", "inspect"}, {"path", changed}}, &remote));
  ASSERT_EQ(remote["sha256"].get<std::string>(), Hash(range));
  return 0;
}

int TestQuotaAuthenticationAndCancellation() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  JianguoyunTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_TRUE(Containers(transport));
  const auto stage = std::string(kStage) + "quota";
  ASSERT_EQ(transport.PutBytes(stage, "retained staging"), VXCORE_OK);
  for (const auto status : {429, 507}) {
    for (const auto *retry : {"120", "999999999999999999999999999999", "not-a-date"}) {
      ASSERT_TRUE(Control({{"action", "fault"},
                           {"method", "PUT"},
                           {"path", stage},
                           {"effect", "status"},
                           {"status", status},
                           {"headers", {{"Retry-After", retry}}},
                           {"body", "private quota details"}}));
      ASSERT_EQ(transport.PutBytes(stage, "rejected"),
                status == 429 ? VXCORE_ERR_SYNC_NETWORK : VXCORE_ERR_IO);
      ASSERT_EQ(transport.LastResponse().http_status, static_cast<long>(status));
      std::string body, tag;
      ASSERT_EQ(transport.Get(stage, body, tag), VXCORE_OK);
      ASSERT_EQ(body, "retained staging");
    }
  }
  auto incorrect = Credentials();
  incorrect.extra[kJsonKeyPassword] = "deliberately incorrect credential";
  JianguoyunTransport denied(RootUrl(), incorrect, nullptr);
  std::string body = "stale", tag = "stale";
  ASSERT_EQ(denied.Get(stage, body, tag), VXCORE_ERR_SYNC_AUTH_FAILED);
  ASSERT_EQ(denied.LastResponse().http_status, 401L);
  ASSERT_TRUE(body.empty());
  ASSERT_TRUE(tag.empty());
  {
    EnvironmentOverride no_ca("VXCORE_WEBDAV_TEST_CA_FILE", nullptr);
    JianguoyunTransport untrusted(RootUrl(), Credentials(), nullptr);
    ASSERT_EQ(untrusted.Get(stage, body, tag), VXCORE_ERR_SYNC_NETWORK);
    ASSERT_TRUE(body.empty());
    ASSERT_TRUE(tag.empty());
  }
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("preserved", "local original"));
  auto token = std::make_shared<SyncCancellation>();
  JianguoyunTransport cancelled(RootUrl(), Credentials(), token);
  ASSERT_TRUE(Control({{"action", "barrier"},
                       {"id", "cancel-get"},
                       {"method", "GET"},
                       {"path", stage},
                       {"phase", "after_headers"}}));
  auto future = std::async(std::launch::async, [&] {
    return cancelled.Download(stage, directory.path, "preserved", Hash("retained staging"), 16);
  });
  Json reached;
  const bool waiting =
      Control({{"action", "wait"}, {"id", "cancel-get"}, {"timeout_ms", 5000}}, &reached);
  token->Cancel();
  const auto ready = future.wait_for(std::chrono::seconds(2));
  const bool released = Control({{"action", "release"}, {"id", "cancel-get"}});
  const auto result = future.get();
  ASSERT_TRUE(waiting && reached.value("reached", false) && released);
  ASSERT_TRUE(ready == std::future_status::ready);
  ASSERT_EQ(result, VXCORE_ERR_CANCELLED);
  ASSERT_EQ(directory.Read("preserved"), "local original");
  ASSERT_EQ(cancelled.PutBytes(stage, "must not upload"), VXCORE_ERR_CANCELLED);
  ASSERT_EQ(cancelled.LastResponse().http_status, 0L);
  ASSERT_EQ(transport.Get(stage, body, tag), VXCORE_OK);
  ASSERT_EQ(body, "retained staging");
  return 0;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  // Direct-compile sources have a separate ConfigManager static on Windows.
  ConfigManager::SetTestMode(true);
  if (RootUrl().empty() || Environment("VXCORE_WEBDAV_TEST_CONTROL_TOKEN").empty() ||
      Environment("VXCORE_WEBDAV_TEST_PROFILE") != "jianguoyun") {
    std::cerr << "Run via run_webdav_test.py --profile jianguoyun -- test_jianguoyun_transport\n";
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || sodium_init() < 0) return 1;
  RUN_TEST(TestEndpointBoundary);
  RUN_TEST(TestRawValidatorsAndCas);
  RUN_TEST(TestImmutablePublicationBoundary);
  RUN_TEST(TestDepthZeroValidation);
  RUN_TEST(TestEnsureCollectionRaces);
  RUN_TEST(TestAtomicDownloadVerification);
  RUN_TEST(TestRangeAuthenticationAndSnapshotStability);
  RUN_TEST(TestQuotaAuthenticationAndCancellation);
  std::cout << "Jianguoyun managed transport checks passed\n";
  return 0;
}
