#include <curl/curl.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "core/config_manager.h"
#include "sync/sync_json_keys.h"
#include "sync/webdav/webdav_transport.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"

namespace {
using namespace vxcore;
using Json = nlohmann::json;

std::string Environment(const char *name) {
  const auto *value = std::getenv(name);
  return value ? value : "";
}

SyncCredentials Credentials() {
  SyncCredentials credentials;
  credentials.extra = {{kJsonKeyUsername, Environment("VXCORE_WEBDAV_TEST_USERNAME")},
                       {kJsonKeyPassword, Environment("VXCORE_WEBDAV_TEST_PASSWORD")}};
  return credentials;
}

std::string RootUrl() { return Environment("VXCORE_WEBDAV_TEST_URL"); }

size_t ControlWrite(char *bytes, size_t size, size_t count, void *userdata) {
  auto &output = *static_cast<std::string *>(userdata);
  const size_t length = size * count;
  if (length > 1024 * 1024 - output.size()) return 0;
  output.append(bytes, length);
  return length;
}

bool Control(Json input, Json *result = nullptr) {
  const auto url = Environment("VXCORE_WEBDAV_TEST_CONTROL_URL");
  const auto token = Environment("VXCORE_WEBDAV_TEST_CONTROL_TOKEN");
  const auto ca = Environment("VXCORE_WEBDAV_TEST_CA_FILE");
  const auto body = input.dump();
  CURL *handle = curl_easy_init();
  if (!handle) return false;
  curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
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
    std::cerr << "Fixture control request failed (redacted), HTTP " << status << '\n';
    return false;
  }
  const auto response = Json::parse(output, nullptr, false);
  if (response.is_discarded() || !response.value("ok", false)) return false;
  if (result) *result = response;
  return true;
}

