#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "core/context.h"
#include "mock_sync_backend.h"
#include "sync/credential_provider.h"
#include "sync/git/git_sync_backend.h"
#include "sync/sync_backend.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

using namespace vxcore;

namespace {

// Proxy bridges shared ownership across the unique_ptr factory boundary.
// SyncManager owns the proxy (via unique_ptr); the test owns the underlying
// MockSyncBackend via shared_ptr; the proxy also holds a shared_ptr so the
// mock stays alive until BOTH go out of scope. Avoids the previous
// double-delete bug where the same MockSyncBackend* was held by both a
// shared_ptr in the test AND a unique_ptr in SyncManager.
class MockBackendProxy : public vxcore::ISyncBackend {
 public:
  explicit MockBackendProxy(std::shared_ptr<vxcore::MockSyncBackend> target)
      : target_(std::move(target)) {}

  std::string GetName() const override { return target_->GetName(); }
  vxcore::SyncCapabilities GetCapabilities() const override {
    return target_->GetCapabilities();
  }
  bool IsInitialized() const override { return target_->IsInitialized(); }
  VxCoreError Initialize(const std::string &root_folder,
                                 const vxcore::SyncConfig &config) override {
    return target_->Initialize(root_folder, config);
  }
  void ReplaceCredsProvider(
      std::shared_ptr<vxcore::ICredentialProvider> p) override {
    target_->ReplaceCredsProvider(std::move(p));
  }
  std::shared_ptr<vxcore::ICredentialProvider> GetCredsProviderSnapshot() const override {
    return target_->GetCredsProviderSnapshot();
  }
  VxCoreError Sync(vxcore::SyncProgressCallback cb, void *ud) override {
    return target_->Sync(cb, ud);
  }
  VxCoreError GetStatus(std::vector<vxcore::SyncFileInfo> &out) override {
    return target_->GetStatus(out);
  }
  VxCoreError GetConflicts(
      std::vector<vxcore::SyncConflictInfo> &out) override {
    return target_->GetConflicts(out);
  }
  VxCoreError ResolveConflict(
      const std::string &path, vxcore::SyncConflictResolution r) override {
    return target_->ResolveConflict(path, r);
  }

 private:
  std::shared_ptr<vxcore::MockSyncBackend> target_;
};

}  // namespace

// Helper class to wrap a shared_ptr<MockSyncBackend> in a copy-constructible factory.
// Returns a fresh MockBackendProxy on each call; the proxy owns its own shared_ptr
// to the underlying mock so the mock outlives both the test's shared_ptr and the
// SyncManager-owned unique_ptr.
class MockBackendFactory {
 public:
  explicit MockBackendFactory(std::shared_ptr<MockSyncBackend> mock)
      : mock_(std::move(mock)) {}

  std::unique_ptr<ISyncBackend> operator()(
      const SyncConfig &, std::shared_ptr<vxcore::ICredentialProvider>) const {
    return std::make_unique<MockBackendProxy>(mock_);
  }

 private:
  std::shared_ptr<MockSyncBackend> mock_;
};

using namespace vxcore;

int test_sync_credentials_default_fields() {
  std::cout << "  Running test_sync_credentials_default_fields..." << std::endl;
  SyncCredentials c;
  ASSERT_TRUE(c.personal_access_token.empty());
  ASSERT_TRUE(c.author_name.empty());
  ASSERT_TRUE(c.author_email.empty());
  std::cout << "  \xE2\x9C\x93 test_sync_credentials_default_fields passed" << std::endl;
  return 0;
}

int test_mock_sync_backend_records() {
  std::cout << "  Running test_mock_sync_backend_records..." << std::endl;
  MockSyncBackend mb;

  ASSERT_EQ(mb.Sync(nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(mb.sync_call_count, 1);

  // Wave 6.3 F4.4: SetCredentials is gone; rotation goes through
  // ReplaceCredsProvider which records the call and stores the provider
  // for snapshot retrieval.
  auto provider = std::make_shared<vxcore::InMemoryCredentialProvider>(SyncCredentials{});
  mb.ReplaceCredsProvider(provider);
  ASSERT_EQ(mb.replace_creds_provider_call_count, 1);
  ASSERT_TRUE(mb.GetCredsProviderSnapshot().get() == provider.get());

  std::cout << "  \xE2\x9C\x93 test_mock_sync_backend_records passed" << std::endl;
  return 0;
}

int test_helper_create_bare_repo() {
  std::cout << "  Running test_helper_create_bare_repo..." << std::endl;
  std::string bare_path = get_test_path("test_helper_bare");
  std::string url = vxcore_test::create_bare_repo(bare_path);
  ASSERT_TRUE(url.rfind("file:///", 0) == 0);
  ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(bare_path + "/HEAD")));
  std::cout << "  \xE2\x9C\x93 test_helper_create_bare_repo passed" << std::endl;
  return 0;
}

