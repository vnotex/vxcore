#include "jianguoyun_transport.h"

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

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <new>
#include <pugixml.hpp>
#include <string_view>
#include <utility>

#include "core/config_manager.h"
#include "sync/webdav/webdav_transport.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace {
constexpr size_t kXmlLimit = 1024 * 1024;
constexpr uint64_t kChunkLimit = 16 * 1024 * 1024;
constexpr size_t kBytesLimit = 128 * 1024 * 1024;
constexpr uint64_t kRetryAfterLimit = 24 * 60 * 60;
constexpr std::string_view kNamespace = ".vnote-sync";
constexpr std::string_view kStaging = ".vnote-sync/staging/";
constexpr std::string_view kHead = ".vnote-sync/head.json";
constexpr std::string_view kObjects = ".vnote-sync/objects/";
constexpr std::string_view kCommits = ".vnote-sync/commits/";
constexpr char kProperties[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getetag/>"
    "<d:getcontentlength/></d:prop></d:propfind>";

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

bool HashName(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool SafePath(const std::string &path, bool root = false) {
  return WebDavTransport::ValidateRelativePath(path, root) == VXCORE_OK;
}

bool StagingPath(const std::string &path) {
  if (!SafePath(path) || !StartsWith(path, kStaging)) return false;
  const std::string_view suffix(path.data() + kStaging.size(), path.size() - kStaging.size());
  const auto slash = suffix.find('/');
  return slash != std::string_view::npos && suffix.find('/', slash + 1) == std::string_view::npos;
}

bool CreateDestination(const std::string &path) {
  if (StagingPath(path) || path == kHead) return true;
  if (!SafePath(path)) return false;
  const std::string_view value(path);
  if (StartsWith(value, kCommits)) {
    const auto name = value.substr(kCommits.size());
    return name.size() == 69 && name.substr(64) == ".json" && HashName(name.substr(0, 64));
  }
  if (StartsWith(value, kObjects)) {
    const auto name = value.substr(kObjects.size());
    return name.size() == 67 && name[2] == '/' && HashName(name.substr(3)) &&
           name.substr(0, 2) == name.substr(3, 2);
  }
  return false;
}

bool CollectionPath(const std::string &path) {
  return SafePath(path) &&
         (path == kNamespace || (StartsWith(path, kNamespace) && path[kNamespace.size()] == '/'));
}

bool ParseCollectionUrl(const std::string &text, sync_http::Url &url) {
  if (!sync_http::ParseUrl(text, url) || url.scheme != "https") return false;
  if (url.path.back() != '/') url.path += '/';
  return SafePath(url.path.substr(1, url.path.size() - 2), true);
}

bool Loopback(const sync_http::Url &url) {
  return url.host == "localhost" || url.host == "127.0.0.1" || url.host == "[::1]";
}

bool DavNode(const pugi::xml_node &node, const char *local_name) {
  if (node.type() != pugi::node_element) return false;
  const std::string name = node.name();
  const auto colon = name.find(':');
  if (name.substr(colon == std::string::npos ? 0 : colon + 1) != local_name) return false;
  const auto attribute = colon == std::string::npos ? "xmlns" : "xmlns:" + name.substr(0, colon);
  for (auto parent = node; parent; parent = parent.parent()) {
    const auto declaration = parent.attribute(attribute.c_str());
    if (declaration) return std::strcmp(declaration.value(), "DAV:") == 0;
  }
  return false;
}

bool TextOnly(const pugi::xml_node &node, std::string &text) {
  text.clear();
  for (const auto &child : node.children()) {
    if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) return false;
    text += child.value();
  }
  return true;
}

VxCoreError HttpError(long status) {
  if (status == 401) return VXCORE_ERR_SYNC_AUTH_FAILED;
  if (status == 403) return VXCORE_ERR_PERMISSION_DENIED;
  if (status == 404 || status == 410) return VXCORE_ERR_NOT_FOUND;
  if (status == 405 || status == 428 || status == 501) return VXCORE_ERR_UNSUPPORTED;
  if (status == 409 || status == 412) return VXCORE_ERR_SYNC_CONFLICT;
  if (status == 423) return VXCORE_ERR_SYNC_IN_PROGRESS;
  if (status == 507) return VXCORE_ERR_IO;
  if (status == 429 || status >= 500) return VXCORE_ERR_SYNC_NETWORK;
  if (status >= 300 && status < 400) return VXCORE_ERR_UNSUPPORTED;
  return VXCORE_ERR_INVALID_STATE;
}

const char *ErrorText(VxCoreError error) {
  switch (error) {
    case VXCORE_OK:
      return "";
    case VXCORE_ERR_CANCELLED:
      return "Jianguoyun operation cancelled.";
    case VXCORE_ERR_INVALID_PARAM:
      return "Invalid Jianguoyun endpoint, managed path or raw validator.";
    case VXCORE_ERR_SYNC_AUTH_FAILED:
      return "Jianguoyun authentication failed. Check the app password.";
    case VXCORE_ERR_PERMISSION_DENIED:
      return "Jianguoyun collection access was denied.";
    case VXCORE_ERR_NOT_FOUND:
      return "The expected Jianguoyun resource is missing.";
    case VXCORE_ERR_SYNC_CONFLICT:
      return "Jianguoyun publication conflicted. Revalidate the current resource.";
    case VXCORE_ERR_SYNC_IN_PROGRESS:
      return "A Jianguoyun resource or local snapshot changed during transfer.";
    case VXCORE_ERR_SYNC_NETWORK:
      return "Jianguoyun transfer failed. Check the network, TLS and server availability.";
    case VXCORE_ERR_UNSUPPORTED:
      return "The Jianguoyun resource or response does not support safe managed synchronization.";
    case VXCORE_ERR_IO:
      return "Jianguoyun transfer could not read or write storage.";
    case VXCORE_ERR_OUT_OF_MEMORY:
      return "Jianguoyun operation could not allocate memory.";
    default:
      return "The Jianguoyun response is invalid, unsafe or incomplete.";
  }
}

bool RetryAfterSeconds(const SyncHttpResponse &response, uint64_t &seconds) {
  size_t count = 0;
  for (const auto &header : response.headers)
    if (header.first == "retry-after") ++count;
  const auto &value = response.retry_after;
  if (count != 1 || value.empty() || value.size() > 128) return false;
  if (std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
    seconds = 0;
    for (char ch : value)
      seconds = std::min(kRetryAfterLimit, seconds * 10 + static_cast<unsigned>(ch - '0'));
    return true;
  }
  const auto date = curl_getdate(value.c_str(), nullptr);
  const auto now = std::time(nullptr);
  if (date < 0 || now < 0) return false;
  const auto delay = std::difftime(date, now);
  seconds = delay <= 0 ? 0 : static_cast<uint64_t>(std::min<double>(delay, kRetryAfterLimit));
  return true;
}

VxCoreError HashRange(std::ifstream &input, uint64_t offset, uint64_t size,
                      const SyncCancellationPtr &cancellation, std::string &out) {
  out.clear();
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input) return VXCORE_ERR_IO;
  crypto_hash_sha256_state hash;
  crypto_hash_sha256_init(&hash);
  std::array<char, 64 * 1024> buffer{};
  for (uint64_t remaining = size; remaining;) {
    if (cancellation && cancellation->IsCancelled()) return VXCORE_ERR_CANCELLED;
    const auto length = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    input.read(buffer.data(), static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length) || !input) return VXCORE_ERR_IO;
    crypto_hash_sha256_update(&hash, reinterpret_cast<const unsigned char *>(buffer.data()),
                              length);
    remaining -= length;
  }
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  crypto_hash_sha256_final(&hash, digest.data());
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  out = hex.data();
  return VXCORE_OK;
}
}  // namespace

