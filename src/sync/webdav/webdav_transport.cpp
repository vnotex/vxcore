#include "webdav_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <thread>
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
#include <sodium.h>

#include <pugixml.hpp>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "core/config_manager.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kXmlLimit = 16 * 1024 * 1024;
constexpr size_t kHeaderLimit = 64 * 1024;
constexpr size_t kChunkSize = 64 * 1024;
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

bool IsUtf8(std::string_view value) {
  for (size_t i = 0; i < value.size();) {
    const auto first = static_cast<unsigned char>(value[i++]);
    if (first < 0x80) continue;
    unsigned int count = 0, scalar = 0, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1;
      scalar = first & 31;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2;
      scalar = first & 15;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3;
      scalar = first & 7;
      minimum = 0x10000;
    } else
      return false;
    if (value.size() - i < count) return false;
    while (count--) {
      const auto next = static_cast<unsigned char>(value[i++]);
      if ((next & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (next & 63);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
      return false;
  }
  return true;
}

bool SafeName(std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.size() > 255 || !IsUtf8(name))
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

int Hex(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool DecodePath(const std::string &encoded, std::string &decoded) {
  decoded.clear();
  if (encoded.empty() || encoded.front() != '/' || encoded.size() > kUrlLimit) return false;
  for (size_t i = 0; i < encoded.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(encoded[i]);
    if (ch == '%') {
      if (encoded.size() - i < 3 || Hex(encoded[i + 1]) < 0 || Hex(encoded[i + 2]) < 0)
        return false;
      ch = static_cast<unsigned char>(Hex(encoded[i + 1]) * 16 + Hex(encoded[i + 2]));
      i += 2;
      if (ch == '/' || ch == '\\') return false;
    }
    if (ch == 0 || ch < 32 || ch == 127 || ch == '\\') return false;
    decoded.push_back(static_cast<char>(ch));
  }
  if (!IsUtf8(decoded)) return false;
  size_t begin = 1;
  while (begin < decoded.size()) {
    const auto end = decoded.find('/', begin);
    const auto part = decoded.substr(begin, end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

std::string EncodePath(std::string_view decoded) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(decoded.size());
  for (unsigned char ch : decoded) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/')
      out.push_back(static_cast<char>(ch));
    else {
      out.push_back('%');
      out.push_back(hex[ch >> 4]);
      out.push_back(hex[ch & 15]);
    }
  }
  return out;
}

struct Url {
  std::string origin;
  std::string path;  // Decoded absolute path, never normalized through dot segments.
  std::string scheme;
  std::string host;
  std::string String() const { return origin + EncodePath(path); }
};

bool ParseUrl(const std::string &text, Url &url) {
  if (text.size() > kUrlLimit || text.find_first_of("?#\\\r\n\t") != std::string::npos ||
      text.find('\0') != std::string::npos)
    return false;
  const auto separator = text.find("://");
  if (separator == std::string::npos) return false;
  url.scheme = LowerAscii(text.substr(0, separator));
  if (url.scheme != "https" && url.scheme != "http") return false;
  const auto path_start = text.find('/', separator + 3);
  const auto authority = text.substr(separator + 3, path_start - separator - 3);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  std::string port;
  if (authority.front() == '[') {
    const auto end = authority.find(']');
    if (end == std::string::npos) return false;
    url.host = LowerAscii(authority.substr(0, end + 1));
    if (end + 1 < authority.size()) {
      if (authority[end + 1] != ':') return false;
      port = authority.substr(end + 2);
      if (port.empty()) return false;
    }
  } else {
    const auto colon = authority.find(':');
    url.host = LowerAscii(authority.substr(0, colon));
    if (colon != std::string::npos) {
      port = authority.substr(colon + 1);
      if (port.empty()) return false;
    }
  }
  if (url.host.empty()) return false;
  for (unsigned char ch : url.host) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
          ch == '[' || ch == ']' || ch == ':'))
      return false;
  }
  if (!port.empty()) {
    unsigned int number = 0;
    for (char ch : port) {
      if (ch < '0' || ch > '9' || number > 6553) return false;
      number = number * 10 + ch - '0';
    }
    if (!number || number > 65535) return false;
    port = std::to_string(number);
    if ((url.scheme == "https" && number == 443) || (url.scheme == "http" && number == 80))
      port.clear();
  }
  if (!DecodePath(path_start == std::string::npos ? "/" : text.substr(path_start), url.path))
    return false;
  url.origin = url.scheme + "://" + url.host + (port.empty() ? "" : ":" + port);
  return true;
}

bool WithinRoot(const Url &root, const Url &url) {
  if (root.origin != url.origin) return false;
  const auto without_slash = root.path.substr(0, root.path.size() - 1);
  return url.path == without_slash || url.path.compare(0, root.path.size(), root.path) == 0;
}

bool ResolveUrl(const Url &root, const std::string &base, const std::string &href, Url &out) {
  if (href.empty() || href.size() > kUrlLimit ||
      href.find_first_of("?#\\\r\n\t") != std::string::npos || href.find('\0') != std::string::npos)
    return false;
  std::string target;
  if (href.find("://") != std::string::npos)
    target = href;
  else if (href.compare(0, 2, "//") == 0)
    target = root.scheme + ":" + href;
  else if (href.front() == '/')
    target = root.origin + href;
  else {
    // A colon in the first component is an unsupported URI scheme, not a filename.
    if (href.substr(0, href.find('/')).find(':') != std::string::npos) return false;
    target = base.substr(0, base.rfind('/') + 1) + href;
  }
  // Literal current-directory URI segments are safe RFC 3986 relative references
  // (notably a collection's "./" self href). DecodePath still rejects encoded dot
  // segments and every parent-directory segment rather than normalizing traversal.
  const auto path_start = target.find('/', target.find("://") + 3);
  if (path_start != std::string::npos) {
    for (auto dot = target.find("/./", path_start); dot != std::string::npos;
         dot = target.find("/./", dot))
      target.erase(dot + 1, 2);
    if (target.size() >= 2 && target.compare(target.size() - 2, 2, "/.") == 0) target.pop_back();
  }
  return ParseUrl(target, out) && WithinRoot(root, out);
}

std::string Relative(const Url &root, const Url &url) {
  if (url.path.size() < root.path.size()) return {};
  auto relative = url.path.substr(root.path.size());
  if (!relative.empty() && relative.back() == '/') relative.pop_back();
  return relative;
}

bool ParseUnsigned(std::string_view text, uint64_t &number) {
  if (text.empty()) return false;
  number = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9' || number > (std::numeric_limits<uint64_t>::max() - (ch - '0')) / 10)
      return false;
    number = number * 10 + ch - '0';
  }
  return true;
}