int test_helper_commit_file_and_head_sha() {
  std::cout << "  Running test_helper_commit_file_and_head_sha..." << std::endl;
  std::string bare_path = get_test_path("test_helper_commit_bare");
  vxcore_test::create_bare_repo(bare_path);
  vxcore_test::commit_file(bare_path, "note.md", "hello", "first commit");
  std::string sha = vxcore_test::git_head_sha(bare_path);
  ASSERT_EQ(sha.length(), static_cast<size_t>(40));
  for (char c : sha) {
    ASSERT_TRUE(std::isxdigit(static_cast<unsigned char>(c)) != 0);
  }
  std::cout << "  \xE2\x9C\x93 test_helper_commit_file_and_head_sha passed" << std::endl;
  return 0;
}

int test_sync_trigger_dispatches_to_backend() {
  std::cout << "  Running test_sync_trigger_dispatches_to_backend..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_trigger");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d1\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered via BackendRegistration in
  // test_internals, so the registry path works. Use EnableSyncWithFactoryForTesting to inject a custom
  // mock instance for call tracking.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";
  
  // Create a mock and capture a pointer for call tracking.
  auto mock = std::make_shared<MockSyncBackend>();
  MockSyncBackend *mock_ptr = mock.get();
  
  // Factory that returns the captured mock instance.
  MockBackendFactory factory(mock);
  
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_OK);
  ASSERT_EQ(mock_ptr->sync_call_count, 1);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_trigger_dispatches_to_backend passed" << std::endl;
  return 0;
}

int test_sync_trigger_no_backend_returns_not_implemented() {
  std::cout << "  Running test_sync_trigger_no_backend_returns_not_implemented..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_no_backend");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d2\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Wave 4.4 / F1.1: the original "enable mock placeholder → trigger NOT_IMPLEMENTED"
  // semantic is no longer reachable (registry rejects unknown backends at enable time).
  // The "state-without-backend" branch in TriggerSync (VXCORE_ERR_NOT_IMPLEMENTED) is now
  // dead code from the public API surface and only reachable via direct private-state
  // manipulation. This test asserts the new contract: enabling with a truly unknown
  // backend fails with UNKNOWN_BACKEND, and trigger reports SYNC_NOT_ENABLED.
  // (Note: "mock" is self-registered by tests/mock_sync_backend.cpp, so we use a
  // name that is guaranteed not to be in the registry.)
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"nonexistent_backend_xyz\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_ERR_UNKNOWN_BACKEND);
  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_ERR_SYNC_NOT_ENABLED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_trigger_no_backend_returns_not_implemented passed"
            << std::endl;
  return 0;
}

int test_sync_trigger_state_transitions() {
  std::cout << "  Running test_sync_trigger_state_transitions..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_state");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d3\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered. Use EnableSyncWithFactoryForTesting
  // to inject a custom mock instance for call tracking.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";
  
  auto mock = std::make_shared<MockSyncBackend>();
  mock->next_sync_result = VXCORE_ERR_SYNC_CONFLICT;
  MockSyncBackend *mock_ptr = mock.get();
  
  MockBackendFactory factory(mock);
  
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_ERR_SYNC_CONFLICT);

  char *status_json = nullptr;
  ASSERT_EQ(vxcore_sync_get_status(ctx, notebook_id, &status_json), VXCORE_OK);
  std::string s(status_json);
  ASSERT_TRUE(s.find("\"state\":\"conflicted\"") != std::string::npos);
  vxcore_string_free(status_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_trigger_state_transitions passed" << std::endl;
  return 0;
}