struct JianguoyunTransport::Impl {
  Impl(const std::string &url, const SyncCredentials &credentials, SyncCancellationPtr token)
      : configured_url(url), http(url, credentials, token), cancellation(std::move(token)) {}

  std::string configured_url;
  SyncHttpClient http;
  SyncCancellationPtr cancellation;
  std::string canonical_root;
  std::string error_text;
  SyncHttpResponse empty_response;
  bool initialized = false;
  bool production = false;
  bool response_current = false;

  bool Cancelled() const { return cancellation && cancellation->IsCancelled(); }

  VxCoreError Finish(VxCoreError error, const char *detail = nullptr) noexcept {
    try {
      error_text = error == VXCORE_OK ? "" : (detail ? detail : ErrorText(error));
    } catch (...) {
      error_text.clear();
    }
    return error;
  }

  const SyncHttpResponse &Response() const noexcept {
    return response_current ? http.LastResponse() : empty_response;
  }

  VxCoreError FinishHttp(const char *method, long status) {
    const auto error = HttpError(status);
    std::string detail = std::string(method) + " returned HTTP " + std::to_string(status) + ". ";
    if (status == 429 || status == 507) {
      detail += "Jianguoyun quota or storage limit reached; local work is retained.";
      uint64_t seconds = 0;
      if (RetryAfterSeconds(Response(), seconds))
        detail += " Retry-After seconds: " + std::to_string(seconds) + ".";
    } else {
      detail += ErrorText(error);
    }
    return Finish(error, detail.c_str());
  }

