#ifndef VXCORE_SYNC_JIANGUOYUN_TRANSPORT_H
#define VXCORE_SYNC_JIANGUOYUN_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "sync/http/sync_http_client.h"

namespace vxcore {

struct JianguoyunResource {
  bool collection = false;
  uint64_t size = 0;
  std::string etag;
};

// Synchronous operation-owned session for VNote's managed Jianguoyun store.
// Publication verification and durable staging ownership belong to the backend.
class JianguoyunTransport final {
 public:
  JianguoyunTransport(const std::string &collection_url, const SyncCredentials &credentials,
                      SyncCancellationPtr cancellation) noexcept;
  ~JianguoyunTransport();
  JianguoyunTransport(const JianguoyunTransport &) = delete;
  JianguoyunTransport &operator=(const JianguoyunTransport &) = delete;

  VxCoreError Initialize() noexcept;
  const std::string &CanonicalRoot() const noexcept;
  const std::string &LastError() const noexcept;
  const SyncHttpResponse &LastResponse() const noexcept;
  void SetProgressCallback(std::function<void(uint64_t, uint64_t)> callback) noexcept;

  static VxCoreError CanonicalizeUrl(const std::string &url, std::string &out_url) noexcept;
  static bool IsRawEtag(const std::string &etag) noexcept;

  VxCoreError Stat(const std::string &path, JianguoyunResource &out) noexcept;
  VxCoreError Get(const std::string &path, std::string &body, std::string &etag,
                  size_t limit = 128 * 1024 * 1024) noexcept;
  VxCoreError Download(const std::string &path, const std::filesystem::path &local_root,
                       const std::string &local_relative_path, const std::string &expected_sha256,
                       uint64_t expected_size) noexcept;

  // Only staging paths, or head.json with a REQUIRED concrete raw If-Match token.
  // A successful upload does not establish ownership or verified publication.
  VxCoreError PutBytes(const std::string &path, const std::string &bytes,
                       const std::string &expected_etag = "") noexcept;
  VxCoreError UploadRange(const std::string &path, const std::filesystem::path &local_root,
                          const std::string &local_relative_path, uint64_t offset,
                          uint64_t size) noexcept;
  // Staging source, typed destination, Overwrite:F only. Even success needs a
  // destination GET+hash check; 409/412 never establish an identical winner.
  VxCoreError MoveCreate(const std::string &source, const std::string &destination) noexcept;
  // Root is inspected, not created. MKCOL status never establishes ownership.
  VxCoreError EnsureCollection(const std::string &path) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  VxCoreError construction_error_ = VXCORE_ERR_OUT_OF_MEMORY;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_JIANGUOYUN_TRANSPORT_H