struct LocalDirectory {
  std::filesystem::path path;
  LocalDirectory() {
    std::array<unsigned char, 12> random{};
    randombytes_buf(random.data(), random.size());
    std::array<char, 25> hex{};
    sodium_bin2hex(hex.data(), hex.size(), random.data(), random.size());
    path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           PathFromUtf8(std::string("webdav-transport-") + hex.data());
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

struct EnvironmentOverride {
  std::string name, old_value;
  bool had_value;
  EnvironmentOverride(const char *key, const char *value)
      : name(key), old_value(Environment(key)), had_value(std::getenv(key) != nullptr) {
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
  ~EnvironmentOverride() { Set(had_value ? old_value.c_str() : nullptr); }
};

std::string ResponseXml(const std::string &href, bool collection = false,
                        const std::string &extra = "", const std::string &status = "200 OK") {
  return "<q:response><q:href>" + href +
         "</q:href><q:propstat><q:prop>"
         "<q:resourcetype>" +
         (collection ? std::string("<q:collection/>") : "") + "</q:resourcetype>" +
         (collection ? std::string()
                     : "<q:getetag>&quot;version&quot;</q:getetag><q:getcontentlength>1</"
                       "q:getcontentlength>") +
         extra + "</q:prop><q:status>HTTP/1.1 " + status + "</q:status></q:propstat></q:response>";
}

std::string Multistatus(const std::string &responses) {
  return "<?xml version=\"1.0\"?><q:multistatus xmlns:q=\"DAV:\">" + responses + "</q:multistatus>";
}

bool XmlFault(const std::string &body, const std::string &path = "") {
  return Control({{"action", "fault"},
                  {"method", "PROPFIND"},
                  {"path", path},
                  {"effect", "xml"},
                  {"body", body},
                  {"count", 1}});
}

int TestAuthenticationAndTls() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  for (const auto *auth : {"basic", "digest"}) {
    ASSERT_TRUE(Control({{"action", "configure"}, {"auth", auth}}));
    WebDavTransport transport(RootUrl(), Credentials(), nullptr);
    ASSERT_EQ(transport.Initialize(), VXCORE_OK);
    WebDavResource root;
    const auto result = transport.Stat("", root);
    if (result != VXCORE_OK)
      std::cerr << "Initial PROPFIND failed: " << transport.LastError() << " HTTP "
                << transport.LastResponse().http_status << '\n';
    ASSERT_EQ(result, VXCORE_OK);
    ASSERT_TRUE(root.kind == WebDavResourceKind::kCollection);
    auto incorrect = Credentials();
    incorrect.extra[kJsonKeyPassword] = "deliberately incorrect credential";
    WebDavTransport denied(RootUrl(), incorrect, nullptr);
    ASSERT_EQ(denied.Stat("", root), VXCORE_ERR_SYNC_AUTH_FAILED);
    ASSERT_EQ(denied.LastResponse().http_status, 401L);
    ASSERT_TRUE(denied.LastError().find("deliberately") == std::string::npos);
  }
  if (RootUrl().compare(0, 8, "https://") == 0) {
    EnvironmentOverride no_ca("VXCORE_WEBDAV_TEST_CA_FILE", nullptr);
    WebDavTransport untrusted(RootUrl(), Credentials(), nullptr);
    WebDavResource root;
    ASSERT_EQ(untrusted.Stat("", root), VXCORE_ERR_SYNC_NETWORK);
  }
  // Production mode must not consume the fixture's private test trust setting.
  ConfigManager::SetTestMode(false);
  WebDavTransport production(RootUrl(), Credentials(), nullptr);
  WebDavResource root;
  const auto production_error = production.Stat("", root);
  ConfigManager::SetTestMode(true);
  ASSERT_EQ(production_error, RootUrl().compare(0, 8, "https://") == 0 ? VXCORE_ERR_SYNC_NETWORK
                                                                       : VXCORE_ERR_INVALID_PARAM);
  ASSERT_TRUE(Control({{"action", "configure"}, {"anonymous_read", true}}));
  WebDavTransport anonymous(RootUrl(), SyncCredentials{}, nullptr);
  ASSERT_EQ(anonymous.Stat("", root), VXCORE_OK);
  return 0;
}

int TestNamesNamespacesAndCompleteListing() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "mkdir"}, {"path", "nested/empty"}, {"parents", true}}));
  const std::string name = u8"nested/笔记 space # %2F.md";
  ASSERT_TRUE(Control({{"action", "put"}, {"path", name}, {"text", "remote bytes\r\n"}}));
  ASSERT_TRUE(
      Control({{"action", "put"}, {"path", ".vnote-webdav-tmp-owned"}, {"text", "scratch"}}));
  for (const auto *xml_namespace : {"prefix", "default"}) {
    for (const auto *href_mode : {"path", "absolute", "relative"}) {
      ASSERT_TRUE(Control({{"action", "configure"},
                           {"namespace", xml_namespace},
                           {"namespace_prefix", "arbitrary"},
                           {"href_mode", href_mode}}));
      WebDavTransport transport(RootUrl(), Credentials(), nullptr);
      std::vector<WebDavResource> resources;
      ASSERT_EQ(transport.List(resources), VXCORE_OK);
      ASSERT_EQ(resources.size(), size_t(5));
      bool saw_note = false, saw_empty = false, saw_scratch = false;
      for (const auto &resource : resources) {
        if (resource.path == name) {
          saw_note = true;
          ASSERT_TRUE(WebDavTransport::IsStrongEtag(resource.etag));
          ASSERT_EQ(resource.size, uint64_t(14));
          LocalDirectory directory;
          ASSERT_EQ(transport.Download(name, resource.etag, directory.path, "note"), VXCORE_OK);
          ASSERT_EQ(directory.Read("note"), "remote bytes\r\n");
        }
        if (resource.path == "nested/empty")
          saw_empty = resource.kind == WebDavResourceKind::kCollection;
        if (resource.path == ".vnote-webdav-tmp-owned") saw_scratch = true;
      }
      ASSERT_TRUE(saw_note && saw_empty && saw_scratch);
    }
  }
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "PROPFIND"},
                       {"path", "nested"},
                       {"effect", "status"},
                       {"status", 500}}));
  WebDavTransport failed(RootUrl(), Credentials(), nullptr);
  std::vector<WebDavResource> resources(1);
  ASSERT_EQ(failed.List(resources), VXCORE_ERR_SYNC_NETWORK);
  ASSERT_TRUE(resources.empty());
  return 0;
}