  VxCoreError Initialize() {
    if (Cancelled()) return Finish(VXCORE_ERR_CANCELLED);
    // A fixture session must not survive disabling the authoritative test-mode seam.
    if (!initialized || !production) {
      std::string canonical;
      const auto error = JianguoyunTransport::CanonicalizeUrl(configured_url, canonical);
      if (error != VXCORE_OK) {
        canonical_root.clear();
        return Finish(error);
      }
      canonical_root = std::move(canonical);
    }
    if (initialized) return VXCORE_OK;
    const auto error = http.Initialize();
    if (error != VXCORE_OK) return Finish(error);
    production = http.Root().origin == "https://dav.jianguoyun.com";
    initialized = true;
    return VXCORE_OK;
  }

  template <typename Function>
  VxCoreError Run(Function &&function) noexcept {
    response_current = false;
    error_text.clear();
    try {
      auto error = Initialize();
      if (error == VXCORE_OK) error = function();
      if (error != VXCORE_OK && error_text.empty()) Finish(error);
      return error;
    } catch (const std::bad_alloc &) {
      return Finish(VXCORE_ERR_OUT_OF_MEMORY);
    } catch (...) {
      return Finish(VXCORE_ERR_IO);
    }
  }

  VxCoreError Request(const char *method, const std::string &path,
                      const std::vector<std::string> &headers = {},
                      const SyncHttpRequest &request = {}) {
    const auto error = http.Request(method, path, headers, request);
    response_current = true;
    if (error == VXCORE_OK) return error;
    if (error != VXCORE_ERR_CANCELLED && Response().http_status >= 400)
      return FinishHttp(method, Response().http_status);
    const auto &detail = http.LastErrorDetail();
    return Finish(error, detail.empty() ? nullptr : detail.c_str());
  }

  bool ExactResponsePath(const std::string &path) const {
    sync_http::Url resolved;
    return sync_http::ResolveUrl(http.Root(), Response().effective_url, Response().effective_url,
                                 resolved) &&
           sync_http::Relative(http.Root(), resolved) == path;
  }