int test_sync_status_returns_files_from_backend() {
  std::cout << "  Running test_sync_status_returns_files_from_backend..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_status");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d4\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered. Use EnableSyncWithFactoryForTesting
  // to inject a custom mock instance with canned status.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";
  
  auto mock = std::make_shared<MockSyncBackend>();
  mock->canned_status.push_back({"a.md", SyncFileStatus::kModifiedLocal});
  mock->canned_status.push_back({"b.md", SyncFileStatus::kAddedLocal});
  
  MockBackendFactory factory(mock);
  
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  char *status_json = nullptr;
  ASSERT_EQ(vxcore_sync_get_status(ctx, notebook_id, &status_json), VXCORE_OK);
  std::string s(status_json);
  ASSERT_TRUE(s.find("\"a.md\"") != std::string::npos);
  ASSERT_TRUE(s.find("\"b.md\"") != std::string::npos);
  vxcore_string_free(status_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_status_returns_files_from_backend passed" << std::endl;
  return 0;
}

int test_sync_conflicts_returns_backend_list() {
  std::cout << "  Running test_sync_conflicts_returns_backend_list..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_conflicts");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d5\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered. Use EnableSyncWithFactoryForTesting
  // to inject a custom mock instance with canned conflicts.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";
  
  auto mock = std::make_shared<MockSyncBackend>();
  SyncConflictInfo c;
  c.path = "a.md";
  c.local_modified_utc = 1000;
  c.remote_modified_utc = 2000;
  c.is_binary = false;
  mock->canned_conflicts.push_back(c);
  
  MockBackendFactory factory(mock);
  
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  char *conflicts_json = nullptr;
  ASSERT_EQ(vxcore_sync_get_conflicts(ctx, notebook_id, &conflicts_json), VXCORE_OK);
  std::string s(conflicts_json);
  ASSERT_TRUE(s.find("\"a.md\"") != std::string::npos);
  vxcore_string_free(conflicts_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_conflicts_returns_backend_list passed" << std::endl;
  return 0;
}

int test_sync_resolve_dispatches_to_backend() {
  std::cout << "  Running test_sync_resolve_dispatches_to_backend..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_resolve");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d6\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered. Use EnableSyncWithFactoryForTesting
  // to inject a custom mock instance for call tracking.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";
  
  auto mock = std::make_shared<MockSyncBackend>();
  MockSyncBackend *mock_ptr = mock.get();
  
  MockBackendFactory factory(mock);
  
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  ASSERT_EQ(vxcore_sync_resolve_conflict(ctx, notebook_id, "x.md", "keep_local"), VXCORE_OK);
  ASSERT_EQ(mock_ptr->resolve_conflict_call_count, 1);
  ASSERT_TRUE(mock_ptr->last_resolve_path == "x.md");
  ASSERT_TRUE(mock_ptr->last_resolve_resolution == SyncConflictResolution::kKeepLocal);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_resolve_dispatches_to_backend passed" << std::endl;
  return 0;
}

int test_sync_resolve_clears_conflicted_state_when_no_more_conflicts() {
  std::cout << "  Running test_sync_resolve_clears_conflicted_state_when_no_more_conflicts..."
            << std::endl;
  std::string path = get_test_path("test_sync_dispatch_resolve_clear");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"d7\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered. Use EnableSyncWithFactoryForTesting
  // to inject a custom mock instance for call tracking.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";
  
  auto mock = std::make_shared<MockSyncBackend>();
  mock->next_sync_result = VXCORE_ERR_SYNC_CONFLICT;
  MockSyncBackend *mock_ptr = mock.get();
  
  MockBackendFactory factory(mock);
  
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  // Trigger -> state becomes conflicted
  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_ERR_SYNC_CONFLICT);

  // No more conflicts after resolve.
  mock_ptr->canned_conflicts.clear();
  mock_ptr->next_resolve_conflict_result = VXCORE_OK;

  ASSERT_EQ(vxcore_sync_resolve_conflict(ctx, notebook_id, "x.md", "keep_local"), VXCORE_OK);

  char *status_json = nullptr;
  ASSERT_EQ(vxcore_sync_get_status(ctx, notebook_id, &status_json), VXCORE_OK);
  std::string s(status_json);
  ASSERT_TRUE(s.find("\"state\":\"idle\"") != std::string::npos);
  vxcore_string_free(status_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_resolve_clears_conflicted_state_when_no_more_conflicts "
               "passed"
            << std::endl;
  return 0;
}

