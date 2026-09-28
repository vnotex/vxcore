#include "webdav_transport.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <curl/curl.h>

#include <pugixml.hpp>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "sync/http/sync_http_client.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace {
constexpr size_t kXmlLimit = 16 * 1024 * 1024;
constexpr size_t kResourceLimit = 250000;
constexpr size_t kDepthLimit = 256;
constexpr size_t kUrlLimit = 32768;
constexpr char kProperties[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getetag/>"
    "<d:getcontentlength/><d:getlastmodified/></d:prop></d:propfind>";

std::string LowerAscii(std::string value) {
  for (auto &ch : value)
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return value;
}

bool SafeName(std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.size() > 255 || !sync_http::IsUtf8(name))
    return false;
  for (unsigned char ch : name) {
    if (ch < 32 || ch == 127 || std::strchr("/\\:<>\"|?*", ch)) return false;
  }
  // Portable names: reject Win32 device/ADS/trailing-dot aliases even on POSIX.
  if (name.back() == '.' || name.back() == ' ') return false;
  auto stem = LowerAscii(std::string(name.substr(0, name.find('.'))));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" ||
      stem == "conout$")
    return false;
  if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
      stem[3] >= '1' && stem[3] <= '9')
    return false;
  // Win32 also recognizes superscript 1, 2, 3 as device digits.
  if (stem.size() == 5 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
      static_cast<unsigned char>(stem[3]) == 0xc2 &&
      (static_cast<unsigned char>(stem[4]) == 0xb9 || static_cast<unsigned char>(stem[4]) == 0xb2 ||
       static_cast<unsigned char>(stem[4]) == 0xb3))
    return false;
  return true;
}

std::string NativeKey(const std::string &path) {
#ifdef _WIN32
  const auto wide = PathFromUtf8(path).native();
  const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                                 static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
  if (!size) return {};
  std::wstring mapped(static_cast<size_t>(size), L'\0');
  if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                     static_cast<int>(wide.size()), mapped.data(), size, nullptr, nullptr, 0))
    return {};
  return PathToGenericUtf8(std::filesystem::path(mapped));
#elif defined(__APPLE__)
  auto input =
      CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.data()),
                              static_cast<CFIndex>(path.size()), kCFStringEncodingUTF8, false);
  if (!input) return {};
  auto normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, input);
  CFRelease(input);
  if (!normalized) return {};
  CFStringNormalize(normalized, kCFStringNormalizationFormD);
  CFStringFold(normalized, kCFCompareCaseInsensitive, nullptr);
  const auto capacity =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(normalized), kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<size_t>(capacity), '\0');
  const bool ok = CFStringGetCString(normalized, result.data(), capacity, kCFStringEncodingUTF8);
  CFRelease(normalized);
  if (!ok) return {};
  result.resize(std::strlen(result.c_str()));
  return result;
#else
  return path;
#endif
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
  if (status == 405 || status == 501 || status == 428) return VXCORE_ERR_UNSUPPORTED;
  if (status == 412) return VXCORE_ERR_SYNC_CONFLICT;
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
      return "WebDAV operation cancelled.";
    case VXCORE_ERR_SYNC_AUTH_FAILED:
      return "WebDAV authentication failed. Check the username and app password.";
    case VXCORE_ERR_PERMISSION_DENIED:
      return "WebDAV permission denied. Check collection access permissions.";
    case VXCORE_ERR_NOT_FOUND:
      return "The expected WebDAV resource is missing. Check the collection URL and sync again.";
    case VXCORE_ERR_UNSUPPORTED:
      return "The WebDAV server or path does not support safe synchronization. Check strong ETags, "
             "conditional writes and the collection URL.";
    case VXCORE_ERR_SYNC_CONFLICT:
      return "A WebDAV resource changed. Revalidate it before synchronizing.";
    case VXCORE_ERR_SYNC_IN_PROGRESS:
      return "A WebDAV resource is busy or a local snapshot changed. Sync again when it is stable.";
    case VXCORE_ERR_IO:
      return "WebDAV transfer could not read or write storage. Check free space and permissions.";
    case VXCORE_ERR_SYNC_NETWORK:
      return "WebDAV connection failed. Check network access and the server TLS certificate.";
    case VXCORE_ERR_INVALID_PARAM:
      return "Invalid WebDAV URL, relative path, credentials or ETag. Check the sync settings.";
    case VXCORE_ERR_OUT_OF_MEMORY:
      return "WebDAV operation could not allocate memory.";
    default:
      return "The WebDAV response is invalid, unsafe or incomplete. No partial listing can be "
             "used.";
  }
}
}  // namespace