int TestConditionalFileOperations() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("payload", "first version"));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_EQ(transport.Options(), VXCORE_OK);
  ASSERT_EQ(transport.MakeCollection("folder"), VXCORE_OK);
  ASSERT_EQ(transport.MakeCollection("folder"), VXCORE_OK);
  ASSERT_EQ(transport.Upload("scratch", directory.path, "payload", ""), VXCORE_OK);
  const auto first_etag = transport.LastResponse().etag;
  ASSERT_TRUE(WebDavTransport::IsStrongEtag(first_etag));
  ASSERT_EQ(transport.Upload("scratch", directory.path, "payload", ""), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 412L);
  ASSERT_EQ(transport.Move("scratch", first_etag, "folder/target", ""), VXCORE_OK);
  WebDavResource target;
  ASSERT_EQ(transport.Stat("folder/target", target), VXCORE_OK);
  ASSERT_EQ(transport.RemoveFile("folder/target", "\"wrong-precondition\""),
            VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 412L);
  ASSERT_EQ(transport.Stat("folder/target", target), VXCORE_OK);
  ASSERT_TRUE(directory.Write("payload", "second version"));
  ASSERT_EQ(transport.Upload("scratch", directory.path, "payload", ""), VXCORE_OK);
  const auto second_etag = transport.LastResponse().etag;
  ASSERT_EQ(transport.Move("scratch", second_etag, "folder/target", "\"wrong-precondition\""),
            VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 412L);
  ASSERT_EQ(transport.Download("folder/target", target.etag, directory.path, "check"), VXCORE_OK);
  ASSERT_EQ(directory.Read("check"), "first version");
  ASSERT_EQ(transport.Move("scratch", second_etag, "folder/target", target.etag), VXCORE_OK);
  ASSERT_EQ(transport.Stat("folder/target", target), VXCORE_OK);
  ASSERT_EQ(transport.Download("folder/target", target.etag, directory.path, "check"), VXCORE_OK);
  ASSERT_EQ(directory.Read("check"), "second version");
  ASSERT_EQ(transport.RemoveFile("folder/target", target.etag), VXCORE_OK);
  ASSERT_EQ(transport.Stat("folder/target", target), VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(transport.RemoveFile("folder", "\"anything\""), VXCORE_ERR_UNSUPPORTED);
  Json requests;
  ASSERT_TRUE(Control({{"action", "requests"}}, &requests));
  for (const auto &request : requests["requests"]) {
    ASSERT_FALSE(request["method"] == "DELETE" && request["path"] == "folder");
  }
  return 0;
}

int TestMutationRaceAndLostAcknowledgement() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "destination"}, {"text", "old"}}));
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("source", "new"));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_EQ(transport.Upload("scratch", directory.path, "source", ""), VXCORE_OK);
  const auto scratch_etag = transport.LastResponse().etag;
  WebDavResource old;
  ASSERT_EQ(transport.Stat("destination", old), VXCORE_OK);
  ASSERT_TRUE(Control({{"action", "barrier"},
                       {"id", "move-race"},
                       {"method", "MOVE"},
                       {"path", "scratch"},
                       {"phase", "before"}}));
  auto future = std::async(std::launch::async, [&] {
    return transport.Move("scratch", scratch_etag, "destination", old.etag);
  });
  Json reached;
  const bool waiting =
      Control({{"action", "wait"}, {"id", "move-race"}, {"timeout_ms", 5000}}, &reached);
  const bool changed =
      Control({{"action", "put"}, {"path", "destination"}, {"text", "external winner"}});
  const bool released = Control({{"action", "release"}, {"id", "move-race"}});
  const auto result = future.get();
  ASSERT_TRUE(waiting && reached.value("reached", false) && changed && released);
  ASSERT_EQ(result, VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(transport.LastResponse().http_status, 412L);
  WebDavResource winner;
  ASSERT_EQ(transport.Stat("destination", winner), VXCORE_OK);
  ASSERT_EQ(transport.Download("destination", winner.etag, directory.path, "check"), VXCORE_OK);
  ASSERT_EQ(directory.Read("check"), "external winner");
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "DELETE"},
                       {"path", "destination"},
                       {"phase", "after"},
                       {"effect", "drop"},
                       {"count", 1}}));
  ASSERT_EQ(transport.RemoveFile("destination", winner.etag), VXCORE_ERR_SYNC_NETWORK);
  Json state;
  ASSERT_TRUE(Control({{"action", "inspect"}, {"path", "destination"}}, &state));
  ASSERT_FALSE(state.value("exists", true));
  Json requests;
  ASSERT_TRUE(Control({{"action", "requests"}}, &requests));
  size_t delete_count = 0;
  for (const auto &request : requests["requests"]) {
    if (request["method"] == "DELETE" && request["authenticated"].get<bool>()) ++delete_count;
  }
  ASSERT_EQ(delete_count, size_t(1));
  return 0;
}

