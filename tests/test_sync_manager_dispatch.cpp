#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <memory>

#include "core/context.h"
#include "mock_sync_backend.h"
#include "sync/git_sync_backend.h"
#include "sync/sync_backend.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

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

int test_isync_backend_default_setcredentials_returns_ok() {
  std::cout << "  Running test_isync_backend_default_setcredentials_returns_ok..." << std::endl;
  MockSyncBackend mb;
  ASSERT_EQ(mb.SetCredentials(SyncCredentials{}), VXCORE_OK);
  std::cout << "  \xE2\x9C\x93 test_isync_backend_default_setcredentials_returns_ok passed"
            << std::endl;
  return 0;
}

int test_mock_sync_backend_records() {
  std::cout << "  Running test_mock_sync_backend_records..." << std::endl;
  MockSyncBackend mb;

  ASSERT_EQ(mb.Sync(nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(mb.sync_call_count, 1);

  ASSERT_EQ(mb.Push(nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(mb.push_call_count, 1);

  SyncCredentials creds;
  creds.personal_access_token = "abc";
  ASSERT_EQ(mb.SetCredentials(creds), VXCORE_OK);
  ASSERT_EQ(mb.set_credentials_call_count, 1);
  ASSERT_TRUE(mb.last_credentials.personal_access_token == "abc");

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  MockSyncBackend *mock_ptr = mock.get();
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_ERR_NOT_IMPLEMENTED);

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  mock->next_sync_result = VXCORE_ERR_SYNC_CONFLICT;
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  mock->canned_status.push_back({"a.md", SyncFileStatus::kModifiedLocal});
  mock->canned_status.push_back({"b.md", SyncFileStatus::kAddedLocal});
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  SyncConflictInfo c;
  c.path = "a.md";
  c.local_modified_utc = 1000;
  c.remote_modified_utc = 2000;
  c.is_binary = false;
  mock->canned_conflicts.push_back(c);
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  MockSyncBackend *mock_ptr = mock.get();
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

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
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  mock->next_sync_result = VXCORE_ERR_SYNC_CONFLICT;
  MockSyncBackend *mock_ptr = mock.get();
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

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

int test_sync_manager_set_credentials_dispatches() {
  std::cout << "  Running test_sync_manager_set_credentials_dispatches..." << std::endl;
  std::string path = get_test_path("test_sync_dispatch_setcred");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"sc1\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  MockSyncBackend *mock_ptr = mock.get();
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ctx_impl->sync_manager->RegisterBackendForTesting(notebook_id, std::move(mock));

  SyncCredentials creds;
  creds.personal_access_token = "abc123";
  ASSERT_EQ(ctx_impl->sync_manager->SetCredentials(notebook_id, creds), VXCORE_OK);
  ASSERT_EQ(mock_ptr->set_credentials_call_count, 1);
  ASSERT_TRUE(mock_ptr->last_credentials.personal_access_token == "abc123");

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_manager_set_credentials_dispatches passed" << std::endl;
  return 0;
}

int test_sync_manager_set_credentials_no_backend_returns_not_implemented() {
  std::cout << "  Running test_sync_manager_set_credentials_no_backend_returns_not_implemented..."
            << std::endl;
  std::string path = get_test_path("test_sync_dispatch_setcred_nobackend");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"sc2\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncCredentials creds;
  creds.personal_access_token = "tkn";
  ASSERT_EQ(ctx_impl->sync_manager->SetCredentials(notebook_id, creds),
            VXCORE_ERR_NOT_IMPLEMENTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout
      << "  \xE2\x9C\x93 test_sync_manager_set_credentials_no_backend_returns_not_implemented "
         "passed"
      << std::endl;
  return 0;
}

int test_sync_manager_set_credentials_unknown_notebook_returns_not_found() {
  std::cout << "  Running test_sync_manager_set_credentials_unknown_notebook_returns_not_found..."
            << std::endl;

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncCredentials creds;
  ASSERT_EQ(ctx_impl->sync_manager->SetCredentials("nonexistent-id-xyz", creds),
            VXCORE_ERR_NOT_FOUND);

  vxcore_context_destroy(ctx);
  std::cout
      << "  \xE2\x9C\x93 test_sync_manager_set_credentials_unknown_notebook_returns_not_found "
         "passed"
      << std::endl;
  return 0;
}

int test_sync_manager_set_credentials_not_enabled_returns_not_enabled() {
  std::cout << "  Running test_sync_manager_set_credentials_not_enabled_returns_not_enabled..."
            << std::endl;
  std::string path = get_test_path("test_sync_dispatch_setcred_notenabled");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"sc4\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  SyncCredentials creds;
  ASSERT_EQ(ctx_impl->sync_manager->SetCredentials(notebook_id, creds),
            VXCORE_ERR_SYNC_NOT_ENABLED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout
      << "  \xE2\x9C\x93 test_sync_manager_set_credentials_not_enabled_returns_not_enabled passed"
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

int test_sync_enable_with_credentials_calls_setcred_first() {
  std::cout << "  Running test_sync_enable_with_credentials_calls_setcred_first..." << std::endl;
  std::string path = get_test_path("test_sync_enable_creds_order");
  cleanup_test_dir(path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, path.c_str(), "{\"name\":\"gf2\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  auto mock = std::make_unique<MockSyncBackend>();
  mock->next_initialize_result = VXCORE_OK;
  MockSyncBackend *mock_ptr = mock.get();

  SyncConfig config;
  config.backend = "mock";
  config.remote_url = "file:///tmp/x";

  SyncCredentials creds;
  creds.personal_access_token = "abc";

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_EQ(ctx_impl->sync_manager->EnableSyncWithBackendForTesting(
                notebook_id, config, &creds, std::move(mock)),
            VXCORE_OK);

  ASSERT_EQ(mock_ptr->set_credentials_call_count, 1);
  ASSERT_EQ(mock_ptr->initialize_call_count, 1);
  // Proves SetCredentials ran BEFORE Initialize.
  ASSERT_EQ(mock_ptr->set_credentials_call_count_at_initialize, 1);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_enable_with_credentials_calls_setcred_first passed"
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

  // Unknown backend: factory ignores it, just stores config + state.
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"unknown_xyz\",\"remoteUrl\":\"x\"}"),
            VXCORE_OK);

  // No backend registered, so trigger returns NOT_IMPLEMENTED.
  ASSERT_EQ(vxcore_sync_trigger(ctx, notebook_id), VXCORE_ERR_NOT_IMPLEMENTED);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(path);
  std::cout << "  \xE2\x9C\x93 test_sync_enable_unknown_backend_no_factory passed" << std::endl;
  return 0;
}

int test_git_sync_writes_default_gitignore_attributes_when_missing() {
  std::cout << "  Running test_git_sync_writes_default_gitignore_attributes_when_missing..."
            << std::endl;
  std::string dir = get_test_path("test_git_sync_defaults_missing");
  cleanup_test_dir(dir);
  std::filesystem::create_directories(vxcore::PathFromUtf8(dir));

  SyncConfig config;
  config.exclude_paths = {"*.bak", "scratch/"};

  int written = GitSyncBackend::WriteDefaultIgnoreAndAttributesForTesting(dir, config);
  ASSERT_EQ(written, 2);

  std::string gi_path = dir + "/.gitignore";
  std::string ga_path = dir + "/.gitattributes";
  ASSERT_TRUE(vxcore::PathExists(gi_path));
  ASSERT_TRUE(vxcore::PathExists(ga_path));

  {
    std::ifstream ifs(vxcore::PathFromUtf8(gi_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("vx_notebook/vx_sync/") != std::string::npos);
    ASSERT_TRUE(content.find("*.bak") != std::string::npos);
    ASSERT_TRUE(content.find("scratch/") != std::string::npos);
  }
  {
    std::ifstream ifs(vxcore::PathFromUtf8(ga_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("text=auto eol=lf") != std::string::npos);
    ASSERT_TRUE(content.find("*.png binary") != std::string::npos);
  }

  cleanup_test_dir(dir);
  std::cout
      << "  \xE2\x9C\x93 test_git_sync_writes_default_gitignore_attributes_when_missing passed"
      << std::endl;
  return 0;
}

int test_git_sync_preserves_existing_gitignore() {
  std::cout << "  Running test_git_sync_preserves_existing_gitignore..." << std::endl;
  std::string dir = get_test_path("test_git_sync_defaults_preserve");
  cleanup_test_dir(dir);
  std::filesystem::create_directories(vxcore::PathFromUtf8(dir));

  std::string gi_path = dir + "/.gitignore";
  std::string ga_path = dir + "/.gitattributes";
  const std::string user_content = "# user customization\n*.user\n";
  {
    std::ofstream ofs(vxcore::PathFromUtf8(gi_path), std::ios::binary | std::ios::trunc);
    ofs.write(user_content.data(), static_cast<std::streamsize>(user_content.size()));
  }

  SyncConfig config;
  int written = GitSyncBackend::WriteDefaultIgnoreAndAttributesForTesting(dir, config);
  ASSERT_EQ(written, 1);

  {
    std::ifstream ifs(vxcore::PathFromUtf8(gi_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content == user_content);
  }
  ASSERT_TRUE(vxcore::PathExists(ga_path));
  {
    std::ifstream ifs(vxcore::PathFromUtf8(ga_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("text=auto eol=lf") != std::string::npos);
  }

  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_git_sync_preserves_existing_gitignore passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running sync manager dispatch tests...\n";
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();
  RUN_TEST(test_sync_credentials_default_fields);
  RUN_TEST(test_isync_backend_default_setcredentials_returns_ok);
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
  RUN_TEST(test_sync_manager_set_credentials_dispatches);
  RUN_TEST(test_sync_manager_set_credentials_no_backend_returns_not_implemented);
  RUN_TEST(test_sync_manager_set_credentials_unknown_notebook_returns_not_found);
  RUN_TEST(test_sync_manager_set_credentials_not_enabled_returns_not_enabled);
  RUN_TEST(test_git_sync_backend_construct_destruct);
  RUN_TEST(test_sync_enable_creates_git_backend);
  RUN_TEST(test_sync_enable_with_credentials_calls_setcred_first);
  RUN_TEST(test_sync_enable_unknown_backend_no_factory);
  RUN_TEST(test_git_sync_writes_default_gitignore_attributes_when_missing);
  RUN_TEST(test_git_sync_preserves_existing_gitignore);
  std::cout << "All sync manager dispatch tests passed\n";
  return 0;
}