  VxCoreError ParseStat(const std::string &path, JianguoyunResource &out) {
    const auto &xml = Response().body;
    if (!ExactResponsePath(path) || !sync_http::IsUtf8(xml) ||
        xml.find('\0') != std::string::npos || xml.find("<!DOCTYPE") != std::string::npos ||
        xml.find("<!ENTITY") != std::string::npos)
      return Finish(VXCORE_ERR_INVALID_STATE);
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_declaration,
                              pugi::encoding_utf8))
      return Finish(VXCORE_ERR_INVALID_STATE);
    const auto multistatus = document.document_element();
    if (!DavNode(multistatus, "multistatus")) return Finish(VXCORE_ERR_INVALID_STATE);
    size_t roots = 0;
    for (const auto &node : document.children())
      if (node.type() == pugi::node_element) ++roots;
    if (roots != 1) return Finish(VXCORE_ERR_INVALID_STATE);
    pugi::xml_node response;
    for (const auto &node : multistatus.children()) {
      if (node.type() != pugi::node_element) continue;
      if (response || !DavNode(node, "response")) return Finish(VXCORE_ERR_INVALID_STATE);
      response = node;
    }
    if (!response) return Finish(VXCORE_ERR_INVALID_STATE);
    JianguoyunResource resource;
    std::string href;
    bool have_href = false, have_type = false, have_length = false, have_status = false;
    uint8_t seen = 0;
    for (const auto &child : response.children()) {
      if (Cancelled()) return Finish(VXCORE_ERR_CANCELLED);
      if (child.type() != pugi::node_element) continue;
      if (DavNode(child, "href")) {
        if (have_href || !TextOnly(child, href)) return Finish(VXCORE_ERR_INVALID_STATE);
        have_href = true;
      } else if (DavNode(child, "status")) {
        std::string status;
        if (have_status || !TextOnly(child, status) || sync_http::ParseStatus(status) != 200)
          return Finish(VXCORE_ERR_INVALID_STATE);
        have_status = true;
      } else if (DavNode(child, "propstat")) {
        pugi::xml_node properties;
        long status = 0;
        for (const auto &part : child.children()) {
          if (part.type() != pugi::node_element) continue;
          if (DavNode(part, "prop")) {
            if (properties) return Finish(VXCORE_ERR_INVALID_STATE);
            properties = part;
          } else if (DavNode(part, "status")) {
            std::string text;
            if (status || !TextOnly(part, text)) return Finish(VXCORE_ERR_INVALID_STATE);
            status = sync_http::ParseStatus(text);
            if (status != 200 && status != 404) return Finish(VXCORE_ERR_INVALID_STATE);
          } else if (!DavNode(part, "responsedescription")) {
            return Finish(VXCORE_ERR_INVALID_STATE);
          }
        }
        if (!properties || !status) return Finish(VXCORE_ERR_INVALID_STATE);
        for (const auto &property : properties.children()) {
          if (property.type() != pugi::node_element) continue;
          const uint8_t bit = DavNode(property, "resourcetype")       ? 1
                              : DavNode(property, "getetag")          ? 2
                              : DavNode(property, "getcontentlength") ? 4
                                                                      : 0;
          if (seen & bit) return Finish(VXCORE_ERR_INVALID_STATE);
          seen |= bit;
          if (status == 404) continue;
          if (bit == 1) {
            have_type = true;
            for (const auto &type : property.children()) {
              if (type.type() == pugi::node_comment) continue;
              if (!DavNode(type, "collection") || resource.collection || type.first_child())
                return Finish(VXCORE_ERR_UNSUPPORTED);
              resource.collection = true;
            }
          } else if (bit == 2) {
            if (!TextOnly(property, resource.etag)) return Finish(VXCORE_ERR_INVALID_STATE);
          } else if (bit == 4) {
            std::string text;
            if (!TextOnly(property, text) || !sync_http::ParseUnsigned(text, resource.size))
              return Finish(VXCORE_ERR_INVALID_STATE);
            have_length = true;
          }
        }
      } else if (!DavNode(child, "responsedescription")) {
        return Finish(VXCORE_ERR_INVALID_STATE);
      }
    }
    sync_http::Url resolved;
    if (!have_href || !have_type ||
        !sync_http::ResolveUrl(http.Root(), Response().effective_url, href, resolved) ||
        sync_http::Relative(http.Root(), resolved) != path)
      return Finish(VXCORE_ERR_INVALID_STATE);
    if (!resource.collection) {
      if (path.empty() || resolved.path.back() == '/' || !have_length)
        return Finish(VXCORE_ERR_INVALID_STATE);
      if (!resource.etag.empty() && !JianguoyunTransport::IsRawEtag(resource.etag))
        return Finish(VXCORE_ERR_UNSUPPORTED);
    } else {
      resource.size = 0;
      resource.etag.clear();
    }
    out = std::move(resource);
    return Finish(VXCORE_OK);
  }

  VxCoreError Stat(const std::string &path, JianguoyunResource &out) {
    if (!SafePath(path, true)) return Finish(VXCORE_ERR_INVALID_PARAM);
    SyncHttpRequest request;
    request.collection = path.empty();
    request.body_status = 207;
    request.body_limit = kXmlLimit;
    request.body = kProperties;
    auto error = Request("PROPFIND", path,
                         {"Depth: 0", "Content-Type: application/xml; charset=utf-8"}, request);
    if (error != VXCORE_OK) return error;
    if (Response().http_status != 207) return FinishHttp("PROPFIND", Response().http_status);
    return ParseStat(path, out);
  }
};