long ParseStatus(std::string_view text) {
  if (text.compare(0, 5, "HTTP/") != 0) return 0;
  const auto space = text.find(' ');
  if (space == std::string::npos || text.size() < space + 4) return 0;
  const auto version = text.substr(0, space);
  if (version != "HTTP/1.0" && version != "HTTP/1.1" && version != "HTTP/2" &&
      version != "HTTP/2.0")
    return 0;
  const auto digits = text.substr(space + 1, 3);
  uint64_t number = 0;
  if (!ParseUnsigned(digits, number) || number < 100 || number > 599) return 0;
  if (text.size() > space + 4 && text[space + 4] != ' ' && text[space + 4] != '\r' &&
      text[space + 4] != '\n')
    return 0;
  return static_cast<long>(number);
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

// C++ local static initialization is process-wide and thread-safe. Never clean up
// libcurl globally while another embedded consumer could still have live handles.
bool InitializeLibraries() {
  static const bool ready = [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || sodium_init() < 0) return false;
    const auto *version = curl_version_info(CURLVERSION_NOW);
    return version && version->version_num >= 0x074000 &&
           (version->features & CURL_VERSION_ASYNCHDNS) != 0;
  }();
  return ready;
}

std::string DigestHex(crypto_hash_sha256_state &state) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
  crypto_hash_sha256_final(&state, hash.data());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
  return hex.data();
}
}  // namespace