int test_sync_manager_update_credentials_dispatches() {
  std::cout << "  Running test_sync_manager_update_credentials_dispatches..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_updatecred");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"sc1\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncConfig cfg;
  cfg.backend = "mock";
  cfg.remote_url = "file:///tmp/x";

  auto mock = std::make_shared<MockSyncBackend>();
  MockSyncBackend *mock_ptr = mock.get();

  MockBackendFactory factory(mock);

  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, cfg, nullptr, factory),
            VXCORE_OK);

  // Baseline: no rotation yet.
  ASSERT_EQ(mock_ptr->replace_creds_provider_call_count, 0);

  SyncCredentials creds;
  creds.personal_access_token = "abc123";
  auto provider = std::make_shared<vxcore::InMemoryCredentialProvider>(creds);
  ASSERT_EQ(ctx_impl->sync_manager->UpdateCredentials(notebook_id, provider), VXCORE_OK);
  ASSERT_EQ(mock_ptr->replace_creds_provider_call_count, 1);
  ASSERT_TRUE(mock_ptr->GetCredsProviderSnapshot().get() == provider.get());

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_manager_update_credentials_dispatches passed"
            << std::endl;
  return 0;
}

// Wave 6.3 F4.4: the legacy "no backend" SetCredentials branch
// (VXCORE_ERR_NOT_IMPLEMENTED) is unreachable for UpdateCredentials in
// practice — see problems.md. The narrower SYNC_NOT_ENABLED contract for
// "enabled but no backend" remains exercised via the
// update_credentials_not_enabled test below.

int test_sync_manager_update_credentials_unknown_notebook_returns_not_found() {
  std::cout << "  Running test_sync_manager_update_credentials_unknown_notebook_returns_not_found..."
            << std::endl;

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto provider = std::make_shared<vxcore::InMemoryCredentialProvider>(SyncCredentials{});
  ASSERT_EQ(ctx_impl->sync_manager->UpdateCredentials("nonexistent-id-xyz", provider),
            VXCORE_ERR_NOT_FOUND);

  vxcore_context_destroy(ctx);
  std::cout
      << "  \xE2\x9C\x93 test_sync_manager_update_credentials_unknown_notebook_returns_not_found "
         "passed"
      << std::endl;
  return 0;
}

int test_sync_manager_update_credentials_not_enabled_returns_not_enabled() {
  std::cout << "  Running test_sync_manager_update_credentials_not_enabled_returns_not_enabled..."
            << std::endl;
  std::string path = get_test_path("test_sync_dispatch_updatecred_notenabled");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"sc4\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto provider = std::make_shared<vxcore::InMemoryCredentialProvider>(SyncCredentials{});
  ASSERT_EQ(ctx_impl->sync_manager->UpdateCredentials(notebook_id, provider),
            VXCORE_ERR_SYNC_NOT_ENABLED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout
      << "  \xE2\x9C\x93 test_sync_manager_update_credentials_not_enabled_returns_not_enabled passed"
      << std::endl;
  return 0;
}

int test_git_sync_backend_construct_destruct() {
  std::cout << "  Running test_git_sync_backend_construct_destruct..." << std::endl;
  // Smoke-test ctor/dtor + LibGit2Init ref-counting (libgit2_ is the FIRST member,
  // so it is destroyed LAST — guarantees libgit2 is still initialized while other
  // dtors run). Stack instance:
  {
    vxcore::GitSyncBackend stack_backend;
    (void)stack_backend;
  }
  // Heap instance:
  auto *heap_backend = new vxcore::GitSyncBackend();
  delete heap_backend;
  std::cout << "  \xE2\x9C\x93 test_git_sync_backend_construct_destruct passed" << std::endl;
  return 0;
}

int test_sync_enable_creates_git_backend() {
  std::cout << "  Running test_sync_enable_creates_git_backend..." << std::endl;
  std::string path = get_test_path("test_sync_enable_git_factory");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"gf1\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // After T15: GitSyncBackend::Initialize succeeds for the clone branch even when
  // the file:// URL doesn't exist (libgit2 reports the missing path → fetch fails →
  // TranslateGitError surfaces NOT_FOUND/NETWORK). Either error proves the factory
  // ran AND Initialize was attempted; with rollback, the state is gone.
  VxCoreError enable_rc = vxcore_sync_enable(
      ctx, notebook_id,
      "{\"backend\":\"git\",\"remoteUrl\":\"file:///tmp/nonexistent_xyz\"}");
  ASSERT_TRUE(enable_rc != VXCORE_OK);  // factory ran, Initialize failed, rollback fired

  // Verify rollback: state should be absent, so GetStatus reports SYNC_NOT_ENABLED.
  char *status_json = nullptr;
  ASSERT_EQ(vxcore_sync_get_status(ctx, notebook_id, &status_json),
            VXCORE_ERR_SYNC_NOT_ENABLED);
  if (status_json) vxcore_string_free(status_json);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_enable_creates_git_backend passed" << std::endl;
  return 0;
}

int test_sync_enable_with_credentials_forwards_provider() {
  std::cout << "  Running test_sync_enable_with_credentials_forwards_provider..." << std::endl;
  std::string path = get_test_path("test_sync_enable_creds_order");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"gf2\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  auto mock = std::make_shared<MockSyncBackend>();
  mock->next_initialize_result = VXCORE_OK;
  MockSyncBackend *mock_ptr = mock.get();

  SyncConfig config;
  config.backend = "mock";
  config.remote_url = "file:///tmp/x";

  SyncCredentials creds;
  creds.personal_access_token = "abc";
  auto provider = std::make_shared<vxcore::InMemoryCredentialProvider>(creds);

  MockBackendFactory factory(mock);

  // Wave 6.3 F4.4: the credential provider is forwarded through the factory
  // — the proxy mock pulls it via ReplaceCredsProvider on construction, so
  // the snapshot reflects the same provider. The proxy ctor wiring records
  // the call once, so we cleared the counter inside the factory lambda
  // (legacy mock at tests/mock_sync_backend.cpp) — the proxy used here
  // forwards through ReplaceCredsProvider but does NOT clear, so for THIS
  // test we just assert the provider arrived (snapshot equality).
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithFactoryForTesting(
                notebook_id, config, provider, factory),
            VXCORE_OK);

  ASSERT_EQ(mock_ptr->initialize_call_count, 1);
  ASSERT_TRUE(mock_ptr->GetCredsProviderSnapshot().get() == provider.get());

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_enable_with_credentials_forwards_provider passed"
            << std::endl;
  return 0;
}