struct WebDavTransport::Impl {
  explicit Impl(const std::string &url, const SyncCredentials &credentials,
                SyncCancellationPtr token)
      : http(url, credentials, token), cancellation(std::move(token)) {}

  SyncHttpClient http;
  bool initialized = false;
  SyncCancellationPtr cancellation;
  WebDavResponse response;
  std::string error_text;

  VxCoreError Finish(VxCoreError error, const char *detail = nullptr) {
    error_text = error == VXCORE_OK ? "" : (detail ? detail : ErrorText(error));
    return error;
  }

  VxCoreError FinishHttp(const char *method, long status) {
    const auto error = HttpError(status);
    const char *reason = status == 405 ? "The server does not allow this HTTP method."
                         : status == 501 ? "The server does not implement this HTTP method."
                         : status == 428 ? "The server requires a different request precondition."
                                         : ErrorText(error);
    const auto detail = std::string(method) + " returned HTTP " + std::to_string(status) +
                        ". " + reason;
    return Finish(error, detail.c_str());
  }

  VxCoreError FinishEtag(const char *source, const std::string &etag) {
    const char *reason = etag.empty() ? "The server omitted the file ETag."
                         : etag.compare(0, 2, "W/") == 0 ? "The server returned a weak file ETag."
                                                        : "The server returned an invalid file ETag.";
    const auto detail = std::string(source) + " (HTTP " + std::to_string(response.http_status) +
                        "): " + reason + " A strong ETag is required for safe synchronization.";
    return Finish(VXCORE_ERR_UNSUPPORTED, detail.c_str());
  }
  bool Cancelled() const { return cancellation && cancellation->IsCancelled(); }

  VxCoreError FinishTransfer(VxCoreError error) {
    switch (http.LastFailure()) {
      case SyncHttpFailure::kLibrariesUnavailable:
        return Finish(error,
                      "WebDAV requires libcurl 7.64 or newer with asynchronous DNS and "
                      "initialized cryptographic support. Check the application build.");
      case SyncHttpFailure::kOptionUnavailable: {
        const auto detail = "WebDAV " + http.LastErrorDetail();
        return Finish(error, detail.c_str());
      }
      case SyncHttpFailure::kRedirectRejected:
        return Finish(error, http.LastErrorDetail().c_str());
      case SyncHttpFailure::kDiscardLimit:
        if (response.http_status >= 300) error = HttpError(response.http_status);
        break;
      case SyncHttpFailure::kNone:
        break;
    }
    return Finish(error);
  }

  VxCoreError Request(const char *method, const std::string &path,
                      const std::vector<std::string> &headers,
                      const SyncHttpRequest &request = {}) {
    const auto error = http.Request(method, path, headers, request);
    const auto &received = http.LastResponse();
    response = {};
    response.http_status = received.http_status;
    response.effective_url = received.effective_url;
    response.corrected_url = received.corrected_url;
    response.etag = received.etag;
    // Buffered DAV XML is metadata, not a file transfer in the public WebDAV response.
    response.bytes = request.body_status ? 0 : received.bytes;
    return FinishTransfer(error);
  }

