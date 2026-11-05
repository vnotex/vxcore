#include <curl/curl.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/config_manager.h"
#include "sync/http/sync_http_client.h"
#include "sync/sync_json_keys.h"
#include "sync/webdav/webdav_transport.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"

#ifdef VXCORE_SYNC_TLS_OPENSSL
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

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

#ifndef _WIN32
// A real one-connection loopback peer for framing/hostname cases the DAV fixture does not inject.
// Listener and accepted sockets are owned by the joined worker, with bounded waits on failure.
struct LoopbackPeer {
  std::string url;
  std::future<void> worker;
  explicit LoopbackPeer(std::function<void(int)> serve) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
      close(listener);
      return;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_size) != 0) {
      close(listener);
      return;
    }
    url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/";
    worker = std::async(std::launch::async, [listener, serve = std::move(serve)] {
      struct Socket {
        int value;
        ~Socket() { if (value >= 0) close(value); }
      } listening{listener};
      pollfd descriptor{listener, POLLIN, 0};
      if (poll(&descriptor, 1, 5000) <= 0) return;
      Socket connected{accept(listener, nullptr, nullptr)};
      if (connected.value < 0) return;
      timeval timeout{5, 0};
      if (setsockopt(connected.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
          setsockopt(connected.value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        return;
#ifdef SO_NOSIGPIPE
      int enabled = 1;
      if (setsockopt(connected.value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return;
#endif
      serve(connected.value);
    });
  }
  ~LoopbackPeer() { if (worker.valid()) worker.wait(); }
};

void ServeHttpResponse(int socket, const std::string &response) {
  std::string request;
  std::array<char, 2048> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto received = recv(socket, buffer.data(), buffer.size(), 0);
    if (received <= 0 || request.size() + static_cast<size_t>(received) > 8192) return;
    request.append(buffer.data(), static_cast<size_t>(received));
  }
  size_t sent = 0;
  while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = send(socket, response.data() + sent, response.size() - sent, flags);
    if (count <= 0) return;
    sent += static_cast<size_t>(count);
  }
}
#endif

VxCoreError RejectTls(const char *, const uint8_t *const *, const uint64_t *, uint64_t,
                      void *userdata) {
  ++*static_cast<size_t *>(userdata);
  return VXCORE_ERR_SYNC_NETWORK;
}

int TestTlsPolicyAdmission() {
  size_t calls = 0;
  SyncTlsPolicy policy{&RejectTls, &calls};
  WebDavTransport custom("https://127.0.0.1:1/notebook/", {}, nullptr, policy);
#ifdef VXCORE_SYNC_TLS_OPENSSL
  ASSERT_EQ(custom.Initialize(), VXCORE_OK);
#else
  ASSERT_EQ(custom.Initialize(), VXCORE_ERR_UNSUPPORTED);
  WebDavResource unused;
  ASSERT_EQ(custom.Stat("", unused), VXCORE_ERR_UNSUPPORTED);
  ASSERT_EQ(custom.LastResponse().http_status, 0L);
#endif
  ASSERT_EQ(calls, size_t(0));  // Local setup never invokes trust or opens a connection.
  WebDavTransport plaintext("http://127.0.0.1:1/notebook/", {}, nullptr, policy);
  ASSERT_EQ(plaintext.Initialize(), VXCORE_ERR_INVALID_PARAM);
  WebDavTransport defaults("https://127.0.0.1:1/notebook/", {}, nullptr);
  ASSERT_EQ(defaults.Initialize(), VXCORE_OK);
  return 0;
}

#ifdef VXCORE_SYNC_TLS_OPENSSL
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using PrivateKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

std::vector<uint8_t> CertificateDer(X509 *certificate) {
  const int length = i2d_X509(certificate, nullptr);
  if (length <= 0) return {};
  std::vector<uint8_t> result(static_cast<size_t>(length));
  auto *output = result.data();
  if (i2d_X509(certificate, &output) != length) return {};
  return result;
}

struct PinnedTrust {
  std::string host = "localhost";
  std::vector<std::vector<uint8_t>> certificates;
  size_t calls = 0;
  bool matched = false;
  bool throws = false;
  VxCoreError result = VXCORE_OK;
};

// Explicit test trust pins every DER byte and the intended host; never a trust-all callback.
VxCoreError VerifyPinned(const char *host, const uint8_t *const *der, const uint64_t *lengths,
                         uint64_t count, void *userdata) {
  auto &trust = *static_cast<PinnedTrust *>(userdata);
  ++trust.calls;
  trust.matched = false;
  if (!host || host != trust.host || !der || !lengths || count != trust.certificates.size())
    return VXCORE_ERR_SYNC_NETWORK;
  for (size_t i = 0; i < trust.certificates.size(); ++i) {
    if (!der[i] || lengths[i] != trust.certificates[i].size() ||
        std::memcmp(der[i], trust.certificates[i].data(), trust.certificates[i].size()) != 0)
      return VXCORE_ERR_SYNC_NETWORK;
  }
  trust.matched = true;
  if (trust.throws) throw std::runtime_error("test verifier exception");
  return trust.result;
}

PrivateKey MakeTlsKey() {
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
  EVP_PKEY *key = nullptr;
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) != 1 ||
      EVP_PKEY_keygen(context.get(), &key) != 1)
    return PrivateKey(nullptr, EVP_PKEY_free);
  return PrivateKey(key, EVP_PKEY_free);
}