int TestReadRedirectsAndWriteRefusal() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "target"}, {"text", "target bytes"}}));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  WebDavResource target;
  ASSERT_EQ(transport.Stat("target", target), VXCORE_OK);
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("copy", "original"));
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "GET"},
                       {"path", "alias"},
                       {"effect", "redirect"},
                       {"location", RootUrl() + "target"}}));
  ASSERT_EQ(transport.Download("alias", target.etag, directory.path, "copy"), VXCORE_OK);
  ASSERT_EQ(directory.Read("copy"), "target bytes");
  ASSERT_EQ(transport.LastResponse().corrected_url, RootUrl() + "target");
  for (const auto &location :
       {std::string("https://127.0.0.1:1/stolen"), RootUrl() + "../escape",
        RootUrl() + "%2e%2e/escape", RootUrl() + "target?secret=forbidden"}) {
    ASSERT_TRUE(Control({{"action", "fault"},
                         {"method", "GET"},
                         {"path", "alias"},
                         {"effect", "redirect"},
                         {"location", location}}));
    ASSERT_EQ(transport.Download("alias", target.etag, directory.path, "copy"),
              VXCORE_ERR_UNSUPPORTED);
    ASSERT_EQ(directory.Read("copy"), "target bytes");
  }
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "GET"},
                       {"path", "loop"},
                       {"effect", "redirect"},
                       {"location", RootUrl() + "loop"},
                       {"count", 8}}));
  ASSERT_EQ(transport.Download("loop", target.etag, directory.path, "copy"),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "PUT"},
                       {"path", "upload"},
                       {"effect", "redirect"},
                       {"location", RootUrl() + "target"}}));
  ASSERT_EQ(transport.Upload("upload", directory.path, "copy", ""), VXCORE_ERR_UNSUPPORTED);
  ASSERT_EQ(transport.Stat("target", target), VXCORE_OK);
  ASSERT_EQ(transport.Download("target", target.etag, directory.path, "verify"), VXCORE_OK);
  ASSERT_EQ(directory.Read("verify"), "target bytes");
  return 0;
}