  VxCoreError ParseListing(const std::string &xml, const std::string &requested, bool depth_one,
                           std::vector<WebDavResource> &out) {
    out.clear();
    // Do not let encoding autodetection hide declarations behind UTF-16 zero bytes.
    if (!sync_http::IsUtf8(xml) || xml.find('\0') != std::string::npos ||
        xml.find("<!DOCTYPE") != std::string::npos || xml.find("<!ENTITY") != std::string::npos)
      return Finish(VXCORE_ERR_INVALID_STATE);
    pugi::xml_document document;
    const auto parsed = document.load_buffer(
        xml.data(), xml.size(), pugi::parse_default | pugi::parse_declaration, pugi::encoding_utf8);
    if (!parsed) return Finish(VXCORE_ERR_INVALID_STATE);
    const auto multistatus = document.document_element();
    if (!DavNode(multistatus, "multistatus")) return Finish(VXCORE_ERR_INVALID_STATE);
    size_t roots = 0;
    for (const auto &node : document.children())
      if (node.type() == pugi::node_element) ++roots;
    if (roots != 1) return Finish(VXCORE_ERR_INVALID_STATE);
    std::unordered_set<std::string> paths, native_paths;
    bool self_found = false;
    for (const auto &item : multistatus.children()) {
      if (Cancelled()) return Finish(VXCORE_ERR_CANCELLED);
      if (item.type() != pugi::node_element) continue;
      if (!DavNode(item, "response")) return Finish(VXCORE_ERR_INVALID_STATE);
      WebDavResource resource;
      bool have_href = false, have_type = false, have_etag = false, have_length = false;
      bool have_modified = false, have_status = false, have_propstat = false;
      uint8_t seen_properties = 0;
      std::string href;
      for (const auto &child : item.children()) {
        if (child.type() != pugi::node_element) continue;
        if (DavNode(child, "href")) {
          if (have_href || !TextOnly(child, href)) return Finish(VXCORE_ERR_INVALID_STATE);
          have_href = true;
        } else if (DavNode(child, "status")) {
          std::string status;
          if (have_status || !TextOnly(child, status)) return Finish(VXCORE_ERR_INVALID_STATE);
          have_status = true;
          if (sync_http::ParseStatus(status) != 200) return Finish(VXCORE_ERR_INVALID_STATE);
        } else if (DavNode(child, "propstat")) {
          have_propstat = true;
          pugi::xml_node properties;
          bool got_status = false;
          long property_status = 0;
          for (const auto &part : child.children()) {
            if (part.type() != pugi::node_element) continue;
            if (DavNode(part, "status")) {
              std::string status;
              if (got_status || !TextOnly(part, status)) return Finish(VXCORE_ERR_INVALID_STATE);
              property_status = sync_http::ParseStatus(status);
              if (property_status != 200 && property_status != 404)
                return Finish(VXCORE_ERR_INVALID_STATE);
              got_status = true;
            } else if (DavNode(part, "prop")) {
              if (properties) return Finish(VXCORE_ERR_INVALID_STATE);
              properties = part;
            } else if (!DavNode(part, "responsedescription"))
              return Finish(VXCORE_ERR_INVALID_STATE);
          }
          if (!got_status || !properties) return Finish(VXCORE_ERR_INVALID_STATE);
          for (const auto &property : properties.children()) {
            if (property.type() != pugi::node_element) continue;
            const uint8_t property_bit = DavNode(property, "resourcetype")       ? 1
                                         : DavNode(property, "getetag")          ? 2
                                         : DavNode(property, "getcontentlength") ? 4
                                         : DavNode(property, "getlastmodified")  ? 8
                                                                                 : 0;
            if (seen_properties & property_bit) return Finish(VXCORE_ERR_INVALID_STATE);
            seen_properties |= property_bit;
            if (property_status == 404) continue;
            if (property_bit == 1) {
              if (have_type) return Finish(VXCORE_ERR_INVALID_STATE);
              have_type = true;
              bool collection = false;
              for (const auto &type : property.children()) {
                if (type.type() != pugi::node_element) continue;
                if (!DavNode(type, "collection") || collection)
                  return Finish(VXCORE_ERR_UNSUPPORTED);
                collection = true;
              }
              resource.kind =
                  collection ? WebDavResourceKind::kCollection : WebDavResourceKind::kFile;
            } else if (property_bit == 2) {
              if (have_etag || !TextOnly(property, resource.etag))
                return Finish(VXCORE_ERR_INVALID_STATE);
              have_etag = true;
            } else if (property_bit == 4) {
              std::string length;
              if (have_length || !TextOnly(property, length) ||
                  !sync_http::ParseUnsigned(length, resource.size))
                return Finish(VXCORE_ERR_INVALID_STATE);
              have_length = true;
            } else if (property_bit == 8) {
              std::string modified;
              if (have_modified || !TextOnly(property, modified))
                return Finish(VXCORE_ERR_INVALID_STATE);
              have_modified = true;
              const auto value = curl_getdate(modified.c_str(), nullptr);
              if (value < 0) return Finish(VXCORE_ERR_INVALID_STATE);
              resource.modified_utc = static_cast<int64_t>(value) * 1000;
            }
          }
        } else if (!DavNode(child, "responsedescription"))
          return Finish(VXCORE_ERR_INVALID_STATE);
      }
      if (!have_href || !have_type || !have_propstat) return Finish(VXCORE_ERR_INVALID_STATE);
      sync_http::Url resolved;
      if (!sync_http::ResolveUrl(http.Root(), response.effective_url, href, resolved))
        return Finish(VXCORE_ERR_INVALID_STATE);
      resource.path = sync_http::Relative(http.Root(), resolved);
      if (WebDavTransport::ValidateRelativePath(resource.path, true) != VXCORE_OK)
        return Finish(VXCORE_ERR_INVALID_STATE);
      if (resource.kind == WebDavResourceKind::kFile &&
          (resolved.path.back() == '/' || !have_length ||
           !WebDavTransport::IsStrongEtag(resource.etag)))
        return !WebDavTransport::IsStrongEtag(resource.etag)
                   ? FinishEtag("PROPFIND file metadata", resource.etag)
                   : Finish(VXCORE_ERR_INVALID_STATE);
      if (resource.path == requested) {
        if (self_found) return Finish(VXCORE_ERR_INVALID_STATE);
        self_found = true;
      } else {
        if (!depth_one) return Finish(VXCORE_ERR_INVALID_STATE);
        const auto slash = resource.path.rfind('/');
        const auto parent = slash == std::string::npos ? "" : resource.path.substr(0, slash);
        if (parent != requested || resource.path.empty()) return Finish(VXCORE_ERR_INVALID_STATE);
      }
      const auto key = resource.path.empty() ? "/" : NativeKey(resource.path);
      if (key.empty() || !paths.insert(resource.path).second || !native_paths.insert(key).second ||
          out.size() >= kResourceLimit)
        return Finish(VXCORE_ERR_INVALID_STATE);
      out.push_back(std::move(resource));
    }
    if (!self_found) return Finish(VXCORE_ERR_INVALID_STATE);
    return Finish(VXCORE_OK);
  }