Certificate MakeTlsCertificate(EVP_PKEY *key, long serial, size_t padding = 0) {
  Certificate certificate(X509_new(), X509_free);
  if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) != 1 ||
      !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
      !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
      X509_set_pubkey(certificate.get(), key) != 1)
    return Certificate(nullptr, X509_free);
  auto *name = X509_get_subject_name(certificate.get());
  if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char *>("localhost"),
                                 -1, -1, 0) != 1 ||
      X509_set_issuer_name(certificate.get(), name) != 1)
    return Certificate(nullptr, X509_free);
  if (padding) {
    std::vector<uint8_t> content(padding, 'x');
    std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)> value(
        ASN1_OCTET_STRING_new(), ASN1_OCTET_STRING_free);
    if (!value || ASN1_OCTET_STRING_set(value.get(), content.data(),
                                       static_cast<int>(content.size())) != 1)
      return Certificate(nullptr, X509_free);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509_EXTENSION_create_by_NID(nullptr, NID_netscape_comment, 0, value.get()),
        X509_EXTENSION_free);
    if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1)
      return Certificate(nullptr, X509_free);
  }
  if (X509_sign(certificate.get(), key, EVP_sha256()) <= 0)
    return Certificate(nullptr, X509_free);
  return certificate;
}

int VerifyChain(X509 *leaf, const std::vector<X509 *> &intermediates, PinnedTrust &trust,
                int &verify_error) {
  std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
  std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context(
      X509_STORE_CTX_new(), X509_STORE_CTX_free);
  struct BorrowedChain {
    STACK_OF(X509) *value = sk_X509_new_null();
    ~BorrowedChain() { sk_X509_free(value); }
  } chain;
  if (!store || !context || !chain.value) return -1;
  for (auto *certificate : intermediates)
    if (!sk_X509_push(chain.value, certificate)) return -1;
  if (X509_STORE_CTX_init(context.get(), store.get(), leaf, chain.value) != 1) return -1;
  const int result = sync_http::VerifyTlsPeer(context.get(), trust.host.c_str(),
                                             SyncTlsPolicy{&VerifyPinned, &trust});
  verify_error = X509_STORE_CTX_get_error(context.get());
  return result;
}

