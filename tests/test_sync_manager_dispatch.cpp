#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>

#include <memory>

#include "core/context.h"
#include "mock_sync_backend.h"
#include "sync/sync_backend.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
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
  std::cout << "All sync manager dispatch tests passed\n";
  return 0;
}
