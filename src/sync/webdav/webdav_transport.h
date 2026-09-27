#ifndef VXCORE_SYNC_WEBDAV_TRANSPORT_H
#define VXCORE_SYNC_WEBDAV_TRANSPORT_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sync/sync_cancellation.h"
#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

enum class WebDavResourceKind { kFile, kCollection };

struct WebDavResource {
  // Decoded UTF-8, notebook-relative, no trailing slash; empty means the root.
  std::string path;
  WebDavResourceKind kind = WebDavResourceKind::kFile;
  // Exact quoted HTTP value. Files always have a strong ETag; collections may not.
  std::string etag;
  uint64_t size = 0;
  int64_t modified_utc = 0;
};

struct WebDavResponse {
  long http_status = 0;
  std::string effective_url;
  // Nonempty after a permitted read redirect. Never changes the configured binding.
  std::string corrected_url;
  std::string etag;
  uint64_t bytes = 0;
  std::string sha256;
};

// Synchronous, operation-owned session; NOT concurrently callable. A backend snapshots
// credentials/cancellation before construction and destroys the session after its round trip.
// No scheduler, retry policy, credential-provider calls, or logging live here.
class WebDavTransport final {
 public:
  WebDavTransport(const std::string &collection_url, const SyncCredentials &credentials,
                  SyncCancellationPtr cancellation);
  ~WebDavTransport();
  WebDavTransport(const WebDavTransport &) = delete;
  WebDavTransport &operator=(const WebDavTransport &) = delete;

  // Local validation and handle creation only; no network. Other operations call it lazily.
  VxCoreError Initialize();
  const std::string &CanonicalRoot() const;
  const WebDavResponse &LastResponse() const;
  // Fixed, redacted, actionable text. Never contains response bodies, URLs or credentials.
  const std::string &LastError() const;
  void SetProgressCallback(std::function<void(uint64_t, uint64_t)> callback);

  VxCoreError Options();  // Informational only; not a substitute for conditional probes.
  VxCoreError Stat(const std::string &path, WebDavResource &out_resource);
  // Complete Depth:1 traversal INCLUDING root and all scratch entries. Backend owns exclusions.
  // Any error clears out_resources; partial listings must never imply remote deletions.
  VxCoreError List(std::vector<WebDavResource> &out_resources);

  // Download publishes atomically only after successful If-Match GET, exact ETag and size
  // validation. Root must exist; the caller creates parents and serializes local mutations.
  VxCoreError Download(const std::string &path, const std::string &expected_etag,
                       const std::filesystem::path &local_root,
                       const std::string &local_relative_path);
  // Empty expected_etag means create-if-absent; otherwise exact strong If-Match.
  // Source must be an immutable, seekable snapshot (Digest authentication can rewind it).
  VxCoreError Upload(const std::string &path, const std::filesystem::path &local_root,
                     const std::string &local_relative_path, const std::string &expected_etag);
  // Empty destination_etag means Overwrite:F; otherwise tagged destination If + Overwrite:T.
  VxCoreError Move(const std::string &source, const std::string &source_etag,
                   const std::string &destination, const std::string &destination_etag);
  // Verifies the target is a file before sending conditional DELETE; never deletes collections.
  VxCoreError RemoveFile(const std::string &path, const std::string &expected_etag);
  VxCoreError MakeCollection(const std::string &path);

  static bool IsStrongEtag(const std::string &etag);
  static VxCoreError ValidateRelativePath(const std::string &path, bool allow_root = false);
  // Checks each existing parent/target for symlinks/reparse points, native aliases, containment.
  // This is a path-based guard, not protection against hostile concurrent filesystem mutation.
  static VxCoreError ResolveLocalPath(const std::filesystem::path &root,
                                      const std::string &relative_path,
                                      std::filesystem::path &out_path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_WEBDAV_TRANSPORT_H