int TestTlsChainExtractionAndBounds() {
  auto key = MakeTlsKey();
  ASSERT_TRUE(key != nullptr);
  std::vector<Certificate> certificates;
  for (long serial = 1; serial <= 17; ++serial) {
    certificates.push_back(MakeTlsCertificate(key.get(), serial));
    ASSERT_TRUE(certificates.back() != nullptr);
  }
  PinnedTrust trust;
  for (size_t i = 0; i < 3; ++i) {
    trust.certificates.push_back(CertificateDer(certificates[i].get()));
    ASSERT_FALSE(trust.certificates.back().empty());
  }
  // The explicit leaf is not taken from the untrusted stack. All duplicates are removed,
  // including separate X509 allocations containing the same intermediate DER.
  Certificate duplicate(X509_dup(certificates[1].get()), X509_free);
  ASSERT_TRUE(duplicate != nullptr);
  std::vector<X509 *> chain{certificates[1].get(), certificates[0].get(),
                            certificates[2].get(), duplicate.get()};
  int verify_error = -1;
  ASSERT_EQ(VerifyChain(certificates[0].get(), chain, trust, verify_error), 1);
  ASSERT_TRUE(trust.matched);
  ASSERT_EQ(verify_error, X509_V_OK);
  for (const auto code : {VXCORE_ERR_SYNC_NETWORK, static_cast<VxCoreError>(1)}) {
    trust.result = code;
    ASSERT_EQ(VerifyChain(certificates[0].get(), chain, trust, verify_error), 0);
    ASSERT_EQ(verify_error, X509_V_ERR_APPLICATION_VERIFICATION);
  }
  trust.result = VXCORE_OK;
  trust.throws = true;
  ASSERT_EQ(VerifyChain(certificates[0].get(), chain, trust, verify_error), 0);
  ASSERT_EQ(verify_error, X509_V_ERR_APPLICATION_VERIFICATION);
  trust.throws = false;
  ASSERT_EQ(sync_http::VerifyTlsPeer(nullptr, "localhost", {&VerifyPinned, &trust}), 0);
  std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> missing(
      X509_STORE_CTX_new(), X509_STORE_CTX_free);
  ASSERT_TRUE(missing != nullptr);
  const auto before_missing = trust.calls;
  ASSERT_EQ(sync_http::VerifyTlsPeer(missing.get(), "localhost", {&VerifyPinned, &trust}), 0);
  ASSERT_EQ(trust.calls, before_missing);

  chain.clear();
  trust.certificates.clear();
  for (size_t i = 0; i < 16; ++i) {
    trust.certificates.push_back(CertificateDer(certificates[i].get()));
    if (i) chain.push_back(certificates[i].get());
  }
  ASSERT_EQ(VerifyChain(certificates[0].get(), chain, trust, verify_error), 1);
  ASSERT_TRUE(trust.matched);
  const auto before_overflow = trust.calls;
  chain.push_back(certificates[16].get());
  ASSERT_EQ(VerifyChain(certificates[0].get(), chain, trust, verify_error), 0);
  ASSERT_EQ(trust.calls, before_overflow);
  ASSERT_EQ(verify_error, X509_V_ERR_APPLICATION_VERIFICATION);

  auto large = MakeTlsCertificate(key.get(), 100, 1024 * 1024 - 4096);
  ASSERT_TRUE(large != nullptr);
  trust.certificates = {CertificateDer(large.get())};
  ASSERT_EQ(VerifyChain(large.get(), {}, trust, verify_error), 1);
  auto oversized = MakeTlsCertificate(key.get(), 101, 1024 * 1024);
  ASSERT_TRUE(oversized != nullptr);
  const auto before_size = trust.calls;
  ASSERT_EQ(VerifyChain(oversized.get(), {}, trust, verify_error), 0);
  ASSERT_EQ(trust.calls, before_size);
  // The one-MiB limit is cumulative, not a per-certificate allowance.
  std::vector<Certificate> padded;
  chain.clear();
  for (long serial = 200; serial < 216; ++serial) {
    padded.push_back(MakeTlsCertificate(key.get(), serial, 64 * 1024));
    ASSERT_TRUE(padded.back() != nullptr);
    if (padded.size() > 1) chain.push_back(padded.back().get());
  }
  ASSERT_EQ(VerifyChain(padded[0].get(), chain, trust, verify_error), 0);
  ASSERT_EQ(trust.calls, before_size);
  ASSERT_EQ(verify_error, X509_V_ERR_APPLICATION_VERIFICATION);
  return 0;
}

