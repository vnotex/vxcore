#include "sync/git/git_credential_callback.h"

#include <cstddef>

#include "sync/credential_provider.h"
#include "sync/git/git_sync_log.h"
#include "sync/sync_cancellation.h"
#include "sync/sync_types.h"

namespace vxcore {

int GitSyncBackendCredentialCb(git_credential **out, const char *url,
                                const char *username_from_url,
                                unsigned int allowed_types, void *payload) {
  (void)url;
  auto *pl = static_cast<GitCredentialPayload *>(payload);
  if (pl == nullptr) {
    return GIT_PASSTHROUGH;
  }
  const std::size_t attempt = ++pl->callback_attempts;
  const bool has_url_username = username_from_url && *username_from_url;
  const bool has_pat = !pl->personal_access_token.empty();
  int rc = GIT_PASSTHROUGH;
  if (has_pat && (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0) {
    const char *username = has_url_username ? username_from_url : "x-access-token";
    rc = git_credential_userpass_plaintext_new(
        out, username, pl->personal_access_token.c_str());
  }
  if (attempt <= 3) {
    Logger::GetInstance().Log(
        rc == 0 || rc == GIT_PASSTHROUGH ? LogLevel::kInfo : LogLevel::kWarn,
        __FILE__, __LINE__,
        "GitSync worker=%llu event=credential_callback attempt=%zu allowed_types=%u "
        "username_source=%s pat_present=%d code=%d",
        GitSyncLog::WorkerId(), attempt, allowed_types,
        has_url_username ? "url" : "default", has_pat ? 1 : 0, rc);
  } else if (attempt == 4) {
    VXCORE_LOG_INFO("GitSync worker=%llu event=credential_callback_suppressed limit=3",
                    GitSyncLog::WorkerId());
  }
  return rc;
}

std::string MaybeEmbedPatInUrl(const std::string &url, const std::string &pat) {
  if (pat.empty() || url.compare(0, 8, "https://") != 0) return url;
  const size_t scheme_end = 8;
  const size_t authority_end = url.find_first_of("/?#", scheme_end);
  const size_t at_pos = url.find('@', scheme_end);
  const bool has_userinfo = at_pos != std::string::npos &&
                            (authority_end == std::string::npos || at_pos < authority_end);
  if (has_userinfo && url.find(':', scheme_end) < at_pos) {
    return url;  // Do not replace an explicit password.
  }

  std::string result;
  result.reserve(url.size() + pat.size() * 3 + 16);
  if (has_userinfo && at_pos > scheme_end) {
    result.append(url, 0, at_pos);  // Preserve the URL-encoded account login.
  } else {
    result = "https://x-access-token";
  }
  result += ':';
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char ch : pat) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
      result += static_cast<char>(ch);
    } else {
      result += '%';
      result += hex[ch >> 4];
      result += hex[ch & 15];
    }
  }
  result += '@';
  result.append(url, has_userinfo ? at_pos + 1 : scheme_end, std::string::npos);
  return result;
}

RemoteCallbacksBundle MakeRemoteCallbacks(ICredentialProvider *provider,
                                          const std::string &remote_url,
                                          SyncCancellation *cancellation) {
  RemoteCallbacksBundle bundle;
  // Wave 6.3 (F4.4): snapshot creds from the provider at callback-build time.
  // The libgit2 thread never touches the provider — it only reads the
  // stack-local payload built here on the caller's thread. If the provider
  // is null or declines to supply creds, the payload PAT stays empty and
  // the callback returns GIT_PASSTHROUGH for anonymous transport.
  if (provider != nullptr) {
    SyncCredentials snapshot;
    if (provider->GetCredentials(remote_url, /*username_from_url=*/"x-access-token",
                                 &snapshot)) {
      bundle.payload.personal_access_token = snapshot.personal_access_token;
    }
  }
  // Inspect delimiters only; never extract or log the URL's username.
  const auto scheme = remote_url.find("://");
  const auto authority_begin = scheme == std::string::npos ? 0 : scheme + 3;
  const auto authority_end = remote_url.find_first_of("/?#", authority_begin);
  const auto at = remote_url.find('@', authority_begin);
  const bool has_url_username = at != std::string::npos && at > authority_begin &&
      (authority_end == std::string::npos || at < authority_end) &&
      remote_url[authority_begin] != ':';
  VXCORE_LOG_INFO(
      "GitSync worker=%llu event=credential_setup provider_present=%d pat_present=%d "
      "url_username_present=%d",
      GitSyncLog::WorkerId(), provider != nullptr ? 1 : 0,
      bundle.payload.personal_access_token.empty() ? 0 : 1, has_url_username ? 1 : 0);

  // W12.1: optional cancellation token rides in the same payload as the
  // credential snapshot (libgit2 dispatches one payload pointer to all
  // callbacks in a git_remote_callbacks struct).
  bundle.payload.cancellation = cancellation;
  bundle.callbacks = GIT_REMOTE_CALLBACKS_INIT;
  bundle.callbacks.payload = &bundle.payload;
  bundle.callbacks.credentials = &GitSyncBackendCredentialCb;
  // Cancellation shims — install ONLY when a token is actually wired.
  // libgit2's `git_remote_fetch` can change its internal codepath when
  // `transfer_progress` is non-null even for trivial local file:// remotes
  // (observed: the checkout step skipped after a successful fetch when an
  // always-return-0 shim was installed unconditionally). Keeping the field
  // null in the no-token case preserves pre-W12 behavior bit-for-bit.
  if (cancellation != nullptr) {
    bundle.callbacks.transfer_progress =
        [](const git_indexer_progress *stats, void *payload) -> int {
          auto *pl = static_cast<GitCredentialPayload *>(payload);
          if (pl == nullptr) return 0;
          return SyncCancellation::Libgit2ProgressCheck(stats, pl->cancellation);
        };
    bundle.callbacks.push_transfer_progress =
        [](unsigned int current, unsigned int total, std::size_t bytes,
           void *payload) -> int {
          auto *pl = static_cast<GitCredentialPayload *>(payload);
          if (pl == nullptr) return 0;
          return SyncCancellation::Libgit2PushProgressCheck(current, total, bytes,
                                                            pl->cancellation);
        };
  }
  return bundle;
}

}  // namespace vxcore