int TestHostileXmlAndHrefs() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  const auto root = ResponseXml("/notebook/", true);
  const std::vector<std::string> hrefs{"https://example.invalid/notebook/evil",
                                       "/notebook-other/evil",
                                       "/notebook/../evil",
                                       "/notebook/%2e%2e/evil",
                                       "/notebook/%2Fetc",
                                       "/notebook/%5Cevil",
                                       "/notebook/%00evil",
                                       "/notebook/C%3Aevil",
                                       "/notebook/CON.txt",
                                       "/notebook/name.",
                                       "/notebook/name%20",
                                       "/notebook/%C0%AE%C0%AE",
                                       "/notebook/a//b",
                                       "/notebook/a/b",
                                       "/notebook/x?query=1"};
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  for (const auto &href : hrefs) {
    ASSERT_TRUE(XmlFault(Multistatus(root + ResponseXml(href))));
    std::vector<WebDavResource> resources(1);
    ASSERT_EQ(transport.List(resources), VXCORE_ERR_INVALID_STATE);
    ASSERT_TRUE(resources.empty());
  }
  const auto valid_child = ResponseXml("/notebook/note");
  const std::vector<std::string> malformed{
      "<q:multistatus xmlns:q=\"DAV:\"><q:response>",
      Multistatus(valid_child),
      Multistatus(root + valid_child + valid_child),
      Multistatus(root + ResponseXml("/notebook/note", false, "", "404 Not Found")),
      Multistatus(root +
                  ResponseXml("/notebook/note", false, "<q:getetag>&quot;other&quot;</q:getetag>")),
      "<!DOCTYPE q:multistatus [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>" + Multistatus(root),
      "<q:multistatus xmlns:q=\"not-DAV\">" + root + "</q:multistatus>",
      Multistatus(root) + "<second/>"};
  for (const auto &body : malformed) {
    ASSERT_TRUE(XmlFault(body));
    std::vector<WebDavResource> resources;
    ASSERT_EQ(transport.List(resources), VXCORE_ERR_INVALID_STATE);
    ASSERT_TRUE(resources.empty());
  }
#if defined(_WIN32) || defined(__APPLE__)
  ASSERT_TRUE(
      XmlFault(Multistatus(root + ResponseXml("/notebook/Name") + ResponseXml("/notebook/name"))));
  std::vector<WebDavResource> aliases;
  ASSERT_EQ(transport.List(aliases), VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(aliases.empty());
#endif
#ifdef __APPLE__
  ASSERT_TRUE(XmlFault(
      Multistatus(root + ResponseXml("/notebook/%C3%A9") + ResponseXml("/notebook/e%CC%81"))));
  ASSERT_EQ(transport.List(aliases), VXCORE_ERR_INVALID_STATE);
#endif
  return 0;
}

int TestEtagsAndAtomicDownload() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "file"}, {"text", "new content"}}));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  WebDavResource file;
  ASSERT_EQ(transport.Stat("file", file), VXCORE_OK);
  const auto etag = file.etag;
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("download", "original local bytes"));
  ASSERT_EQ(transport.Download("file", "\"stale\"", directory.path, "download"),
            VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(directory.Read("download"), "original local bytes");
  for (const auto *mode : {"weak", "missing"}) {
    ASSERT_TRUE(Control({{"action", "configure"}, {"etag_mode", mode}}));
    std::vector<WebDavResource> resources;
    ASSERT_EQ(transport.List(resources), VXCORE_ERR_UNSUPPORTED);
    ASSERT_TRUE(resources.empty());
    ASSERT_EQ(transport.Download("file", etag, directory.path, "download"), VXCORE_ERR_UNSUPPORTED);
    ASSERT_EQ(directory.Read("download"), "original local bytes");
  }
  ASSERT_TRUE(Control({{"action", "configure"}, {"etag_mode", "strong"}}));
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "GET"},
                       {"path", "file"},
                       {"effect", "drop"},
                       {"phase", "after_headers"}}));
  ASSERT_EQ(transport.Download("file", etag, directory.path, "download"), VXCORE_ERR_SYNC_NETWORK);
  ASSERT_EQ(directory.Read("download"), "original local bytes");
  return 0;
}

