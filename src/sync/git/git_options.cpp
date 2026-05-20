#include "sync/git/git_options.h"

#include "utils/logger.h"

namespace vxcore {

namespace {

// Read a JSON field as type T into out, returning false (and logging WARN)
// when the field is present but has the wrong type. Missing fields are
// treated as "leave default in place" — they return true with no log.
template <typename T>
bool ReadField(const nlohmann::json &json, const char *key, T &out) {
  auto it = json.find(key);
  if (it == json.end()) return true;  // absent -> default kept silently
  try {
    out = it->template get<T>();
    return true;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_WARN(
        "GitOptions: ignoring malformed backendOptions field '%s' (%s); "
        "falling back to default",
        key, e.what());
    return false;
  }
}

}  // namespace

GitOptions GitOptions::FromJson(const nlohmann::json &json) {
  GitOptions opts;  // all fields default-initialized.
  if (json.is_null() || !json.is_object() || json.empty()) {
    return opts;
  }
  ReadField(json, "sslVerify", opts.ssl_verify);
  ReadField(json, "connectTimeoutMs", opts.connect_timeout_ms);
  ReadField(json, "proxyUrl", opts.proxy_url);
  return opts;
}

}  // namespace vxcore