JianguoyunTransport::JianguoyunTransport(const std::string &collection_url,
                                         const SyncCredentials &credentials,
                                         SyncCancellationPtr cancellation) noexcept {
  try {
    impl_ = std::make_unique<Impl>(collection_url, credentials, std::move(cancellation));
  } catch (const std::bad_alloc &) {
    construction_error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    construction_error_ = VXCORE_ERR_IO;
  }
}
JianguoyunTransport::~JianguoyunTransport() = default;

const std::string &JianguoyunTransport::CanonicalRoot() const noexcept {
  static const std::string empty;
  return impl_ ? impl_->canonical_root : empty;
}
const std::string &JianguoyunTransport::LastError() const noexcept {
  static const std::string empty;
  return impl_ ? impl_->error_text : empty;
}
const SyncHttpResponse &JianguoyunTransport::LastResponse() const noexcept {
  static const SyncHttpResponse empty;
  return impl_ ? impl_->Response() : empty;
}
void JianguoyunTransport::SetProgressCallback(
    std::function<void(uint64_t, uint64_t)> callback) noexcept {
  if (!impl_) return;
  try {
    impl_->http.SetProgressCallback(std::move(callback));
  } catch (...) {
    impl_->Finish(VXCORE_ERR_OUT_OF_MEMORY);
  }
}

VxCoreError JianguoyunTransport::CanonicalizeUrl(const std::string &text,
                                                 std::string &out_url) noexcept {
  try {
    sync_http::Url url;
    if (!ParseCollectionUrl(text, url)) {
      out_url.clear();
      return VXCORE_ERR_INVALID_PARAM;
    }
    const bool production = url.origin == "https://dav.jianguoyun.com" &&
                            StartsWith(url.path, "/dav/") && url.path.size() > 5;
    auto canonical = url.String();
    if (!production) {
      const char *configured =
          ConfigManager::IsTestMode() ? std::getenv("VXCORE_WEBDAV_TEST_URL") : nullptr;
      sync_http::Url fixture;
      if (!configured || !Loopback(url) || !ParseCollectionUrl(configured, fixture) ||
          !Loopback(fixture) || canonical != fixture.String()) {
        out_url.clear();
        return VXCORE_ERR_INVALID_PARAM;
      }
    }
    out_url.swap(canonical);
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    out_url.clear();
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    out_url.clear();
    return VXCORE_ERR_INVALID_PARAM;
  }
}

bool JianguoyunTransport::IsRawEtag(const std::string &etag) noexcept {
  if (etag.empty() || etag.size() > 8192 || etag.compare(0, 2, "W/") == 0) return false;
  for (unsigned char ch : etag)
    if (ch <= 0x20 || ch >= 0x7f || ch == '"' || ch == ',' || ch == '*' || ch == '\\') return false;
  return true;
}

VxCoreError JianguoyunTransport::Initialize() noexcept {
  if (!impl_) return construction_error_;
  return impl_->Run([] { return VXCORE_OK; });
}