int TestXmlAndDepthBounds() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "fault"},
                       {"method", "PROPFIND"},
                       {"path", ""},
                       {"effect", "xml"},
                       {"size", 16 * 1024 * 1024 + 1},
                       {"repeat_base64", "IA=="}}));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  std::vector<WebDavResource> resources(1);
  ASSERT_EQ(transport.List(resources), VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(resources.empty());
  std::string deep;
  for (size_t i = 0; i < 257; ++i) {
    if (!deep.empty()) deep += '/';
    deep += 'd';
  }
  ASSERT_TRUE(Control({{"action", "mkdir"}, {"path", deep}, {"parents", true}}));
  ASSERT_EQ(transport.List(resources), VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(resources.empty());
  ASSERT_TRUE(Control({{"action", "reset"}}));
  for (size_t i = 0; i < 5; ++i) {
    const auto path = "d" + std::to_string(i);
    ASSERT_TRUE(Control({{"action", "mkdir"}, {"path", path}}));
    ASSERT_TRUE(Control({{"action", "fault"},
                         {"method", "PROPFIND"},
                         {"path", path},
                         {"effect", "xml"},
                         {"resource_count", 50000}}));
  }
  ASSERT_EQ(transport.List(resources), VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(resources.empty());
  return 0;
}

int TestHttpErrorMapping() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  const std::vector<std::pair<int, VxCoreError>> cases{{403, VXCORE_ERR_PERMISSION_DENIED},
                                                       {404, VXCORE_ERR_NOT_FOUND},
                                                       {405, VXCORE_ERR_UNSUPPORTED},
                                                       {412, VXCORE_ERR_SYNC_CONFLICT},
                                                       {423, VXCORE_ERR_SYNC_IN_PROGRESS},
                                                       {429, VXCORE_ERR_SYNC_NETWORK},
                                                       {500, VXCORE_ERR_SYNC_NETWORK},
                                                       {501, VXCORE_ERR_UNSUPPORTED},
                                                       {507, VXCORE_ERR_IO}};
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  for (const auto &test : cases) {
    ASSERT_TRUE(Control({{"action", "fault"},
                         {"method", "PROPFIND"},
                         {"path", ""},
                         {"effect", "status"},
                         {"status", test.first},
                         {"body", "server-private-response-should-not-be-reported"}}));
    WebDavResource root;
    ASSERT_EQ(transport.Stat("", root), test.second);
    ASSERT_EQ(transport.LastResponse().http_status, static_cast<long>(test.first));
    ASSERT_TRUE(transport.LastError().find("server-private") == std::string::npos);
  }
  return 0;
}

int TestStreamingAndCancellation() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "configure"}, {"auth", "digest"}}));
  LocalDirectory directory;
  constexpr size_t chunks = 129;
  std::array<char, 64 * 1024> chunk{};
  for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>(i % 251);
  std::ofstream file(directory.path / "attachment", std::ios::binary);
  crypto_hash_sha256_state hash;
  crypto_hash_sha256_init(&hash);
  for (size_t i = 0; i < chunks; ++i) {
    file.write(chunk.data(), chunk.size());
    crypto_hash_sha256_update(&hash, reinterpret_cast<const unsigned char *>(chunk.data()),
                              chunk.size());
  }
  file.close();
  ASSERT_TRUE(file.good());
  std::array<unsigned char, 32> digest{};
  std::array<char, 65> hex{};
  crypto_hash_sha256_final(&hash, digest.data());
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  ASSERT_EQ(transport.Upload("binary", directory.path, "attachment", ""), VXCORE_OK);
  ASSERT_EQ(transport.LastResponse().sha256, std::string(hex.data()));
  ASSERT_EQ(transport.LastResponse().bytes, uint64_t(chunks * chunk.size()));
  const auto etag = transport.LastResponse().etag;
  Json remote;
  ASSERT_TRUE(Control({{"action", "inspect"}, {"path", "binary"}}, &remote));
  ASSERT_EQ(remote["sha256"].get<std::string>(), std::string(hex.data()));
  ASSERT_EQ(transport.Download("binary", etag, directory.path, "download"), VXCORE_OK);
  ASSERT_EQ(transport.LastResponse().sha256, std::string(hex.data()));
  ASSERT_EQ(std::filesystem::file_size(directory.path / "download"), chunks * chunk.size());
  ASSERT_TRUE(directory.Write("preserved", "keep this"));
  auto token = std::make_shared<SyncCancellation>();
  WebDavTransport cancelled(RootUrl(), Credentials(), token);
  ASSERT_TRUE(Control({{"action", "barrier"},
                       {"id", "cancel-get"},
                       {"method", "GET"},
                       {"path", "binary"},
                       {"phase", "after_headers"}}));
  auto future = std::async(std::launch::async, [&] {
    return cancelled.Download("binary", etag, directory.path, "preserved");
  });
  Json reached;
  const bool waiting =
      Control({{"action", "wait"}, {"id", "cancel-get"}, {"timeout_ms", 5000}}, &reached);
  const auto start = std::chrono::steady_clock::now();
  token->Cancel();
  const auto ready = future.wait_for(std::chrono::seconds(2));
  const bool released = Control({{"action", "release"}, {"id", "cancel-get"}});
  const auto result = future.get();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(waiting && reached.value("reached", false) && released);
  ASSERT_TRUE(ready == std::future_status::ready);
  ASSERT_EQ(result, VXCORE_ERR_CANCELLED);
  ASSERT_TRUE(elapsed < std::chrono::seconds(2));
  ASSERT_EQ(directory.Read("preserved"), "keep this");
  return 0;
}