int test_sync_enable_unknown_backend_no_factory() {
  std::cout << "  Running test_sync_enable_unknown_backend_no_factory..." << std::endl;
  std::string path = get_test_path("test_sync_enable_unknown");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"gf3\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Unknown backend: factory rejects with VXCORE_ERR_UNKNOWN_BACKEND (B3 fail-fast).
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"unknown_xyz\",\"remoteUrl\":\"x\"}"),
            VXCORE_ERR_UNKNOWN_BACKEND);

  // Sync was never enabled, so trigger returns SYNC_NOT_ENABLED.
  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_ERR_SYNC_NOT_ENABLED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_enable_unknown_backend_no_factory passed" << std::endl;
  return 0;
}

// Two default-gitignore/gitattributes tests previously lived here; they
// moved to test_git_sync_init.cpp during T14 (git-folder-restructure plan)
// because they now exercise vxcore::WriteIfMissing +
// vxcore::BuildGitignoreContent directly instead of the removed
// GitSyncBackend::WriteDefaultIgnoreAndAttributesForTesting shim.

int main() {
  std::cout << "Running sync manager dispatch tests...\n";
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();
  RUN_TEST(test_sync_credentials_default_fields);
  RUN_TEST(test_mock_sync_backend_records);
  RUN_TEST(test_helper_create_bare_repo);
  RUN_TEST(test_helper_commit_file_and_head_sha);
  RUN_TEST(test_sync_trigger_dispatches_to_backend);
  RUN_TEST(test_sync_trigger_no_backend_returns_not_implemented);
  RUN_TEST(test_sync_trigger_state_transitions);
  RUN_TEST(test_sync_status_returns_files_from_backend);
  RUN_TEST(test_sync_conflicts_returns_backend_list);
  RUN_TEST(test_sync_resolve_dispatches_to_backend);
  RUN_TEST(test_sync_resolve_clears_conflicted_state_when_no_more_conflicts);
  RUN_TEST(test_sync_manager_update_credentials_dispatches);
  RUN_TEST(test_sync_manager_update_credentials_unknown_notebook_returns_not_found);
  RUN_TEST(test_sync_manager_update_credentials_not_enabled_returns_not_enabled);
  RUN_TEST(test_git_sync_backend_construct_destruct);
  RUN_TEST(test_sync_enable_creates_git_backend);
  RUN_TEST(test_sync_enable_with_credentials_forwards_provider);
  RUN_TEST(test_sync_enable_unknown_backend_no_factory);
  std::cout << "All sync manager dispatch tests passed\n";
  return 0;
}