  VxCoreError Propfind(const std::string &path, bool depth_one, std::vector<WebDavResource> &out) {
    SyncHttpRequest request;
    request.collection = depth_one || path.empty();
    request.body_status = 207;
    request.body_limit = kXmlLimit;
    request.body = kProperties;
    auto error = Request(
        "PROPFIND", path,
        {depth_one ? "Depth: 1" : "Depth: 0", "Content-Type: application/xml; charset=utf-8"},
        request);
    if (error != VXCORE_OK) return error;
    if (response.http_status != 207) return FinishHttp("PROPFIND", response.http_status);
    if (!response.corrected_url.empty()) {
      sync_http::Url corrected;
      if (!sync_http::ParseUrl(response.effective_url, corrected))
        return Finish(VXCORE_ERR_INVALID_STATE);
      const auto corrected_path = sync_http::Relative(http.Root(), corrected);
      // Discovery reports the corrected URL. Recursive scans cannot silently rebind
      // notebook-relative paths to a different collection.
      if (depth_one && corrected_path != path) return Finish(VXCORE_ERR_UNSUPPORTED);
      return ParseListing(http.LastResponse().body, corrected_path, depth_one, out);
    }
    return ParseListing(http.LastResponse().body, path, depth_one, out);
  }
};

WebDavTransport::WebDavTransport(const std::string &collection_url,
                                 const SyncCredentials &credentials,
                                 SyncCancellationPtr cancellation)
    : impl_(std::make_unique<Impl>(collection_url, credentials, std::move(cancellation))) {}
WebDavTransport::~WebDavTransport() = default;
const std::string &WebDavTransport::CanonicalRoot() const { return impl_->http.CanonicalRoot(); }
const WebDavResponse &WebDavTransport::LastResponse() const { return impl_->response; }
const std::string &WebDavTransport::LastError() const { return impl_->error_text; }
void WebDavTransport::SetProgressCallback(std::function<void(uint64_t, uint64_t)> callback) {
  impl_->http.SetProgressCallback(std::move(callback));
}

VxCoreError WebDavTransport::Initialize() {
  if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
  if (impl_->initialized) return VXCORE_OK;
  const auto error = impl_->http.Initialize();
  if (error == VXCORE_OK) impl_->initialized = true;
  return impl_->FinishTransfer(error);
}