VxCoreError JianguoyunTransport::Stat(const std::string &path, JianguoyunResource &out) noexcept {
  out = {};
  if (!impl_) return construction_error_;
  return impl_->Run([&] { return impl_->Stat(path, out); });
}

VxCoreError JianguoyunTransport::Get(const std::string &path, std::string &body, std::string &etag,
                                     size_t limit) noexcept {
  body.clear();
  etag.clear();
  if (!impl_) return construction_error_;
  return impl_->Run([&] {
    if (!SafePath(path) || limit > kBytesLimit) return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    SyncHttpRequest request;
    request.body_status = 200;
    request.body_limit = limit;
    auto error = impl_->Request("GET", path, {}, request);
    if (error != VXCORE_OK) return error;
    const auto &response = LastResponse();
    if (response.http_status != 200) return impl_->FinishHttp("GET", response.http_status);
    if (!impl_->ExactResponsePath(path) ||
        (response.length_seen && response.content_length != response.bytes))
      return impl_->Finish(VXCORE_ERR_INVALID_STATE);
    if (!IsRawEtag(response.etag)) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
    std::string bytes = response.body;
    std::string tag = response.etag;
    body.swap(bytes);
    etag.swap(tag);
    return impl_->Finish(VXCORE_OK);
  });
}

VxCoreError JianguoyunTransport::Download(const std::string &path,
                                          const std::filesystem::path &local_root,
                                          const std::string &local_relative_path,
                                          const std::string &expected_sha256,
                                          uint64_t expected_size) noexcept {
  if (!impl_) return construction_error_;
  return impl_->Run([&] {
    if (!SafePath(path) || !HashName(expected_sha256))
      return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    std::filesystem::path destination;
    auto error = WebDavTransport::ResolveLocalPath(local_root, local_relative_path, destination);
    if (error != VXCORE_OK) return impl_->Finish(error);
    AtomicFileWriter writer(destination);
    error = writer.Open();
    if (error != VXCORE_OK) return impl_->Finish(error);
    SyncHttpRequest request;
    request.writer = &writer;
    error = impl_->Request("GET", path, {}, request);
    if (error != VXCORE_OK) return error;
    const auto &response = LastResponse();
    if (response.http_status != 200) return impl_->FinishHttp("GET", response.http_status);
    if (!impl_->ExactResponsePath(path) || response.bytes != expected_size ||
        response.sha256 != expected_sha256 ||
        (response.length_seen && response.content_length != response.bytes))
      return impl_->Finish(VXCORE_ERR_INVALID_STATE);
    std::filesystem::path rechecked;
    error = WebDavTransport::ResolveLocalPath(local_root, local_relative_path, rechecked);
    if (error != VXCORE_OK) return impl_->Finish(error);
    if (destination != rechecked) return impl_->Finish(VXCORE_ERR_SYNC_IN_PROGRESS);
    if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
    return impl_->Finish(writer.Commit());
  });
}

VxCoreError JianguoyunTransport::PutBytes(const std::string &path, const std::string &bytes,
                                          const std::string &expected_etag) noexcept {
  if (!impl_) return construction_error_;
  return impl_->Run([&] {
    if ((!StagingPath(path) && path != kHead) || bytes.size() > kBytesLimit ||
        (!expected_etag.empty() && !IsRawEtag(expected_etag)) ||
        (path == kHead && expected_etag.empty()))
      return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    std::vector<std::string> headers{"Content-Type: application/octet-stream",
                                     "Expect: 100-continue"};
    if (!expected_etag.empty()) headers.emplace_back("If-Match: " + expected_etag);
    SyncHttpRequest request;
    request.body = bytes;
    const auto error = impl_->Request("PUT", path, headers, request);
    if (error != VXCORE_OK) return error;
    const auto status = LastResponse().http_status;
    return status == 200 || status == 201 || status == 204 ? impl_->Finish(VXCORE_OK)
                                                           : impl_->FinishHttp("PUT", status);
  });
}