int TestCustomTrustPreservesHostnameCheck() {
  auto key = MakeTlsKey();
  ASSERT_TRUE(key != nullptr);
  auto certificate = MakeTlsCertificate(key.get(), 300);
  ASSERT_TRUE(certificate != nullptr);
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_server_method()),
                                                         SSL_CTX_free);
  ASSERT_TRUE(context != nullptr);
  ASSERT_EQ(SSL_CTX_use_certificate(context.get(), certificate.get()), 1);
  ASSERT_EQ(SSL_CTX_use_PrivateKey(context.get(), key.get()), 1);
  LoopbackPeer peer([&](int socket) {
    std::unique_ptr<SSL, decltype(&SSL_free)> session(SSL_new(context.get()), SSL_free);
    if (!session || SSL_set_fd(session.get(), socket) != 1 || SSL_accept(session.get()) != 1)
      return;
    std::array<char, 2048> request{};
    if (SSL_read(session.get(), request.data(), static_cast<int>(request.size())) <= 0) return;
    constexpr char response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    SSL_write(session.get(), response, static_cast<int>(sizeof(response) - 1));
  });
  ASSERT_TRUE(peer.worker.valid());
  PinnedTrust trust;
  trust.host = "127.0.0.1";
  trust.certificates = {CertificateDer(certificate.get())};
  // The verifier explicitly trusts the pinned localhost leaf for this request, but curl's
  // independent IP-name check must still reject it before any HTTP request can succeed.
  WebDavTransport transport("https" + peer.url.substr(4), {}, nullptr, {&VerifyPinned, &trust});
  ASSERT_EQ(transport.Options(), VXCORE_ERR_SYNC_NETWORK);
  ASSERT_TRUE(trust.calls > 0 && trust.matched);
  ASSERT_EQ(transport.LastResponse().http_status, 0L);
  return 0;
}

int TestReplacingTlsPolicy() {
  if (RootUrl().compare(0, 8, "https://") != 0) return 0;
  ASSERT_TRUE(Control({{"action", "reset"}}));
  const auto ca = Environment("VXCORE_WEBDAV_TEST_CA_FILE");
  std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(ca.c_str(), "r"), BIO_free);
  ASSERT_TRUE(file != nullptr);
  Certificate certificate(PEM_read_bio_X509(file.get(), nullptr, nullptr, nullptr), X509_free);
  ASSERT_TRUE(certificate != nullptr);
  PinnedTrust trust;
  sync_http::Url root_url;
  ASSERT_TRUE(sync_http::ParseUrl(RootUrl(), root_url));
  trust.host = root_url.host;
  if (trust.host.front() == '[') trust.host = trust.host.substr(1, trust.host.size() - 2);
  trust.certificates = {CertificateDer(certificate.get())};
  ASSERT_FALSE(trust.certificates.front().empty());
  SyncTlsPolicy policy{&VerifyPinned, &trust};
  WebDavTransport accepted(RootUrl(), Credentials(), nullptr, policy);
  // A session owns its policy snapshot, not a reference to the caller's mutable struct.
  size_t unexpected_calls = 0;
  policy = {&RejectTls, &unexpected_calls};
  // The replacement succeeds without OpenSSL's CA store, even with an invalid test CA path.
  EnvironmentOverride bad_ca("VXCORE_WEBDAV_TEST_CA_FILE", "/nonexistent/vxcore-test-ca.pem");
  WebDavResource root;
  ASSERT_EQ(accepted.Stat("", root), VXCORE_OK);
  ASSERT_TRUE(trust.calls > 0 && trust.matched);
  const auto previous_calls = trust.calls;
  ASSERT_EQ(accepted.Stat("", root), VXCORE_OK);
  ASSERT_TRUE(trust.calls > previous_calls);  // Fixture closes each connection; no resumed trust.
  ASSERT_EQ(unexpected_calls, size_t(0));
  // Even when the desktop CA setting trusts this leaf, only exact VXCORE_OK grants trust.
  bad_ca.Set(ca.c_str());
  for (const auto code : {VXCORE_ERR_SYNC_NETWORK, static_cast<VxCoreError>(1)}) {
    trust.result = code;
    WebDavTransport denied(RootUrl(), Credentials(), nullptr, {&VerifyPinned, &trust});
    ASSERT_EQ(denied.Stat("", root), VXCORE_ERR_SYNC_NETWORK);
    ASSERT_EQ(denied.LastResponse().http_status, 0L);
    ASSERT_TRUE(trust.matched);
  }
  trust.result = VXCORE_OK;
  trust.throws = true;
  WebDavTransport throwing(RootUrl(), Credentials(), nullptr, {&VerifyPinned, &trust});
  ASSERT_EQ(throwing.Stat("", root), VXCORE_ERR_SYNC_NETWORK);
  ASSERT_EQ(throwing.LastResponse().http_status, 0L);
  WebDavTransport unchanged_default(RootUrl(), Credentials(), nullptr);
  ASSERT_EQ(unchanged_default.Stat("", root), VXCORE_OK);
  return 0;
}
#endif

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
  ASSERT_EQ(production_error,
            RootUrl().compare(0, 8, "https://") == 0 ? VXCORE_ERR_SYNC_NETWORK : VXCORE_OK);
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