bool WebDavTransport::IsStrongEtag(const std::string &etag) {
  if (etag.size() < 2 || etag.size() > 8192 || etag.front() != '"' || etag.back() != '"')
    return false;
  for (size_t i = 1; i + 1 < etag.size(); ++i) {
    const auto ch = static_cast<unsigned char>(etag[i]);
    if (ch < 0x21 || ch == 0x22 || ch == 0x7f) return false;
  }
  return true;
}

VxCoreError WebDavTransport::ValidateRelativePath(const std::string &path, bool allow_root) {
  if (path.empty()) return allow_root ? VXCORE_OK : VXCORE_ERR_INVALID_PARAM;
  if (path.size() > kUrlLimit || path.front() == '/' || path.back() == '/')
    return VXCORE_ERR_INVALID_PARAM;
  size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    if (!SafeName(std::string_view(path).substr(start, end - start)))
      return VXCORE_ERR_INVALID_PARAM;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return VXCORE_OK;
}

VxCoreError WebDavTransport::ResolveLocalPath(const std::filesystem::path &root,
                                              const std::string &relative_path,
                                              std::filesystem::path &out_path) {
  out_path.clear();
  if (ValidateRelativePath(relative_path) != VXCORE_OK) return VXCORE_ERR_INVALID_PARAM;
  try {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(root, ec);
    if (ec || !std::filesystem::is_directory(absolute, ec) || ec) return VXCORE_ERR_IO;
    auto current = absolute.root_path();
    for (const auto &part : absolute.relative_path()) {
      if (part == "..") return VXCORE_ERR_INVALID_PARAM;
      current /= part;
      const auto state = CheckReparsePoint(PathToGenericUtf8(current));
      if (state != ReparseState::kNo)
        return state == ReparseState::kYes ? VXCORE_ERR_UNSUPPORTED : VXCORE_ERR_IO;
    }
    const auto canonical = std::filesystem::canonical(absolute, ec);
    if (ec) return VXCORE_ERR_IO;
    current = absolute;
    bool missing = false;
    for (const auto &part : PathFromUtf8(relative_path)) {
      const auto parent = current;
      current /= part;
      if (missing) continue;
      const auto status = std::filesystem::symlink_status(current, ec);
      if (ec == std::errc::no_such_file_or_directory ||
          status.type() == std::filesystem::file_type::not_found) {
        ec.clear();
        missing = true;
        continue;
      }
      if (ec) return VXCORE_ERR_IO;
      const auto state = CheckReparsePoint(PathToGenericUtf8(current));
      if (state != ReparseState::kNo)
        return state == ReparseState::kYes ? VXCORE_ERR_UNSUPPORTED : VXCORE_ERR_IO;
      if (!IsPathWithinCanonical(canonical, PathToGenericUtf8(current), false))
        return VXCORE_ERR_UNSUPPORTED;
      // An existing differently-spelled native alias must not be overwritten.
      bool exact = false;
      for (std::filesystem::directory_iterator it(parent, ec), end; !ec && it != end;
           it.increment(ec)) {
        if (PathToGenericUtf8(it->path().filename()) == PathToGenericUtf8(part)) {
          exact = true;
          break;
        }
      }
      if (ec) return VXCORE_ERR_IO;
      if (!exact) return VXCORE_ERR_UNSUPPORTED;
    }
    out_path = current;
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError WebDavTransport::Options() {
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  error = impl_->Request("OPTIONS", "", {});
  if (error != VXCORE_OK) return error;
  return LastResponse().http_status >= 200 && LastResponse().http_status < 300
             ? impl_->Finish(VXCORE_OK)
             : impl_->FinishHttp("OPTIONS", LastResponse().http_status);
}

VxCoreError WebDavTransport::Stat(const std::string &path, WebDavResource &out_resource) {
  out_resource = {};
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (ValidateRelativePath(path, true) != VXCORE_OK) return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  std::vector<WebDavResource> resources;
  error = impl_->Propfind(path, false, resources);
  if (error != VXCORE_OK) return error;
  if (resources.size() != 1) return impl_->Finish(VXCORE_ERR_INVALID_STATE);
  out_resource = std::move(resources.front());
  return VXCORE_OK;
}

VxCoreError WebDavTransport::List(std::vector<WebDavResource> &out_resources) {
  out_resources.clear();
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  std::vector<WebDavResource> complete;
  std::vector<std::pair<std::string, size_t>> pending{{"", 0}};
  std::unordered_map<std::string, WebDavResource> observed;
  std::unordered_set<std::string> native_paths;
  for (size_t index = 0; index < pending.size(); ++index) {
    if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
    const auto current = pending[index];
    std::vector<WebDavResource> children;
    error = impl_->Propfind(current.first, true, children);
    if (error != VXCORE_OK) return error;
    for (auto &resource : children) {
      if (resource.path == current.first) {
        if (resource.kind != WebDavResourceKind::kCollection)
          return impl_->Finish(VXCORE_ERR_INVALID_STATE);
        if (index != 0) {
          const auto &before = observed.at(resource.path);
          if (before.kind != resource.kind || before.etag != resource.etag)
            return impl_->Finish(VXCORE_ERR_SYNC_CONFLICT);
          continue;
        }
      }
      const auto key = resource.path.empty() ? "/" : NativeKey(resource.path);
      if (key.empty() || !native_paths.insert(key).second || observed.count(resource.path) ||
          complete.size() >= kResourceLimit)
        return impl_->Finish(VXCORE_ERR_INVALID_STATE);
      observed.emplace(resource.path, resource);
      if (resource.kind == WebDavResourceKind::kCollection && !resource.path.empty()) {
        if (current.second >= kDepthLimit) return impl_->Finish(VXCORE_ERR_INVALID_STATE);
        pending.emplace_back(resource.path, current.second + 1);
      }
      complete.push_back(std::move(resource));
    }
  }
  out_resources.swap(complete);
  return impl_->Finish(VXCORE_OK);
}

VxCoreError WebDavTransport::Download(const std::string &path, const std::string &expected_etag,
                                      const std::filesystem::path &local_root,
                                      const std::string &local_relative_path) {
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (ValidateRelativePath(path) != VXCORE_OK || !IsStrongEtag(expected_etag))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  std::filesystem::path destination;
  error = ResolveLocalPath(local_root, local_relative_path, destination);
  if (error != VXCORE_OK) return impl_->Finish(error);
  AtomicFileWriter writer(destination);
  error = writer.Open();
  if (error != VXCORE_OK) return impl_->Finish(error);
  SyncHttpRequest request;
  request.writer = &writer;
  error = impl_->Request("GET", path, {"If-Match: " + expected_etag}, request);
  if (error != VXCORE_OK) return error;
  if (LastResponse().http_status != 200)
    return impl_->FinishHttp("GET", LastResponse().http_status);
  const auto &transfer = impl_->http.LastResponse();
  if (!IsStrongEtag(transfer.etag)) return impl_->FinishEtag("GET response", transfer.etag);
  if (transfer.etag != expected_etag) return impl_->Finish(VXCORE_ERR_SYNC_CONFLICT);
  if (transfer.length_seen && transfer.content_length != transfer.bytes)
    return impl_->Finish(VXCORE_ERR_INVALID_STATE);
  impl_->response.sha256 = transfer.sha256;
  std::filesystem::path rechecked;
  error = ResolveLocalPath(local_root, local_relative_path, rechecked);
  if (error != VXCORE_OK) return impl_->Finish(error);
  if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
  return impl_->Finish(writer.Commit());
}

VxCoreError WebDavTransport::Upload(const std::string &path,
                                    const std::filesystem::path &local_root,
                                    const std::string &local_relative_path,
                                    const std::string &expected_etag) {
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (ValidateRelativePath(path) != VXCORE_OK ||
      (!expected_etag.empty() && !IsStrongEtag(expected_etag)))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  std::filesystem::path source;
  error = ResolveLocalPath(local_root, local_relative_path, source);
  if (error != VXCORE_OK) return impl_->Finish(error);
  if (!expected_etag.empty()) {
    WebDavResource existing;
    error = Stat(path, existing);
    if (error != VXCORE_OK) return error;
    if (existing.kind != WebDavResourceKind::kFile) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(source, ec) || ec) return impl_->Finish(VXCORE_ERR_IO);
  const auto size = std::filesystem::file_size(source, ec);
  if (ec || size > static_cast<uint64_t>(std::numeric_limits<curl_off_t>::max()))
    return impl_->Finish(VXCORE_ERR_IO);
  const auto time = std::filesystem::last_write_time(source, ec);
  if (ec) return impl_->Finish(VXCORE_ERR_IO);
  std::ifstream input(source, std::ios::binary);
  if (!input) return impl_->Finish(VXCORE_ERR_IO);
  SyncHttpRequest request;
  request.input = &input;
  request.upload_size = size;
  error = impl_->Request("PUT", path,
                         {expected_etag.empty() ? "If-None-Match: *" : "If-Match: " + expected_etag,
                          "Content-Type: application/octet-stream", "Expect: 100-continue"},
                         request);
  if (error != VXCORE_OK) return error;
  if (LastResponse().http_status != 200 && LastResponse().http_status != 201 &&
      LastResponse().http_status != 204)
    return impl_->FinishHttp("PUT", LastResponse().http_status);
  const auto &transfer = impl_->http.LastResponse();
  if (transfer.bytes != size || std::filesystem::file_size(source, ec) != size || ec ||
      std::filesystem::last_write_time(source, ec) != time || ec)
    return impl_->Finish(VXCORE_ERR_SYNC_IN_PROGRESS);
  if (!IsStrongEtag(transfer.etag)) return impl_->FinishEtag("PUT response", transfer.etag);
  impl_->response.sha256 = transfer.sha256;
  return impl_->Finish(VXCORE_OK);
}

VxCoreError WebDavTransport::Move(const std::string &source, const std::string &source_etag,
                                  const std::string &destination,
                                  const std::string &destination_etag) {
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (ValidateRelativePath(source) != VXCORE_OK || ValidateRelativePath(destination) != VXCORE_OK ||
      source == destination || !IsStrongEtag(source_etag) ||
      (!destination_etag.empty() && !IsStrongEtag(destination_etag)))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  WebDavResource resource;
  error = Stat(source, resource);
  if (error != VXCORE_OK) return error;
  if (resource.kind != WebDavResourceKind::kFile) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  if (resource.etag != source_etag) return impl_->Finish(VXCORE_ERR_SYNC_CONFLICT);
  // Never replace a collection, even if a nonconforming server reuses a file ETag.
  error = Stat(destination, resource);
  if (error != VXCORE_OK && error != VXCORE_ERR_NOT_FOUND) return error;
  if (error == VXCORE_OK && resource.kind != WebDavResourceKind::kFile)
    return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  const auto target = impl_->http.UrlFor(destination);
  std::vector<std::string> headers{"Destination: " + target, "If-Match: " + source_etag,
                                   destination_etag.empty() ? "Overwrite: F" : "Overwrite: T"};
  if (!destination_etag.empty())
    headers.push_back("If: <" + target + "> ([" + destination_etag + "])");
  error = impl_->Request("MOVE", source, headers);
  if (error != VXCORE_OK) return error;
  return LastResponse().http_status == 201 || LastResponse().http_status == 204
             ? impl_->Finish(VXCORE_OK)
             : impl_->FinishHttp("MOVE", LastResponse().http_status);
}

VxCoreError WebDavTransport::RemoveFile(const std::string &path, const std::string &expected_etag) {
  if (ValidateRelativePath(path) != VXCORE_OK || !IsStrongEtag(expected_etag))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  WebDavResource resource;
  auto error = Stat(path, resource);
  if (error != VXCORE_OK) return error;
  if (resource.kind != WebDavResourceKind::kFile) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  error = impl_->Request("DELETE", path, {"If-Match: " + expected_etag});
  if (error != VXCORE_OK) return error;
  return LastResponse().http_status == 200 || LastResponse().http_status == 204
             ? impl_->Finish(VXCORE_OK)
             : impl_->FinishHttp("DELETE", LastResponse().http_status);
}

VxCoreError WebDavTransport::MakeCollection(const std::string &path) {
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (ValidateRelativePath(path) != VXCORE_OK) return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  SyncHttpRequest request;
  request.collection = true;
  error = impl_->Request("MKCOL", path, {}, request);
  if (error != VXCORE_OK) return error;
  if (LastResponse().http_status == 201) return impl_->Finish(VXCORE_OK);
  if (LastResponse().http_status == 405) {
    WebDavResource resource;
    error = Stat(path, resource);
    if (error != VXCORE_OK) return error;
    return impl_->Finish(resource.kind == WebDavResourceKind::kCollection ? VXCORE_OK
                                                                          : VXCORE_ERR_UNSUPPORTED);
  }
  return impl_->FinishHttp("MKCOL", LastResponse().http_status);
}

}  // namespace vxcore