VxCoreError JianguoyunTransport::UploadRange(const std::string &path,
                                             const std::filesystem::path &local_root,
                                             const std::string &local_relative_path,
                                             uint64_t offset, uint64_t size) noexcept {
  if (!impl_) return construction_error_;
  return impl_->Run([&] {
    const auto max_offset = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (!StagingPath(path) || size > kChunkLimit || offset > max_offset ||
        size > max_offset - offset)
      return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    std::filesystem::path source;
    auto error = WebDavTransport::ResolveLocalPath(local_root, local_relative_path, source);
    if (error != VXCORE_OK) return impl_->Finish(error);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) return impl_->Finish(VXCORE_ERR_IO);
    const auto full_size = std::filesystem::file_size(source, ec);
    if (ec) return impl_->Finish(VXCORE_ERR_IO);
    if (offset > full_size || size > full_size - offset)
      return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    const auto time = std::filesystem::last_write_time(source, ec);
    if (ec) return impl_->Finish(VXCORE_ERR_IO);
    std::ifstream input(source, std::ios::binary);
    if (!input) return impl_->Finish(VXCORE_ERR_IO);
    std::string expected_hash;
    error = HashRange(input, offset, size, impl_->cancellation, expected_hash);
    if (error != VXCORE_OK) return impl_->Finish(error);
    SyncHttpRequest request;
    request.input = &input;
    request.upload_offset = offset;
    request.upload_size = size;
    error = impl_->Request(
        "PUT", path, {"Content-Type: application/octet-stream", "Expect: 100-continue"}, request);
    if (error != VXCORE_OK) return error;
    const auto &response = LastResponse();
    if (response.http_status != 200 && response.http_status != 201 && response.http_status != 204)
      return impl_->FinishHttp("PUT", response.http_status);
    std::filesystem::path rechecked;
    error = WebDavTransport::ResolveLocalPath(local_root, local_relative_path, rechecked);
    if (error != VXCORE_OK) return impl_->Finish(error);
    if (source != rechecked || response.bytes != size || response.sha256 != expected_hash ||
        std::filesystem::file_size(source, ec) != full_size || ec ||
        std::filesystem::last_write_time(source, ec) != time || ec)
      return impl_->Finish(VXCORE_ERR_SYNC_IN_PROGRESS);
    if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
    return impl_->Finish(VXCORE_OK);
  });
}

VxCoreError JianguoyunTransport::MoveCreate(const std::string &source,
                                            const std::string &destination) noexcept {
  if (!impl_) return construction_error_;
  return impl_->Run([&] {
    if (!StagingPath(source) || !CreateDestination(destination) || source == destination)
      return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    const auto error = impl_->Request(
        "MOVE", source, {"Destination: " + impl_->http.UrlFor(destination), "Overwrite: F"});
    if (error != VXCORE_OK) return error;
    const auto status = LastResponse().http_status;
    // A create-only MOVE must create a new resource; 204 is an unsafe overwrite response.
    return status == 201 ? impl_->Finish(VXCORE_OK) : impl_->FinishHttp("MOVE", status);
  });
}

VxCoreError JianguoyunTransport::EnsureCollection(const std::string &path) noexcept {
  if (!impl_) return construction_error_;
  return impl_->Run([&] {
    if (!path.empty() && !CollectionPath(path)) return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
    JianguoyunResource resource;
    auto error = impl_->Stat(path, resource);
    if (error == VXCORE_OK)
      return impl_->Finish(resource.collection ? VXCORE_OK : VXCORE_ERR_UNSUPPORTED);
    if (error != VXCORE_ERR_NOT_FOUND || path.empty()) return error;
    SyncHttpRequest request;
    request.collection = true;
    error = impl_->Request("MKCOL", path, {}, request);
    if (error != VXCORE_OK) return error;
    const auto status = LastResponse().http_status;
    if (status != 201 && status != 405) return impl_->FinishHttp("MKCOL", status);
    resource = {};
    error = impl_->Stat(path, resource);
    if (error != VXCORE_OK) return error;
    return impl_->Finish(resource.collection ? VXCORE_OK : VXCORE_ERR_UNSUPPORTED);
  });
}

}  // namespace vxcore