struct WebDavTransport::Impl {
  explicit Impl(std::string url, const SyncCredentials &credentials, SyncCancellationPtr token)
      : configured_url(std::move(url)), cancellation(std::move(token)) {
    if (!credentials.extra.is_null() && !credentials.extra.is_object()) credentials_valid = false;
    if (credentials.extra.is_object()) {
      for (const auto *key : {kJsonKeyUsername, kJsonKeyPassword}) {
        auto found = credentials.extra.find(key);
        if (found != credentials.extra.end() && !found->is_string()) credentials_valid = false;
      }
      if (credentials_valid) {
        username = credentials.extra.value(kJsonKeyUsername, std::string());
        password = credentials.extra.value(kJsonKeyPassword, std::string());
      }
    }
    if (username.find_first_of("\r\n:") != std::string::npos ||
        username.find('\0') != std::string::npos || password.find('\0') != std::string::npos)
      credentials_valid = false;
    if (username.empty() != password.empty()) credentials_valid = false;
  }
  ~Impl() {
    if (easy) curl_easy_cleanup(easy);
    if (multi) curl_multi_cleanup(multi);
    if (!password.empty()) sodium_memzero(password.data(), password.size());
  }

  std::string configured_url, canonical_root, username, password, ca_file;
  bool credentials_valid = true;
  Url root;
  SyncCancellationPtr cancellation;
  std::function<void(uint64_t, uint64_t)> progress;
  CURL *easy = nullptr;
  CURLM *multi = nullptr;
  Clock::time_point deadline = Clock::now() + std::chrono::minutes(30);
  WebDavResponse response;
  std::string error_text;

  VxCoreError Finish(VxCoreError error) {
    error_text = ErrorText(error);
    return error;
  }
  bool Cancelled() const { return cancellation && cancellation->IsCancelled(); }
  std::string UrlFor(const std::string &path, bool collection = false) const {
    auto result = canonical_root + EncodePath(path);
    if (collection && !path.empty()) result += '/';
    return result;
  }

  struct Transfer {
    Impl *owner = nullptr;
    long status = 0;
    VxCoreError error = VXCORE_OK;
    size_t header_bytes = 0, discarded_bytes = 0;
    std::string xml, etag, location;
    bool xml_body = false, etag_seen = false, location_seen = false, length_seen = false;
    uint64_t content_length = 0, bytes = 0, upload_size = 0;
    AtomicFileWriter *writer = nullptr;
    std::ifstream *input = nullptr;
    crypto_hash_sha256_state hash{};
    Clock::time_point last_activity = Clock::now();
    curl_off_t last_download = 0, last_upload = 0;

    static size_t Header(char *data, size_t size, size_t count, void *userdata) noexcept {
      auto &self = *static_cast<Transfer *>(userdata);
      try {
        if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
        const size_t length = size * count;
        if (length > 16384 || length > kHeaderLimit - self.header_bytes) {
          self.error = VXCORE_ERR_INVALID_STATE;
          return 0;
        }
        self.header_bytes += length;
        self.last_activity = Clock::now();
        std::string line(data, length);
        if (line.compare(0, 5, "HTTP/") == 0) {
          self.status = ParseStatus(line);
          if (!self.status) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.etag.clear();
          self.location.clear();
          self.etag_seen = self.location_seen = self.length_seen = false;
          self.content_length = 0;
          return length;
        }
        if (line == "\r\n" || line == "\n") return length;
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.front() == ' ' || line.front() == '\t') {
          self.error = VXCORE_ERR_INVALID_STATE;
          return 0;
        }
        const auto name = LowerAscii(line.substr(0, colon));
        const auto start = line.find_first_not_of(" \t", colon + 1);
        const auto end = line.find_last_not_of(" \t\r\n");
        const auto value =
            start == std::string::npos || end < start ? "" : line.substr(start, end - start + 1);
        if (value.find_first_of("\r\n") != std::string::npos ||
            value.find('\0') != std::string::npos) {
          self.error = VXCORE_ERR_INVALID_STATE;
          return 0;
        }
        if (name == "etag") {
          if (self.etag_seen) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.etag_seen = true;
          self.etag = value;
        } else if (name == "location") {
          if (self.location_seen) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.location_seen = true;
          self.location = value;
        } else if (name == "content-length") {
          if (self.length_seen || !ParseUnsigned(value, self.content_length)) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.length_seen = true;
          if (self.xml_body && self.status == 207 && self.content_length > kXmlLimit) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
        }
        return length;
      } catch (...) {
        self.error = VXCORE_ERR_OUT_OF_MEMORY;
        return 0;
      }
    }

