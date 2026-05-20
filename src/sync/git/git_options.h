#ifndef VXCORE_SYNC_GIT_GIT_OPTIONS_H
#define VXCORE_SYNC_GIT_GIT_OPTIONS_H

#include <nlohmann/json.hpp>
#include <string>

#include "vxcore/vxcore_types.h"

namespace vxcore {

// Typed view over SyncConfig::backend_options for the git backend (Task 5.4 /
// F1.5 of sync-backend-phase4). Today no git/ source consumes
// backend_options directly — this struct is the forward-looking surface so
// that adding a new tunable means adding a typed field (with default) here
// instead of sprinkling stringly-typed json[...] lookups across the backend.
//
// On-disk shape of SyncConfig::backend_options is unchanged (still
// nlohmann::json). FromJson() is total: missing/wrong-typed/null/empty JSON
// silently falls back to defaults, no throws. JSON keys are camelCase per
// libs/vxcore/AGENTS.md; struct members are snake_case.
struct GitOptions {
  // Verify TLS certificates for HTTPS remotes. JSON key: "sslVerify".
  bool ssl_verify = true;

  // Connect timeout for libgit2 transports, in milliseconds.
  // JSON key: "connectTimeoutMs".
  int connect_timeout_ms = 30000;

  // Optional HTTP/HTTPS proxy URL (e.g., "http://proxy.example.com:8080").
  // Empty means "no proxy override; libgit2 picks up environment".
  // JSON key: "proxyUrl".
  std::string proxy_url;

  // Parse a GitOptions out of an opaque backend_options blob. Total function:
  //   - null / empty / non-object json -> all defaults
  //   - object with missing keys       -> per-field defaults
  //   - object with wrong-typed values -> per-field default + WARN log
  // Never throws.
  static VXCORE_API GitOptions FromJson(const nlohmann::json &json);
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_OPTIONS_H