int TestPropertyStatusHandling() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "note"}, {"text", "x"}}));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  const auto root = ResponseXml("/notebook/", true);
  const auto file = ResponseXml("/notebook/note");
  const std::string collection_type =
      "<q:propstat><q:prop><q:resourcetype><q:collection/></q:resourcetype>"
      "</q:prop><q:status>HTTP/1.1 200 OK</q:status></q:propstat>";
  const std::string collection_metadata =
      "<q:propstat><q:prop><q:resourcetype><q:collection/></q:resourcetype>"
      "<q:getetag>&quot;collection-version&quot;</q:getetag>"
      "<q:getlastmodified>Wed, 21 Oct 2015 07:28:00 GMT</q:getlastmodified>"
      "</q:prop><q:status>HTTP/1.1 200 OK</q:status></q:propstat>";
  const std::string missing_length =
      "<q:propstat><q:prop><q:getcontentlength/></q:prop>"
      "<q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>";
  for (const bool missing_first : {false, true}) {
    const auto collection = "<q:response><q:href>/notebook/</q:href>" +
                            (missing_first ? missing_length + collection_metadata
                                           : collection_metadata + missing_length) +
                            "</q:response>";
    ASSERT_TRUE(XmlFault(Multistatus(collection)));
    WebDavResource resource;
    ASSERT_EQ(transport.Stat("", resource), VXCORE_OK);
    ASSERT_TRUE(resource.kind == WebDavResourceKind::kCollection);
    ASSERT_EQ(resource.etag, "\"collection-version\"");
    ASSERT_EQ(resource.size, uint64_t(0));
    ASSERT_EQ(resource.modified_utc, int64_t(1445412480000));

    ASSERT_TRUE(XmlFault(Multistatus(collection + file)));
    std::vector<WebDavResource> resources;
    ASSERT_EQ(transport.List(resources), VXCORE_OK);
    ASSERT_EQ(resources.size(), size_t(2));
    bool saw_root = false, saw_file = false;
    for (const auto &entry : resources) {
      if (entry.path.empty()) {
        saw_root = true;
        ASSERT_TRUE(entry.kind == WebDavResourceKind::kCollection);
        ASSERT_EQ(entry.etag, "\"collection-version\"");
        ASSERT_EQ(entry.modified_utc, int64_t(1445412480000));
      } else {
        ASSERT_EQ(entry.path, "note");
        saw_file = true;
        ASSERT_TRUE(entry.kind == WebDavResourceKind::kFile);
        ASSERT_EQ(entry.etag, "\"version\"");
        ASSERT_EQ(entry.size, uint64_t(1));
      }
    }
    ASSERT_TRUE(saw_root && saw_file);
  }

  // Failed properties carry plausible values, but none may populate the result.
  const std::string missing_optional =
      "<q:propstat><q:prop><q:getetag>&quot;ignored&quot;</q:getetag>"
      "<q:getcontentlength>123</q:getcontentlength>"
      "<q:getlastmodified>Wed, 21 Oct 2015 07:28:00 GMT</q:getlastmodified>"
      "</q:prop><q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>";
  ASSERT_TRUE(XmlFault(Multistatus("<q:response><q:href>/notebook/</q:href>" + missing_optional +
                                   collection_type + "</q:response>")));
  WebDavResource resource;
  ASSERT_EQ(transport.Stat("", resource), VXCORE_OK);
  ASSERT_TRUE(resource.kind == WebDavResourceKind::kCollection);
  ASSERT_TRUE(resource.etag.empty());
  ASSERT_EQ(resource.size, uint64_t(0));
  ASSERT_EQ(resource.modified_utc, int64_t(0));

  const std::string file_metadata =
      "<q:propstat><q:prop><q:resourcetype/><q:getetag>&quot;version&quot;</q:getetag>"
      "<q:getcontentlength>1</q:getcontentlength></q:prop>"
      "<q:status>HTTP/1.1 200 OK</q:status></q:propstat>";
  const std::string missing_modified =
      "<q:propstat><q:prop><q:getlastmodified>Wed, 21 Oct 2015 07:28:00 GMT</q:getlastmodified>"
      "</q:prop><q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>";
  ASSERT_TRUE(XmlFault(Multistatus("<q:response><q:href>/notebook/note</q:href>" + file_metadata +
                                   missing_modified + "</q:response>"),
                       "note"));
  ASSERT_EQ(transport.Stat("note", resource), VXCORE_OK);
  ASSERT_EQ(resource.path, "note");
  ASSERT_TRUE(resource.kind == WebDavResourceKind::kFile);
  ASSERT_EQ(resource.etag, "\"version\"");
  ASSERT_EQ(resource.size, uint64_t(1));
  ASSERT_EQ(resource.modified_utc, int64_t(0));

  const std::string successful_length =
      "<q:propstat><q:prop><q:getcontentlength>1</q:getcontentlength></q:prop>"
      "<q:status>HTTP/1.1 200 OK</q:status></q:propstat>";
  const std::string missing_aliased_length =
      "<q:propstat xmlns:d=\"DAV:\"><q:prop><d:getcontentlength/></q:prop>"
      "<q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>";
  struct Failure {
    const char *name;
    std::string groups;
    VxCoreError error = VXCORE_ERR_INVALID_STATE;
  };
  const std::vector<Failure> failures{
      {"missing resource type",
       "<q:propstat><q:prop><q:resourcetype><q:collection/></q:resourcetype></q:prop>"
       "<q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>"},
      {"missing file length",
       "<q:propstat><q:prop><q:resourcetype/><q:getetag>&quot;version&quot;</q:getetag>"
       "</q:prop><q:status>HTTP/1.1 200 OK</q:status></q:propstat>" +
           missing_length},
      {"missing file etag",
       "<q:propstat><q:prop><q:resourcetype/><q:getcontentlength>1</q:getcontentlength>"
       "</q:prop><q:status>HTTP/1.1 200 OK</q:status></q:propstat>"
       "<q:propstat><q:prop><q:getetag>&quot;ignored&quot;</q:getetag></q:prop>"
       "<q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>",
       VXCORE_ERR_UNSUPPORTED},
      {"forbidden optional property",
       collection_type + "<q:propstat><q:prop><q:getcontentlength/></q:prop>"
                         "<q:status>HTTP/1.1 403 Forbidden</q:status></q:propstat>"},
      {"failed optional property",
       collection_type + "<q:propstat><q:prop><q:getcontentlength/></q:prop>"
                         "<q:status>HTTP/1.1 500 Internal Server Error</q:status></q:propstat>"},
      {"malformed status", collection_type + "<q:propstat><q:prop><q:getcontentlength/></q:prop>"
                                             "<q:status>404 Not Found</q:status></q:propstat>"},
      {"missing status",
       collection_type + "<q:propstat><q:prop><q:getcontentlength/></q:prop></q:propstat>"},
      {"duplicate status", collection_type +
                               "<q:propstat><q:prop><q:getcontentlength/></q:prop>"
                               "<q:status>HTTP/1.1 404 Not Found</q:status>"
                               "<q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>"},
      {"missing prop",
       collection_type + "<q:propstat><q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>"},
      {"duplicate prop", collection_type +
                             "<q:propstat><q:prop><q:getcontentlength/></q:prop><q:prop/>"
                             "<q:status>HTTP/1.1 404 Not Found</q:status></q:propstat>"},
      {"duplicate successful property", collection_type + successful_length + successful_length},
      {"duplicate missing property", collection_type + missing_length + missing_aliased_length},
      {"successful then missing property",
       collection_type + successful_length + missing_aliased_length},
      {"missing then successful property",
       collection_type + missing_aliased_length + successful_length}};
  for (const auto &failure : failures) {
    std::cout << "  Property status: " << failure.name << '\n';
    ASSERT_TRUE(
        XmlFault(Multistatus(root + file + "<q:response><q:href>/notebook/invalid</q:href>" +
                             failure.groups + "</q:response>")));
    std::vector<WebDavResource> resources(1);
    resources.front().path = "previous-listing";
    ASSERT_EQ(transport.List(resources), failure.error);
    ASSERT_TRUE(resources.empty());
  }
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
  const auto other_scheme_target =
      std::string(RootUrl().compare(0, 8, "https://") == 0 ? "http" : "https") +
      RootUrl().substr(RootUrl().find("://")) + "target";
  for (const auto &location :
       {std::string("https://127.0.0.1:1/stolen"), other_scheme_target, RootUrl() + "../escape",
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

int TestBoundedDownloads() {
  ASSERT_TRUE(Control({{"action", "reset"}}));
  const std::string body = "bounded-content";
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "file"}, {"text", body}}));
  ASSERT_TRUE(Control({{"action", "put"}, {"path", "empty"}, {"text", ""}}));
  WebDavTransport transport(RootUrl(), Credentials(), nullptr);
  WebDavResource file, empty;
  ASSERT_EQ(transport.Stat("file", file), VXCORE_OK);
  ASSERT_EQ(transport.Stat("empty", empty), VXCORE_OK);
  LocalDirectory directory;
  ASSERT_TRUE(directory.Write("download", "preserved"));
  ASSERT_EQ(transport.Download("file", file.etag, directory.path, "download", body.size() - 1,
                               body.size()), VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(transport.LastResponse().bytes, uint64_t(0));  // Reject declared length before body.
  ASSERT_EQ(directory.Read("download"), "preserved");
  ASSERT_EQ(transport.Download("file", file.etag, directory.path, "download", 0, body.size()),
            VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(directory.Read("download"), "preserved");
  // A short body must not atomically replace the destination just because it fits the cap.
  ASSERT_EQ(transport.Download("file", file.etag, directory.path, "download", body.size() + 1,
                               body.size() + 1), VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(transport.LastResponse().bytes, static_cast<uint64_t>(body.size()));
  ASSERT_EQ(directory.Read("download"), "preserved");
  ASSERT_EQ(transport.Download("file", file.etag, directory.path, "download", body.size(),
                               body.size()), VXCORE_OK);
  ASSERT_EQ(directory.Read("download"), body);
  ASSERT_EQ(transport.Download("empty", empty.etag, directory.path, "download", 0, 0), VXCORE_OK);
  ASSERT_TRUE(directory.Read("download").empty());
  // Existing callers retain the default unbounded transfer behavior.
  ASSERT_EQ(transport.Download("file", file.etag, directory.path, "download"), VXCORE_OK);
  ASSERT_EQ(directory.Read("download"), body);
  return 0;
}

#ifndef _WIN32
int TestStreamingDownloadBoundsWithoutLength() {
  for (const bool chunked : {false, true}) {
    const std::string response =
        std::string("HTTP/1.1 200 OK\r\nETag: \"stream\"\r\nConnection: close\r\n") +
        (chunked ? "Transfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n4\r\nefgh\r\n0\r\n\r\n"
                 : "\r\nabcdefgh");
    for (const uint64_t cap : {uint64_t(0), uint64_t(7), uint64_t(8)}) {
      LoopbackPeer peer([&](int socket) { ServeHttpResponse(socket, response); });
      ASSERT_TRUE(peer.worker.valid());
      LocalDirectory directory;
      ASSERT_TRUE(directory.Write("download", "preserved"));
      WebDavTransport transport(peer.url, {}, nullptr);
      ASSERT_EQ(transport.Download("file", "\"stream\"", directory.path, "download", cap, 8),
                cap < 8 ? VXCORE_ERR_INVALID_STATE : VXCORE_OK);
      ASSERT_TRUE(transport.LastResponse().bytes <= cap);
      ASSERT_EQ(directory.Read("download"), cap < 8 ? "preserved" : "abcdefgh");
    }
  }
  return 0;
}
#endif

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
       {"https://user:password@example.com/notebook/", "file:///notebook/",
        "https://example.com/notebook/?query", "https://example.com/notebook/#fragment",
        "https://example.com/notebook/%2e%2e/", "https://example.com/notebook/%2F"}) {
    WebDavTransport invalid(url, Credentials(), nullptr);
    ASSERT_EQ(invalid.Initialize(), VXCORE_ERR_INVALID_PARAM);
  }
  WebDavTransport canonical("HTTPS://EXAMPLE.COM:443/folder%20space", Credentials(), nullptr);
  ASSERT_EQ(canonical.Initialize(), VXCORE_OK);
  ASSERT_EQ(canonical.CanonicalRoot(), "https://example.com/folder%20space/");
  const char *http_urls[][2] = {
      {"HTTP://EXAMPLE.COM:80/folder%20space", "http://example.com/folder%20space/"},
      {"http://example.com:443/notebook/", "http://example.com:443/notebook/"},
      {"http://localhost/notebook/", "http://localhost/notebook/"}};
  for (const auto &url : http_urls) {
    ConfigManager::SetTestMode(false);
    WebDavTransport accepted(url[0], Credentials(), nullptr);
    const auto error = accepted.Initialize();
    ConfigManager::SetTestMode(true);
    ASSERT_EQ(error, VXCORE_OK);
    ASSERT_EQ(accepted.CanonicalRoot(), url[1]);
  }
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
  RUN_TEST(TestTlsPolicyAdmission);
#ifdef VXCORE_SYNC_TLS_OPENSSL
  RUN_TEST(TestTlsChainExtractionAndBounds);
  RUN_TEST(TestReplacingTlsPolicy);
  RUN_TEST(TestCustomTrustPreservesHostnameCheck);
#endif
  RUN_TEST(TestAuthenticationAndTls);
  RUN_TEST(TestNamesNamespacesAndCompleteListing);
  RUN_TEST(TestPropertyStatusHandling);
  RUN_TEST(TestConditionalFileOperations);
  RUN_TEST(TestMutationRaceAndLostAcknowledgement);
  RUN_TEST(TestReadRedirectsAndWriteRefusal);
  RUN_TEST(TestHostileXmlAndHrefs);
  RUN_TEST(TestEtagsAndAtomicDownload);
  RUN_TEST(TestBoundedDownloads);
#ifndef _WIN32
  RUN_TEST(TestStreamingDownloadBoundsWithoutLength);
#endif
  RUN_TEST(TestXmlAndDepthBounds);
  RUN_TEST(TestHttpErrorMapping);
  RUN_TEST(TestStreamingAndCancellation);
  RUN_TEST(TestUnsafeLocalPathsAndUrls);
  std::cout << "WebDAV transport protocol checks passed\n";
  return 0;
}
