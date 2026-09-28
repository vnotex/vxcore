#ifndef VXCORE_SYNC_HTTP_CLIENT_H
#define VXCORE_SYNC_HTTP_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sync/sync_cancellation.h"
#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {
class AtomicFileWriter;

// Shared HTTP syntax helpers; DAV XML and portable local-path policy stay in the adapter.
namespace sync_http {
struct Url {
  std::string origin;
  std::string path;  // Decoded absolute path, never normalized through dot segments.
  std::string scheme;
  std::string host;
  std::string String() const;
};
bool IsUtf8(std::string_view value);
bool ParseUnsigned(std::string_view text, uint64_t &number);
long ParseStatus(std::string_view text);
bool ParseUrl(const std::string &text, Url &url);
bool ResolveUrl(const Url &root, const std::string &base, const std::string &href, Url &out);
std::string Relative(const Url &root, const Url &url);
}  // namespace sync_http

struct SyncHttpResponse {
  long http_status = 0;
  std::string effective_url;
  std::string corrected_url;
  // Values retain the server's syntax (including unquoted/weak ETags), with HTTP OWS removed.
  // Header names are lowercase; repeated headers remain separate entries.
  std::vector<std::pair<std::string, std::string>> headers;
  std::string etag;
  std::string retry_after;
  std::string body;
  bool length_seen = false;
  uint64_t content_length = 0;
  uint64_t bytes = 0;
  std::string sha256;  // Streamed file/range hash; buffered bodies are hashed by the caller.
};

struct SyncHttpRequest {
  static constexpr size_t kDefaultBodyLimit = 128 * 1024 * 1024;
  bool collection = false;
  // Capture only this status into LastResponse().body; zero discards response bodies.
  // Every other body is discarded with a separate 64 KiB bound, including auth challenges.
  long body_status = 0;
  size_t body_limit = kDefaultBodyLimit;
  // GET streams status 200 into this already-open writer. The caller MUST validate status,
  // length, hash/validator and its local path, then Commit(); errors never publish the file.
  AtomicFileWriter *writer = nullptr;
  // Immutable seekable snapshot, owned and lifetime-checked by the caller. Only this range
  // is sent/hashed; challenge authentication rewinds to upload_offset, never file offset zero.
  // A 16 MiB object therefore needs no 16 MiB transfer allocation.
  std::ifstream *input = nullptr;
  uint64_t upload_offset = 0;
  uint64_t upload_size = 0;
  // Optional small request body; storage must remain alive until Request returns.
  std::string_view body;
};

// Transport failure classification lets adapters retain their protocol-specific diagnostics
// and status mapping. HTTP error statuses themselves are returned raw, not interpreted here.
enum class SyncHttpFailure {
  kNone,
  kLibrariesUnavailable,
  kOptionUnavailable,
  kRedirectRejected,
  kDiscardLimit
};

// Synchronous, operation-owned, non-concurrent session. Credentials/cancellation are snapshots;
// there are no provider callbacks, scheduler, retry policy or logging. Certificate and hostname
// verification are mandatory; the existing custom CA seam requires actual vxcore test mode.
class SyncHttpClient final {
 public:
  SyncHttpClient(const std::string &collection_url, const SyncCredentials &credentials,
                 SyncCancellationPtr cancellation);
  ~SyncHttpClient();
  SyncHttpClient(const SyncHttpClient &) = delete;
  SyncHttpClient &operator=(const SyncHttpClient &) = delete;

  VxCoreError Initialize();  // Local validation/handle creation only; Request calls it lazily.
  const std::string &CanonicalRoot() const;
  const sync_http::Url &Root() const;
  const SyncHttpResponse &LastResponse() const;
  SyncHttpFailure LastFailure() const;
  // Fixed redacted detail for classified transport failures; otherwise empty. Never includes
  // response bodies, URLs, credentials or server-supplied header values.
  const std::string &LastErrorDetail() const;
  void SetProgressCallback(std::function<void(uint64_t, uint64_t)> callback);
  // Only use validated decoded relative paths for protocol headers such as Destination.
  std::string UrlFor(const std::string &path, bool collection = false) const;

  // Decoded relative path (empty = root); URL construction cannot escape the canonical root.
  // Supported methods: GET, OPTIONS, PROPFIND, PUT, MOVE, DELETE, MKCOL. Only reads
  // (GET/OPTIONS/PROPFIND) follow up to three same-origin/root-contained redirects.
  // Mutations always use fresh non-reusable HTTP/1.1 connections, without redirect/replay.
  // Request supplies protocol headers, status-body selection and sinks, not DAV policy.
  VxCoreError Request(const char *method, const std::string &path,
                      const std::vector<std::string> &headers = {},
                      const SyncHttpRequest &request = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_HTTP_CLIENT_H
