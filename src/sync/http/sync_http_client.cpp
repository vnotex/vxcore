#include "sync_http_client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
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

#include "core/config_manager.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kHeaderLimit = 64 * 1024;
constexpr size_t kChunkSize = 64 * 1024;
constexpr size_t kUrlLimit = 32768;

std::string LowerAscii(std::string value) {
  for (auto &ch : value)
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return value;
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

bool IsRequestPath(std::string_view path) {
  if (path.empty()) return true;
  if (path.front() == '/' || path.back() == '/' || !sync_http::IsUtf8(path)) return false;
  for (unsigned char ch : path)
    if (ch < 32 || ch == 127 || ch == '\\') return false;
  size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}
}  // namespace

namespace sync_http {
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

static int Hex(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static bool DecodePath(const std::string &encoded, std::string &decoded) {
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

static std::string EncodePath(std::string_view decoded) {
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

std::string Url::String() const { return origin + EncodePath(path); }

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

static bool WithinRoot(const Url &root, const Url &url) {
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
}  // namespace sync_http

struct SyncHttpClient::Impl {
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
  sync_http::Url root;
  SyncCancellationPtr cancellation;
  std::function<void(uint64_t, uint64_t)> progress;
  CURL *easy = nullptr;
  CURLM *multi = nullptr;
  Clock::time_point deadline = Clock::now() + std::chrono::minutes(30);
  SyncHttpResponse response;
  SyncHttpFailure failure = SyncHttpFailure::kNone;
  std::string error_detail;

  VxCoreError Finish(VxCoreError error, SyncHttpFailure cause = SyncHttpFailure::kNone,
                     const char *detail = nullptr) {
    failure = cause;
    error_detail = detail ? detail : "";
    return error;
  }
  bool Cancelled() const { return cancellation && cancellation->IsCancelled(); }

  struct Transfer {
    Impl *owner = nullptr;
    long status = 0;
    VxCoreError error = VXCORE_OK;
    size_t header_bytes = 0, discarded_bytes = 0;
    std::string body, etag, location, retry_after;
    std::vector<std::pair<std::string, std::string>> headers;
    long body_status = 0;
    size_t body_limit = 0;
    bool etag_seen = false, location_seen = false, length_seen = false;
    bool discard_limit = false;
    uint64_t content_length = 0, bytes = 0, upload_offset = 0, upload_size = 0;
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
          self.status = sync_http::ParseStatus(line);
          if (!self.status) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.etag.clear();
          self.location.clear();
          self.retry_after.clear();
          self.headers.clear();
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
        self.headers.emplace_back(name, value);
        if (name == "retry-after") self.retry_after = value;
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
          if (self.length_seen || !sync_http::ParseUnsigned(value, self.content_length)) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.length_seen = true;
          if (self.body_status && self.status == self.body_status &&
              self.content_length > self.body_limit) {
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
        if (self.body_status && self.status == self.body_status) {
          if (length > self.body_limit - self.body.size()) {
            self.error = VXCORE_ERR_INVALID_STATE;
            return 0;
          }
          self.body.append(data, length);
          self.bytes += length;
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
            self.discard_limit = true;
            self.error = VXCORE_ERR_INVALID_STATE;
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
        const auto length = static_cast<size_t>(
            std::min<uint64_t>(std::min(kChunkSize, size * count), self.upload_size - self.bytes));
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
      // curl authentication rewinds to the start of this range. Never hash an incomplete suffix.
      if (origin != SEEK_SET || offset != 0) return CURL_SEEKFUNC_CANTSEEK;
      try {
        self.input->clear();
        self.input->seekg(static_cast<std::streamoff>(self.upload_offset));
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
                      Transfer &transfer, bool allow_redirect, std::string_view body) {
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
      transfer.body.clear();
      transfer.retry_after.clear();
      transfer.headers.clear();
      transfer.discard_limit = false;
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
#define HTTP_SET(option, value)                                                \
  if (curl_easy_setopt(easy, option, value) != CURLE_OK) {                     \
    return Finish(VXCORE_ERR_UNSUPPORTED, SyncHttpFailure::kOptionUnavailable, \
                  "HTTP support is unavailable (" #option                      \
                  "). Check the libcurl build configuration.");                \
  }
      HTTP_SET(CURLOPT_URL, url.c_str());
      HTTP_SET(CURLOPT_CUSTOMREQUEST, method);
      HTTP_SET(CURLOPT_HTTPHEADER, list.value);
      HTTP_SET(CURLOPT_NOSIGNAL, 1L);
      HTTP_SET(CURLOPT_FOLLOWLOCATION, 0L);
      // libcurl may transparently replay a failed request on a reused connection.
      // Mutations use a fresh connection so a lost acknowledgement is never retried.
      if (!allow_redirect) {
        HTTP_SET(CURLOPT_FRESH_CONNECT, 1L);
        HTTP_SET(CURLOPT_FORBID_REUSE, 1L);
        // Apple system curl may include HTTP/2; avoid its REFUSED_STREAM replay path.
        HTTP_SET(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
      }
      HTTP_SET(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
      HTTP_SET(CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
      HTTP_SET(CURLOPT_PATH_AS_IS, 1L);
      // easy_reset leaves .netrc disabled; setting even IGNORED is unsupported
      // when the production curl build compiles .netrc support out entirely.
      HTTP_SET(CURLOPT_UNRESTRICTED_AUTH, 0L);
      HTTP_SET(CURLOPT_SSL_VERIFYPEER, 1L);
      HTTP_SET(CURLOPT_SSL_VERIFYHOST, 2L);
      if (!ca_file.empty()) HTTP_SET(CURLOPT_CAINFO, ca_file.c_str());
      HTTP_SET(CURLOPT_CONNECTTIMEOUT_MS, 30000L);
      HTTP_SET(CURLOPT_TIMEOUT_MS, static_cast<long>(remaining.count()));
      HTTP_SET(CURLOPT_LOW_SPEED_LIMIT, 1L);
      HTTP_SET(CURLOPT_LOW_SPEED_TIME, 60L);
      HTTP_SET(CURLOPT_BUFFERSIZE, static_cast<long>(kChunkSize));
      HTTP_SET(CURLOPT_UPLOAD_BUFFERSIZE, static_cast<long>(kChunkSize));
      HTTP_SET(CURLOPT_NOPROGRESS, 0L);
      HTTP_SET(CURLOPT_XFERINFOFUNCTION, &Transfer::Progress);
      HTTP_SET(CURLOPT_XFERINFODATA, &transfer);
      HTTP_SET(CURLOPT_HEADERFUNCTION, &Transfer::Header);
      HTTP_SET(CURLOPT_HEADERDATA, &transfer);
      HTTP_SET(CURLOPT_WRITEFUNCTION, &Transfer::Write);
      HTTP_SET(CURLOPT_WRITEDATA, &transfer);
      HTTP_SET(CURLOPT_HTTP_CONTENT_DECODING, 0L);
      if (!username.empty()) {
        HTTP_SET(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC | CURLAUTH_DIGEST));
        HTTP_SET(CURLOPT_USERNAME, username.c_str());
        HTTP_SET(CURLOPT_PASSWORD, password.c_str());
      }
      if (body.data()) {
        HTTP_SET(CURLOPT_POSTFIELDS, body.data());
        HTTP_SET(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
      }
      if (transfer.input) {
        HTTP_SET(CURLOPT_UPLOAD, 1L);
        HTTP_SET(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(transfer.upload_size));
        HTTP_SET(CURLOPT_READFUNCTION, &Transfer::Read);
        HTTP_SET(CURLOPT_READDATA, &transfer);
        HTTP_SET(CURLOPT_SEEKFUNCTION, &Transfer::Seek);
        HTTP_SET(CURLOPT_SEEKDATA, &transfer);
      }
#undef HTTP_SET
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
      response.etag = std::move(transfer.etag);
      response.retry_after = std::move(transfer.retry_after);
      response.headers = std::move(transfer.headers);
      response.body = std::move(transfer.body);
      response.length_seen = transfer.length_seen;
      response.content_length = transfer.content_length;
      response.bytes = transfer.bytes;
      if (Cancelled()) return Finish(VXCORE_ERR_CANCELLED);
      if (transfer.error != VXCORE_OK)
        return Finish(transfer.error, transfer.discard_limit ? SyncHttpFailure::kDiscardLimit
                                                             : SyncHttpFailure::kNone);
      if (!complete || result != CURLE_OK) return Finish(VXCORE_ERR_SYNC_NETWORK);
      if (transfer.status >= 300 && transfer.status < 400) {
        const char *reason = nullptr;
        sync_http::Url destination;
        if (!allow_redirect || redirects >= 3 ||
            (transfer.status != 301 && transfer.status != 302 && transfer.status != 303 &&
             transfer.status != 307 && transfer.status != 308))
          reason = "Write redirects and excessive read redirects are not allowed.";
        else if (!sync_http::ResolveUrl(root, url, transfer.location, destination))
          reason = "The redirect leaves the allowed origin or collection.";
        if (reason) {
          const auto detail = std::string(method) + " redirect (HTTP " +
                              std::to_string(transfer.status) + "): " + reason +
                              " Use the final collection URL.";
          return Finish(VXCORE_ERR_UNSUPPORTED, SyncHttpFailure::kRedirectRejected, detail.c_str());
        }
        url = destination.String();
        corrected = url;
        continue;
      }
      if (transfer.writer || transfer.input) response.sha256 = DigestHex(transfer.hash);
      return Finish(VXCORE_OK);
    }
  }
};

SyncHttpClient::SyncHttpClient(const std::string &collection_url,
                               const SyncCredentials &credentials, SyncCancellationPtr cancellation)
    : impl_(std::make_unique<Impl>(collection_url, credentials, std::move(cancellation))) {}
SyncHttpClient::~SyncHttpClient() = default;
const std::string &SyncHttpClient::CanonicalRoot() const { return impl_->canonical_root; }
const sync_http::Url &SyncHttpClient::Root() const { return impl_->root; }
const SyncHttpResponse &SyncHttpClient::LastResponse() const { return impl_->response; }
SyncHttpFailure SyncHttpClient::LastFailure() const { return impl_->failure; }
const std::string &SyncHttpClient::LastErrorDetail() const { return impl_->error_detail; }
void SyncHttpClient::SetProgressCallback(std::function<void(uint64_t, uint64_t)> callback) {
  impl_->progress = std::move(callback);
}
std::string SyncHttpClient::UrlFor(const std::string &path, bool collection) const {
  auto result = CanonicalRoot() + sync_http::EncodePath(path);
  if (collection && !path.empty()) result += '/';
  return result;
}

VxCoreError SyncHttpClient::Initialize() {
  if (impl_->Cancelled()) return impl_->Finish(VXCORE_ERR_CANCELLED);
  if (impl_->easy && impl_->multi) return VXCORE_OK;
  if (!impl_->credentials_valid || !sync_http::ParseUrl(impl_->configured_url, impl_->root))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  if (impl_->root.path.back() != '/') impl_->root.path += '/';
  impl_->canonical_root = impl_->root.String();
  if (ConfigManager::IsTestMode()) {
    const char *ca = std::getenv("VXCORE_WEBDAV_TEST_CA_FILE");
    if (ca) impl_->ca_file = ca;
  }
  if (!InitializeLibraries())
    return impl_->Finish(
        VXCORE_ERR_UNSUPPORTED, SyncHttpFailure::kLibrariesUnavailable,
        "HTTP synchronization requires libcurl 7.64 or newer with asynchronous DNS and "
        "initialized cryptographic support. Check the application build.");
  if (!impl_->easy) impl_->easy = curl_easy_init();
  if (!impl_->multi) impl_->multi = curl_multi_init();
  return impl_->Finish(impl_->easy && impl_->multi ? VXCORE_OK : VXCORE_ERR_OUT_OF_MEMORY);
}

VxCoreError SyncHttpClient::Request(const char *method, const std::string &path,
                                    const std::vector<std::string> &headers,
                                    const SyncHttpRequest &request) {
  impl_->response = {};
  auto error = Initialize();
  if (error != VXCORE_OK) return error;
  if (!method || !IsRequestPath(path)) return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  const std::string_view verb(method);
  const bool read = verb == "GET" || verb == "OPTIONS" || verb == "PROPFIND";
  if (!read && verb != "PUT" && verb != "MOVE" && verb != "DELETE" && verb != "MKCOL")
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  for (const auto &header : headers)
    if (header.find_first_of("\r\n") != std::string::npos || header.find('\0') != std::string::npos)
      return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  if ((request.writer && (request.body_status || request.input || verb != "GET")) ||
      (request.input && (read || request.body.data())) ||
      request.body.size() > static_cast<uint64_t>(std::numeric_limits<curl_off_t>::max()))
    return impl_->Finish(VXCORE_ERR_INVALID_PARAM);
  if (request.input) {
    const auto max_offset = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (request.upload_offset > max_offset ||
        request.upload_size > max_offset - request.upload_offset ||
        request.upload_size > static_cast<uint64_t>(std::numeric_limits<curl_off_t>::max()))
      return impl_->Finish(VXCORE_ERR_IO);
    request.input->clear();
    request.input->seekg(static_cast<std::streamoff>(request.upload_offset));
    if (!*request.input) return impl_->Finish(VXCORE_ERR_IO);
  }
  Impl::Transfer transfer;
  transfer.body_status = request.body_status;
  transfer.body_limit = request.body_limit;
  transfer.writer = request.writer;
  transfer.input = request.input;
  transfer.upload_offset = request.upload_offset;
  transfer.upload_size = request.upload_size;
  return impl_->Request(method, UrlFor(path, request.collection), headers, transfer, read,
                        request.body);
}

}  // namespace vxcore