    static size_t Write(char *data, size_t size, size_t count, void *userdata) noexcept {
      auto &self = *static_cast<Transfer *>(userdata);
      try {
        if (self.owner->Cancelled()) {
          self.error = VXCORE_ERR_CANCELLED;
          return 0;
        }
        if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
        const size_t length = size * count;
        self.last_activity = Clock::now();
        if (self.xml_body && self.status == 207) {
          if (length > kXmlLimit - self.xml.size()) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.xml.append(data, length);
        } else if (self.writer && self.status == 200) {
          for (size_t offset = 0; offset < length;) {
            if (self.owner->Cancelled()) {
              self.error = VXCORE_ERR_CANCELLED;
              return 0;
            }
            const auto chunk = std::min(kChunkSize, length - offset);
            self.error = self.writer->Write(data + offset, chunk);
            if (self.error != VXCORE_OK) return 0;
            crypto_hash_sha256_update(
                &self.hash, reinterpret_cast<const unsigned char *>(data + offset), chunk);
            self.bytes += chunk;
            offset += chunk;
          }
        } else {
          if (length > kHeaderLimit - self.discarded_bytes) {
            self.error = self.status >= 300 ? HttpError(self.status) : VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.discarded_bytes += length;
        }
        return length;
      } catch (...) {
        self.error = VXCORE_ERR_IO;
        return 0;
      }
    }

    static size_t Read(char *data, size_t size, size_t count, void *userdata) noexcept {
      auto &self = *static_cast<Transfer *>(userdata);
      try {
        if (self.owner->Cancelled()) {
          self.error = VXCORE_ERR_CANCELLED;
          return CURL_READFUNC_ABORT;
        }
        if (size && count > std::numeric_limits<size_t>::max() / size) return CURL_READFUNC_ABORT;
        const auto length = std::min(kChunkSize, size * count);
        self.input->read(data, static_cast<std::streamsize>(length));
        const auto read = self.input->gcount();
        if (self.input->bad() || (self.input->fail() && !self.input->eof())) {
          self.error = VXCORE_ERR_IO;
          return CURL_READFUNC_ABORT;
        }
        if (self.input->eof() && self.bytes + static_cast<uint64_t>(read) < self.upload_size) {
          self.error = VXCORE_ERR_IO;
          return CURL_READFUNC_ABORT;
        }
        crypto_hash_sha256_update(&self.hash, reinterpret_cast<unsigned char *>(data),
                                  static_cast<unsigned long long>(read));
        self.bytes += static_cast<uint64_t>(read);
        self.last_activity = Clock::now();
        return static_cast<size_t>(read);
      } catch (...) {
        self.error = VXCORE_ERR_IO;
        return CURL_READFUNC_ABORT;
      }
    }

    static int Seek(void *userdata, curl_off_t offset, int origin) noexcept {
      auto &self = *static_cast<Transfer *>(userdata);
      // curl authentication rewinds the body to zero. Never hash an incomplete suffix.
      if (origin != SEEK_SET || offset != 0) return CURL_SEEKFUNC_CANTSEEK;
      try {
        self.input->clear();
        self.input->seekg(0);
        if (!*self.input) {
          self.error = VXCORE_ERR_IO;
          return CURL_SEEKFUNC_FAIL;
        }
        self.bytes = 0;
        crypto_hash_sha256_init(&self.hash);
        return CURL_SEEKFUNC_OK;
      } catch (...) {
        self.error = VXCORE_ERR_IO;
        return CURL_SEEKFUNC_FAIL;
      }
    }

    static int Progress(void *userdata, curl_off_t download_total, curl_off_t download,
                        curl_off_t upload_total, curl_off_t upload) noexcept {
      auto &self = *static_cast<Transfer *>(userdata);
      if (self.owner->Cancelled()) {
        self.error = VXCORE_ERR_CANCELLED;
        return 1;
      }
      if (download != self.last_download || upload != self.last_upload) {
        self.last_activity = Clock::now();
        self.last_download = download;
        self.last_upload = upload;
        try {
          if (self.owner->progress)
            self.owner->progress(static_cast<uint64_t>(std::max<curl_off_t>(0, download) +
                                                       std::max<curl_off_t>(0, upload)),
                                 static_cast<uint64_t>(std::max<curl_off_t>(0, download_total) +
                                                       std::max<curl_off_t>(0, upload_total)));
        } catch (...) {
          self.error = VXCORE_ERR_CANCELLED;
          return 1;
        }
      }
      return 0;
    }
  };