int TestUnsafeLocalPathsAndUrls() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  for (const auto *url :
       {"http://example.com/notebook/", "http://localhost/notebook/",
        "https://user:password@example.com/notebook/", "file:///notebook/",
        "https://example.com/notebook/?query", "https://example.com/notebook/#fragment",
        "https://example.com/notebook/%2e%2e/", "https://example.com/notebook/%2F"}) {
    WebDavTransport invalid(url, Credentials(), nullptr);
    ASSERT_EQ(invalid.Initialize(), VXCORE_ERR_INVALID_PARAM);
  }
  WebDavTransport canonical("HTTPS://EXAMPLE.COM:443/folder%20space", Credentials(), nullptr);
  ASSERT_EQ(canonical.Initialize(), VXCORE_OK);
  ASSERT_EQ(canonical.CanonicalRoot(), "https://example.com/folder%20space/");
  LocalDirectory directory, outside;
  ASSERT_TRUE(directory.Write("regular", "safe local content"));
  std::filesystem::path resolved;
  for (const auto *path :
       {"../escape", "/absolute", "C:/drive", "a\\b", "a/../b", "a//b", "NUL", "name."})
    ASSERT_EQ(WebDavTransport::ResolveLocalPath(directory.path, path, resolved),
              VXCORE_ERR_INVALID_PARAM);
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside.path, directory.path / "link", ec);
  if (!ec) {
    ASSERT_EQ(WebDavTransport::ResolveLocalPath(directory.path, "link/escape", resolved),
              VXCORE_ERR_UNSUPPORTED);
    ASSERT_FALSE(std::filesystem::exists(outside.path / "escape"));
  } else {
    std::cout << "Local symlink creation unavailable; symlink scenario not exercised\n";
  }
#if defined(_WIN32) || defined(__APPLE__)
  ASSERT_EQ(WebDavTransport::ResolveLocalPath(directory.path, "REGULAR", resolved),
            VXCORE_ERR_UNSUPPORTED);
#endif
  return 0;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  // Internal direct-compile sources have their own ConfigManager static state on Windows.
  vxcore::ConfigManager::SetTestMode(true);
  if (RootUrl().empty() || Environment("VXCORE_WEBDAV_TEST_CONTROL_TOKEN").empty()) {
    std::cerr << "Run via run_webdav_test.py -- test_webdav_transport\n";
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || sodium_init() < 0) return 1;
  RUN_TEST(TestAuthenticationAndTls);
  RUN_TEST(TestNamesNamespacesAndCompleteListing);
  RUN_TEST(TestConditionalFileOperations);
  RUN_TEST(TestMutationRaceAndLostAcknowledgement);
  RUN_TEST(TestReadRedirectsAndWriteRefusal);
  RUN_TEST(TestHostileXmlAndHrefs);
  RUN_TEST(TestEtagsAndAtomicDownload);
  RUN_TEST(TestXmlAndDepthBounds);
  RUN_TEST(TestHttpErrorMapping);
  RUN_TEST(TestStreamingAndCancellation);
  RUN_TEST(TestUnsafeLocalPathsAndUrls);
  std::cout << "WebDAV transport protocol checks passed\n";
  return 0;
}