  VxCoreError Request(const char *method, std::string url, const std::vector<std::string> &headers,
                      Transfer &transfer, bool allow_redirect, const char *body = nullptr) {
    response = {};
    std::string corrected;
    for (unsigned redirects = 0;; ++redirects) {
      if (Cancelled()) return Finish(VXCORE_ERR_CANCELLED);
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      if (remaining.count() <= 0) return Finish(VXCORE_ERR_SYNC_NETWORK);
      transfer.owner = this;
      transfer.status = 0;
      transfer.error = VXCORE_OK;
      transfer.header_bytes = transfer.discarded_bytes = 0;
      transfer.etag.clear();
      transfer.location.clear();
      transfer.xml.clear();
      transfer.etag_seen = transfer.location_seen = transfer.length_seen = false;
      transfer.content_length = transfer.bytes = 0;
      transfer.last_activity = Clock::now();
      transfer.last_download = transfer.last_upload = 0;
      crypto_hash_sha256_init(&transfer.hash);
      curl_easy_reset(easy);
      struct HeaderList {
        curl_slist *value = nullptr;
        ~HeaderList() { curl_slist_free_all(value); }
        bool Add(const std::string &header) {
          auto *next = curl_slist_append(value, header.c_str());
          if (!next) return false;
          value = next;
          return true;
        }
      } list;
      for (const auto &header : headers)
        if (!list.Add(header)) return Finish(VXCORE_ERR_OUT_OF_MEMORY);
      if (!list.Add("Accept-Encoding: identity")) return Finish(VXCORE_ERR_OUT_OF_MEMORY);
#define DAV_SET(option, value)                                  \
  if (curl_easy_setopt(easy, option, value) != CURLE_OK) {      \
    error_text = "WebDAV HTTP support is unavailable (" #option \
                 "). Check the libcurl build configuration.";   \
    return VXCORE_ERR_UNSUPPORTED;                              \
  }
      DAV_SET(CURLOPT_URL, url.c_str());
      DAV_SET(CURLOPT_CUSTOMREQUEST, method);
      DAV_SET(CURLOPT_HTTPHEADER, list.value);
      DAV_SET(CURLOPT_NOSIGNAL, 1L);
      DAV_SET(CURLOPT_FOLLOWLOCATION, 0L);
      // libcurl may transparently replay a failed request on a reused connection.
      // Mutations use a fresh connection so a lost acknowledgement is never retried.
      if (!allow_redirect) {
        DAV_SET(CURLOPT_FRESH_CONNECT, 1L);
        DAV_SET(CURLOPT_FORBID_REUSE, 1L);
        // Apple system curl may include HTTP/2; avoid its REFUSED_STREAM replay path.
        DAV_SET(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
      }
      DAV_SET(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
      DAV_SET(CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
      DAV_SET(CURLOPT_PATH_AS_IS, 1L);
      // easy_reset leaves .netrc disabled; setting even IGNORED is unsupported
      // when the production curl build compiles .netrc support out entirely.
      DAV_SET(CURLOPT_UNRESTRICTED_AUTH, 0L);
      DAV_SET(CURLOPT_SSL_VERIFYPEER, 1L);
      DAV_SET(CURLOPT_SSL_VERIFYHOST, 2L);
      if (!ca_file.empty()) DAV_SET(CURLOPT_CAINFO, ca_file.c_str());
      DAV_SET(CURLOPT_CONNECTTIMEOUT_MS, 30000L);
      DAV_SET(CURLOPT_TIMEOUT_MS, static_cast<long>(remaining.count()));
      DAV_SET(CURLOPT_LOW_SPEED_LIMIT, 1L);
      DAV_SET(CURLOPT_LOW_SPEED_TIME, 60L);
      DAV_SET(CURLOPT_BUFFERSIZE, static_cast<long>(kChunkSize));
      DAV_SET(CURLOPT_UPLOAD_BUFFERSIZE, static_cast<long>(kChunkSize));
      DAV_SET(CURLOPT_NOPROGRESS, 0L);
      DAV_SET(CURLOPT_XFERINFOFUNCTION, &Transfer::Progress);
      DAV_SET(CURLOPT_XFERINFODATA, &transfer);
      DAV_SET(CURLOPT_HEADERFUNCTION, &Transfer::Header);
      DAV_SET(CURLOPT_HEADERDATA, &transfer);
      DAV_SET(CURLOPT_WRITEFUNCTION, &Transfer::Write);
      DAV_SET(CURLOPT_WRITEDATA, &transfer);
      DAV_SET(CURLOPT_HTTP_CONTENT_DECODING, 0L);
      if (!username.empty()) {
        DAV_SET(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC | CURLAUTH_DIGEST));
        DAV_SET(CURLOPT_USERNAME, username.c_str());
        DAV_SET(CURLOPT_PASSWORD, password.c_str());
      }
      if (body) {
        DAV_SET(CURLOPT_POSTFIELDS, body);
        DAV_SET(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(std::strlen(body)));
      }
      if (transfer.input) {
        DAV_SET(CURLOPT_UPLOAD, 1L);
        DAV_SET(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(transfer.upload_size));
        DAV_SET(CURLOPT_READFUNCTION, &Transfer::Read);
        DAV_SET(CURLOPT_READDATA, &transfer);
        DAV_SET(CURLOPT_SEEKFUNCTION, &Transfer::Seek);
        DAV_SET(CURLOPT_SEEKDATA, &transfer);
      }
#undef DAV_SET
      if (curl_multi_add_handle(multi, easy) != CURLM_OK) return Finish(VXCORE_ERR_SYNC_NETWORK);
      CURLcode result = CURLE_OK;
      bool complete = false;
      int running = 0;
      do {
        if (Cancelled()) {
          transfer.error = VXCORE_ERR_CANCELLED;
          break;
        }
        if (Clock::now() >= deadline ||
            Clock::now() - transfer.last_activity >= std::chrono::seconds(60)) {
          transfer.error = VXCORE_ERR_SYNC_NETWORK;
          break;
        }
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
          transfer.error = VXCORE_ERR_SYNC_NETWORK;
          break;
        }
        int messages = 0;
        while (auto *message = curl_multi_info_read(multi, &messages)) {
          if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
            result = message->data.result;
            complete = true;
          }
        }
        if (complete || !running) break;
        int descriptors = 0;
        if (curl_multi_wait(multi, nullptr, 0, 90, &descriptors) != CURLM_OK) {
          transfer.error = VXCORE_ERR_SYNC_NETWORK;
          break;
        }
        // The threaded resolver may have no fd yet; multi_wait then returns immediately.
        if (!descriptors) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (true);
      curl_multi_remove_handle(multi, easy);
      response.http_status = transfer.status;
      response.effective_url = url;
      response.corrected_url = corrected;
      response.etag = transfer.etag;
      response.bytes = transfer.bytes;
      if (Cancelled()) return Finish(VXCORE_ERR_CANCELLED);
      if (transfer.error != VXCORE_OK) return Finish(transfer.error);
      if (!complete || result != CURLE_OK) return Finish(VXCORE_ERR_SYNC_NETWORK);
      if (transfer.status >= 300 && transfer.status < 400) {
        if (!allow_redirect || redirects >= 3 ||
            (transfer.status != 301 && transfer.status != 302 && transfer.status != 303 &&
             transfer.status != 307 && transfer.status != 308))
          return Finish(VXCORE_ERR_UNSUPPORTED);
        Url destination;
        if (!ResolveUrl(root, url, transfer.location, destination))
          return Finish(VXCORE_ERR_UNSUPPORTED);
        url = destination.String();
        corrected = url;
        continue;
      }
      return Finish(VXCORE_OK);
    }
  }

  VxCoreError ParseListing(const Transfer &transfer, const std::string &requested, bool depth_one,
                           std::vector<WebDavResource> &out) {
    out.clear();
    const auto &xml = transfer.xml;
    // Do not let encoding autodetection hide declarations behind UTF-16 zero bytes.
    if (!IsUtf8(xml) || xml.find('\0') != std::string::npos ||
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
          if (ParseStatus(status) != 200) return Finish(VXCORE_ERR_INVALID_STATE);
        } else if (DavNode(child, "propstat")) {
          have_propstat = true;
          pugi::xml_node properties;
          bool got_status = false;
          for (const auto &part : child.children()) {
            if (part.type() != pugi::node_element) continue;
            if (DavNode(part, "status")) {
              std::string status;
              if (got_status || !TextOnly(part, status) || ParseStatus(status) != 200)
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
            if (DavNode(property, "resourcetype")) {
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
            } else if (DavNode(property, "getetag")) {
              if (have_etag || !TextOnly(property, resource.etag))
                return Finish(VXCORE_ERR_INVALID_STATE);
              have_etag = true;
            } else if (DavNode(property, "getcontentlength")) {
              std::string length;
              if (have_length || !TextOnly(property, length) ||
                  !ParseUnsigned(length, resource.size))
                return Finish(VXCORE_ERR_INVALID_STATE);
              have_length = true;
            } else if (DavNode(property, "getlastmodified")) {
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
      Url resolved;
      if (!ResolveUrl(root, response.effective_url, href, resolved))
        return Finish(VXCORE_ERR_INVALID_STATE);
      resource.path = Relative(root, resolved);
      if (WebDavTransport::ValidateRelativePath(resource.path, true) != VXCORE_OK)
        return Finish(VXCORE_ERR_INVALID_STATE);
      if (resource.kind == WebDavResourceKind::kFile &&
          (resolved.path.back() == '/' || !have_length ||
           !WebDavTransport::IsStrongEtag(resource.etag)))
        return Finish(!WebDavTransport::IsStrongEtag(resource.etag) ? VXCORE_ERR_UNSUPPORTED
                                                                    : VXCORE_ERR_INVALID_STATE);
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
    Transfer transfer;
    transfer.xml_body = true;
    auto error = Request(
        "PROPFIND", UrlFor(path, depth_one || path.empty()),
        {depth_one ? "Depth: 1" : "Depth: 0", "Content-Type: application/xml; charset=utf-8"},
        transfer, true, kProperties);
    if (error != VXCORE_OK) return error;
    if (response.http_status != 207) return Finish(HttpError(response.http_status));
    if (!response.corrected_url.empty()) {
      Url corrected;
      if (!ParseUrl(response.effective_url, corrected)) return Finish(VXCORE_ERR_INVALID_STATE);
      const auto corrected_path = Relative(root, corrected);
      // Discovery reports the corrected URL. Recursive scans cannot silently rebind
      // notebook-relative paths to a different collection.
      if (depth_one && corrected_path != path) return Finish(VXCORE_ERR_UNSUPPORTED);
      return ParseListing(transfer, corrected_path, depth_one, out);
    }
    return ParseListing(transfer, path, depth_one, out);
  }
};

WebDavTransport::WebDavTransport(const std::string &collection_url,
                                 const SyncCredentials &credentials,
                                 SyncCancellationPtr cancellation)
    : impl_(std::make_unique<Impl>(collection_url, credentials, std::move(cancellation))) {}
WebDavTransport::~WebDavTransport() = default;
const std::string &WebDavTransport::CanonicalRoot() const { return impl_->canonical_root; }
const WebDavResponse &WebDavTransport::LastResponse() const { return impl_->response; }
const std::string &WebDavTransport::LastError() const { return impl_->error_text; }
void WebDavTransport::SetProgressCallback(std::function<void(uint64_t, uint64_t)> callback) {
  impl_->progress = std::move(callback);
}

VxCoreError WebDavTransport::Initialize() {
  if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
  if (impl_->easy && impl_->multi) return VXCORE_OK;
  if (!impl_->credentials_valid || !ParseUrl(impl_->configured_url, impl_->root))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  if (impl_->root.scheme == "http" &&
      (!ConfigManager::IsTestMode() ||
       (impl_->root.host != "127.0.0.1" && impl_->root.host != "[::1]")))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  if (impl_->root.path.back() != '/') impl_->root.path += '/';
  impl_->canonical_root = impl_->root.String();
  if (ConfigManager::IsTestMode()) {
    const char *ca = std::getenv("VXCORE_WEBDAV_TEST_CA_FILE");
    if (ca) impl_->ca_file = ca;
  }
  if (!InitializeLibraries()) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  if (!impl_->easy) impl_->easy = curl_easy_init();
  if (!impl_->multi) impl_->multi = curl_multi_init();
  return impl_->Finish(impl_->easy && impl_->multi ? VXCORE_OK : VXCORE_ERR_OUT_OF_MEMORY);
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
  Impl::Transfer transfer;
  error = impl_->Request("OPTIONS", CanonicalRoot(), {}, transfer, true);
  if (error != VXCORE_OK) return error;
  return impl_->Finish(LastResponse().http_status >= 200 && LastResponse().http_status < 300
                           ? VXCORE_OK
                           : HttpError(LastResponse().http_status));
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
  Impl::Transfer transfer;
  transfer.writer = &writer;
  error =
      impl_->Request("GET", impl_->UrlFor(path), {"If-Match: " + expected_etag}, transfer, true);
  if (error != VXCORE_OK) return error;
  if (LastResponse().http_status != 200)
    return impl_->Finish(HttpError(LastResponse().http_status));
  if (!IsStrongEtag(transfer.etag)) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  if (transfer.etag != expected_etag) return impl_->Finish(VXCORE_ERR_SYNC_CONFLICT);
  if (transfer.length_seen && transfer.content_length != transfer.bytes)
    return impl_->Finish(VXCORE_ERR_INVALID_STATE);
  impl_->response.sha256 = DigestHex(transfer.hash);
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
  Impl::Transfer transfer;
  transfer.input = &input;
  transfer.upload_size = size;
  error = impl_->Request("PUT", impl_->UrlFor(path),
                         {expected_etag.empty() ? "If-None-Match: *" : "If-Match: " + expected_etag,
                          "Content-Type: application/octet-stream", "Expect: 100-continue"},
                         transfer, false);
  if (error != VXCORE_OK) return error;
  if (LastResponse().http_status != 200 && LastResponse().http_status != 201 &&
      LastResponse().http_status != 204)
    return impl_->Finish(HttpError(LastResponse().http_status));
  if (transfer.bytes != size || std::filesystem::file_size(source, ec) != size || ec ||
      std::filesystem::last_write_time(source, ec) != time || ec)
    return impl_->Finish(VXCORE_ERR_SYNC_IN_PROGRESS);
  if (!IsStrongEtag(transfer.etag)) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  impl_->response.sha256 = DigestHex(transfer.hash);
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
  const auto target = impl_->UrlFor(destination);
  std::vector<std::string> headers{"Destination: " + target, "If-Match: " + source_etag,
                                   destination_etag.empty() ? "Overwrite: F" : "Overwrite: T"};
  if (!destination_etag.empty())
    headers.push_back("If: <" + target + "> ([" + destination_etag + "])");
  Impl::Transfer transfer;
  error = impl_->Request("MOVE", impl_->UrlFor(source), headers, transfer, false);
  if (error != VXCORE_OK) return error;
  return impl_->Finish(LastResponse().http_status == 201 || LastResponse().http_status == 204
                           ? VXCORE_OK
                           : HttpError(LastResponse().http_status));
}

VxCoreError WebDavTransport::RemoveFile(const std::string &path, const std::string &expected_etag) {
  if (ValidateRelativePath(path) != VXCORE_OK || !IsStrongEtag(expected_etag))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  WebDavResource resource;
  auto error = Stat(path, resource);
  if (error != VXCORE_OK) return error;
  if (resource.kind != WebDavResourceKind::kFile) return impl_->Finish(VXCORE_ERR_UNSUPPORTED);
  Impl::Transfer transfer;
  error = impl_->Request("DELETE", impl_->UrlFor(path), {"If-Match: " + expected_etag}, transfer,
                         false);
  if (error != VXCORE_OK) return error;
  return impl_->Finish(LastResponse().http_status == 200 || LastResponse().http_status == 204
                           ? VXCORE_OK
                           : HttpError(LastResponse().http_status));
}

VxCoreError WebDavTransport::MakeCollection(const std::string &path) {
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (ValidateRelativePath(path) != VXCORE_OK) return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  Impl::Transfer transfer;
  error = impl_->Request("MKCOL", impl_->UrlFor(path, true), {}, transfer, false);
  if (error != VXCORE_OK) return error;
  if (LastResponse().http_status == 201) return impl_->Finish(VXCORE_OK);
  if (LastResponse().http_status == 405) {
    WebDavResource resource;
    error = Stat(path, resource);
    if (error != VXCORE_OK) return error;
    return impl_->Finish(resource.kind == WebDavResourceKind::kCollection ? VXCORE_OK
                                                                          : VXCORE_ERR_UNSUPPORTED);
  }
  return impl_->Finish(HttpError(LastResponse().http_status));
}

}  // namespace vxcore
