include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/git_sync_sources.cmake)

# Shared static-library test helpers (Task 0.1 of sync-backend-phase4 plan).
# Must be added BEFORE add_vxcore_test() invocations so the target exists
# when INTERNALS-flagged tests reference it.
add_subdirectory(test_internals)

# add_vxcore_test(<name> [INTERNALS])
#   INTERNALS  Link against vxcore_test_internals static lib (sync test helpers,
#              mocks, etc.). Use for tests that need shared sync-test fixtures.
function(add_vxcore_test test_name)
    set(options INTERNALS)
    cmake_parse_arguments(VXCORE_TEST "${options}" "" "" ${ARGN})

    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE vxcore)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
    if(VXCORE_TEST_INTERNALS)
        target_link_libraries(${test_name} PRIVATE vxcore_test_internals)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_vxcore_test(test_core)
add_vxcore_test(test_error_codes)
add_vxcore_test(test_notebook)

# test_session_record_readonly: Tests NotebookRecord serialization.
# Needs internal C++ symbols so we link against vxcore library via add_vxcore_test.
add_vxcore_test(test_session_record_readonly)

add_vxcore_test(test_folder)

# test_folder_child_order: Tasks T1+T5 of notebook-explorer-drag-reorder plan.
# Covers vxcore_folder_set_children_order — atomic reorder of a folder's
# children with permutation validation, no-op detection, and dedicated
# folder.children_reordered event emission. Uses only the public C API
# (vxcore.dll + vxcore_on_event subscription), so links via add_vxcore_test()
# rather than direct-compile.
add_vxcore_test(test_folder_child_order)
target_link_libraries(test_folder_child_order PRIVATE nlohmann_json)

# test_bundled_folder_manager_read_only: T9 of open-notebook-remote-readonly
# plan. Verifies that every public mutator on BundledFolderManager honors the
# Notebook::IsReadOnly() guard (returns VXCORE_ERR_READ_ONLY + leaves disk
# state unchanged) and that the same mutators still succeed once the
# notebook is flipped back to writable. Source guards: 21 sites in
# src/core/bundled_folder_manager.cpp (commit b7f13c5).
add_vxcore_test(test_bundled_folder_manager_read_only)

add_vxcore_test(test_node)

# test_missing_nodes: T4 of the missing-files-on-disk plan. Verifies the
# reactive missing-content gate on the bundled read/access C-APIs
# (vxcore_node_get_config / vxcore_node_get_metadata / vxcore_buffer_open)
# returns VXCORE_ERR_NODE_NOT_EXISTS when an indexed node's content is gone
# from disk, while vxcore_node_unindex still removes the phantom and raw
# notebooks stay unaffected. Public C-API only, so links via add_vxcore_test().
add_vxcore_test(test_missing_nodes)

add_vxcore_test(test_search)
add_vxcore_test(test_tag_sync)
add_vxcore_test(test_filetype)
add_vxcore_test(test_workspace)
add_vxcore_test(test_buffer)
add_vxcore_test(test_buffer_save_read_only)
add_vxcore_test(test_notebook_mutators_read_only)
# test_notebook_open_ex: T14 of open-notebook-remote-readonly plan. Verifies
# the new vxcore_notebook_open_ex C API parses {"readOnly":true} from the
# options JSON, flips the notebook's runtime SetReadOnly state, persists
# read_only into the matching NotebookRecord in session.json, treats
# malformed options_json as VXCORE_ERR_JSON_PARSE without registering a
# notebook, and that the legacy vxcore_notebook_open shim still works
# unchanged. Links via add_vxcore_test() (vxcore.dll provides both the
# C API and the session persistence layer).
add_vxcore_test(test_notebook_open_ex)
target_link_libraries(test_notebook_open_ex PRIVATE nlohmann_json)
# test_notebook_record_reconcile: Part B of fix-notebook-reappears-id-mismatch.
# Regression test proving the "closed notebook reappears after restart" bug
# stays fixed: exercises the NotebookManager defense-in-depth fix (A1
# create-on-existing id preservation, A2 close-by-id-OR-root, A4 load-time id
# reconcile + same-root dedupe) through the public C API only. Links via
# add_vxcore_test() (vxcore.dll provides the C API + session persistence);
# nlohmann_json for the JSON crafting of session/config files.
add_vxcore_test(test_notebook_record_reconcile)
target_link_libraries(test_notebook_record_reconcile PRIVATE nlohmann_json)
add_vxcore_test(test_history)
add_vxcore_test(test_log_handler)
add_vxcore_test(test_session_persistence)
add_vxcore_test(test_template)
add_vxcore_test(test_e2e_assets)
add_vxcore_test(test_sync INTERNALS)
add_vxcore_test(test_sync_config_backcompat)
add_vxcore_test(test_notebook_readonly_flag)
add_vxcore_test(test_raw_folder_manager_read_only)
add_vxcore_test(test_sync_status_queries INTERNALS)
add_vxcore_test(test_sync_config_cache INTERNALS)
add_vxcore_test(test_vxcore_sync_is_registered INTERNALS)
add_vxcore_test(test_sync_unregister INTERNALS)
add_vxcore_test(test_work_queue)
add_vxcore_test(test_event_manager INTERNALS)
add_vxcore_test(test_metadata_events)
add_vxcore_test(test_mock_sync_backend INTERNALS)

# test_dirty_tracker: Task 9.1 (F2.4) of sync-backend-phase4. Standalone
# DirtyTracker class extracted from SyncManager. Its symbols are NOT
# VXCORE_API-exported, so we direct-compile dirty_tracker.cpp into the test
# rather than linking via vxcore.dll. No libgit2, no Qt deps, no INTERNALS
# helper needed (uses only the public sync header).
add_executable(test_dirty_tracker
    test_dirty_tracker.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/dirty_tracker.cpp
)
target_include_directories(test_dirty_tracker PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
add_test(NAME test_dirty_tracker COMMAND test_dirty_tracker)

# test_retry_policy: F5.10 / Task 11.1 of sync-backend-phase4. Tests the
# RetryPolicy struct + compute_delay + is_retryable_network_error.
# retry_policy.cpp lives in vxcore.dll's source list but its free functions
# carry no VXCORE_API decorations, so we direct-compile (mirrors
# test_dirty_tracker). No libgit2 / Qt deps.
add_executable(test_retry_policy
    test_retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
)
target_include_directories(test_retry_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
add_test(NAME test_retry_policy COMMAND test_retry_policy)

# test_git_error_translator: W11.2 / Task 11.2 (B3) of sync-backend-phase4.
# Tests ClassifyGitOp + GitOpErrorMessage. ClassifyGitOp is the ONLY function
# allowed to inspect English libgit2 error-message text -- callers across
# vxcore must switch on GitOpError. Direct-compile pattern: the symbols are
# not VXCORE_API-exported. Pulls in libgit2 (for git_error_last during klass
# inspection) and logger/file_utils (used by VXCORE_LOG_ERROR inside
# TranslateGitError, also compiled here for completeness). Mirrors the
# test_libgit2_init scaffolding (no vxcore.dll dependency).
add_executable(test_git_error_translator
    test_git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_git_error_translator PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_error_translator PRIVATE libgit2package)
add_test(NAME test_git_error_translator COMMAND test_git_error_translator)

# test_sync_cancellation: F5.9 / Task 12.1 of sync-backend-phase4. Tests the
# SyncCancellation atomic token + libgit2 progress shims. Direct-compile
# (sync_cancellation.cpp's symbols are not VXCORE_API-exported). No libgit2,
# no Qt deps -- the shims only dereference the token payload.
add_executable(test_sync_cancellation
    test_sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp)
target_include_directories(test_sync_cancellation PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_sync_cancellation PRIVATE Threads::Threads)
endif()
add_test(NAME test_sync_cancellation COMMAND test_sync_cancellation)

# test_progress_dispatcher: Task 13.1 (F5.7 part 2) of sync-backend-phase4.
# Tests the SyncProgressDispatcher thread-safe progress fan-out. Direct-compile
# (sync_progress_dispatcher.cpp's symbols are not VXCORE_API-exported). No
# libgit2, no Qt deps -- pure std::function/std::mutex coverage.
add_executable(test_progress_dispatcher
    test_progress_dispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_progress_dispatcher.cpp)
target_include_directories(test_progress_dispatcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_progress_dispatcher PRIVATE Threads::Threads)
endif()
add_test(NAME test_progress_dispatcher COMMAND test_progress_dispatcher)

# test_credential_provider: Task 6.1 (F4.4) of sync-backend-phase4. Tests the
# ICredentialProvider interface, InMemoryCredentialProvider (in vxcore.dll),
# and the migrated MockCredentialProvider (in vxcore_test_internals).
add_vxcore_test(test_credential_provider INTERNALS)

# test_sync_backend_registry: Task 4.2 (F1.1) of sync-backend-phase4. Tests
# the SyncBackendRegistry public surface (Register / Create / Names plus
# BackendRegistration token) against local lambda factories — does NOT
# instantiate GitSyncBackend, so links via add_vxcore_test() through
# vxcore.dll's VXCORE_API-exported registry symbols.
add_vxcore_test(test_sync_backend_registry)

# test_git_options: Task 5.4 (F1.5) of sync-backend-phase4. Pure value-type
# tests for GitOptions::FromJson — no libgit2, no SyncManager. Links via
# add_vxcore_test() through vxcore.dll's VXCORE_API-exported FromJson symbol.
add_vxcore_test(test_git_options)

# test_sync_manager_factory_override: Task 5.1 (F4.2) of sync-backend-phase4.
# Tests SyncManager::EnableSyncImpl factory-override overload via the
# EnableSyncWithFactoryForTesting hook. Uses MockSyncBackend (from
# vxcore_test_internals) so links via add_vxcore_test(... INTERNALS).
add_vxcore_test(test_sync_manager_factory_override INTERNALS)

# test_sync_stage_only: vxcore-sync-stage-only V2. Exercises SyncManager
# StageOnly / NetworkPhaseOnly via a fake backend (subclass of MockSyncBackend)
# that overrides StageAndCommit / FetchRebasePush. No libgit2, no network.
add_vxcore_test(test_sync_stage_only INTERNALS)

# test_sync_manager_reentrancy: Task 10.1 (F2.4 part 2) of sync-backend-phase4.
# Verifies that SyncManager::state_mutex_ is RELEASED before backend method
# invocations (Sync, Initialize), so a backend (or hook) may call back into
# SyncManager::GetSyncConfig from inside those methods without deadlocking on
# std::mutex re-entry. Uses MockSyncBackend (force-link anchor) for the
# vxcore_test_internals linkage and a local ReentrantBackend in-test for the
# actual re-entry exercise. Routed through EnableSyncWithFactoryForTesting so
# no libgit2 / network is touched.
add_vxcore_test(test_sync_manager_reentrancy INTERNALS)

# test_sync_thread_safety: Task 10.2 (F2.4 part 3) of sync-backend-phase4.
# Stress test: 8 threads x 5 notebooks x 200 iterations hammering
# EnableSync / TriggerSync / DisableSync / ClearDirty / GetSyncConfig /
# GetDirtyNotebooks / IsReady. Watchdog thread (30s budget) aborts on
# deadlock so failures surface with a useful core dump rather than a silent
# ctest timeout. Uses MockSyncBackend (from vxcore_test_internals) via the
# registry's "mock" auto-registration — no libgit2, no network, no Qt.
add_vxcore_test(test_sync_thread_safety INTERNALS)

# test_sync_backend_with_provider: Task 6.2 (F4.4) of sync-backend-phase4.
# Tests SyncManager::EnableSync(notebook_id, config, provider) overload
# (provider forwarding via the registry path) and AuthRequired capability
# enforcement (missing-provider → VXCORE_ERR_MISSING_CREDENTIALS + rollback).
# Uses MockSyncBackend (from vxcore_test_internals) so links via INTERNALS.
add_vxcore_test(test_sync_backend_with_provider INTERNALS)

# test_buffer_save_marks_dirty: T10 of auto-sync-on-save plan. Verifies that
# vxcore_buffer_save fires file.saved events that propagate through
# SyncManager's event subscription to mark notebooks dirty + enqueue sync
# jobs. Uses MockSyncBackend (extra source) for backend injection without
# touching libgit2 / network, so we cannot use the add_vxcore_test() helper
# (it only takes a single .cpp).
add_executable(test_buffer_save_marks_dirty
    test_buffer_save_marks_dirty.cpp
    mock_sync_backend.cpp)
target_include_directories(test_buffer_save_marks_dirty PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_buffer_save_marks_dirty PRIVATE vxcore nlohmann_json)
add_test(NAME test_buffer_save_marks_dirty COMMAND test_buffer_save_marks_dirty)


# spike_libgit2: Phase-0 kill-switch test for the libgit2 vendoring (T1 of the
# git-sync-backend plan). Exercises a separate gitdir/workdir + a full
# index->tree->commit round-trip to prove the static link works on this
# platform. Linked directly against libgit2package (NOT vxcore) because the
# real GitSyncBackend doesn't exist yet.
add_executable(spike_libgit2 spike_libgit2.cpp)
target_link_libraries(spike_libgit2 PRIVATE libgit2package)
target_include_directories(spike_libgit2 PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME spike_libgit2 COMMAND spike_libgit2)

# test_libgit2_init: T4 of the git-sync-backend plan. Verifies the LibGit2Init
# RAII ref-counted singleton balances git_libgit2_init/shutdown calls.
# Compiles libgit2_init.cpp directly (NOT linked through vxcore.dll) so the
# test binary owns its own copy of the ref-count statics — Windows DLL
# boundaries duplicate static storage, which would otherwise pollute the
# ref-count semantics with whatever vxcore did at load time.
add_executable(test_libgit2_init test_libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_libgit2_init PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_libgit2_init PRIVATE libgit2package)
add_test(NAME test_libgit2_init COMMAND test_libgit2_init)

# test_libgit2_init_propagation: Task 2.4 of sync-backend-phase4. Verifies that
# LibGit2Init::ok() tracks initialization success and SyncManager::EnableSync
# checks it. Direct-compile test (like test_libgit2_init) to own its own
# LibGit2Init statics.
add_executable(test_libgit2_init_propagation test_libgit2_init_propagation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_libgit2_init_propagation PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_libgit2_init_propagation PRIVATE vxcore libgit2package)
add_test(NAME test_libgit2_init_propagation COMMAND test_libgit2_init_propagation)

# test_sync_manager_unknown_backend: Task 2.3 of sync-backend-phase4. Verifies that
# SyncManager::EnableSync fails with VXCORE_ERR_UNKNOWN_BACKEND for unknown backends
# and succeeds for empty/git backends. Direct-compile test to access LibGit2Init.
add_executable(test_sync_manager_unknown_backend test_sync_manager_unknown_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_sync_manager_unknown_backend PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_sync_manager_unknown_backend PRIVATE vxcore libgit2package)
add_test(NAME test_sync_manager_unknown_backend COMMAND test_sync_manager_unknown_backend)

add_executable(test_content_processor test_content_processor.cpp
    ${CMAKE_SOURCE_DIR}/src/core/content_processor/markdown_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/core/content_processor/content_processor.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_link_libraries(test_content_processor PRIVATE vxcore cmark)
target_include_directories(test_content_processor PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_definitions(test_content_processor PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME test_content_processor COMMAND test_content_processor)

add_executable(test_asset_utils test_asset_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/core/content_processor/asset_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_link_libraries(test_asset_utils PRIVATE vxcore nlohmann_json)
target_include_directories(test_asset_utils PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_definitions(test_asset_utils PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME test_asset_utils COMMAND test_asset_utils)

# test_snippet: Tests SnippetManager C++ API directly (no C API yet).
# Compiles snippet_manager and dependencies directly since C++ symbols aren't DLL-exported.
add_executable(test_snippet test_snippet.cpp
    ${CMAKE_SOURCE_DIR}/src/core/snippet_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_link_libraries(test_snippet PRIVATE vxcore nlohmann_json)
target_include_directories(test_snippet PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_definitions(test_snippet PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME test_snippet COMMAND test_snippet)

add_executable(test_file_utils test_file_utils.cpp ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_file_utils PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME test_file_utils COMMAND test_file_utils)

add_executable(test_process_utils test_process_utils.cpp ${CMAKE_SOURCE_DIR}/src/platform/process_utils.cpp)
target_include_directories(test_process_utils PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
add_test(NAME test_process_utils COMMAND test_process_utils)

add_executable(test_rg_search_backend test_rg_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/rg_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/search_file_info.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/process_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_rg_search_backend PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_rg_search_backend PRIVATE nlohmann_json)
add_test(NAME test_rg_search_backend COMMAND test_rg_search_backend)

add_executable(test_simple_search_backend test_simple_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/simple_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/search_file_info.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_simple_search_backend PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_simple_search_backend PRIVATE nlohmann_json)
add_test(NAME test_simple_search_backend COMMAND test_simple_search_backend)

add_executable(test_db test_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/db_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/db/file_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/tag_db.cpp
    ${CMAKE_SOURCE_DIR}/src/core/folder.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp)
target_include_directories(test_db PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_db PRIVATE sqlite3 nlohmann_json)
add_test(NAME test_db COMMAND test_db)

add_executable(test_metadata_store test_metadata_store.cpp
    ${CMAKE_SOURCE_DIR}/src/db/db_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/db/file_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/tag_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/notebook_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/sqlite_metadata_store.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_metadata_store PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_metadata_store PRIVATE sqlite3 nlohmann_json)
add_test(NAME test_metadata_store COMMAND test_metadata_store)

# test_folder_manager: Tests FolderManager attachment interfaces directly
# Links against folder manager sources instead of vxcore library
add_executable(test_folder_manager test_folder_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bundled_folder_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/raw_folder_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/folder_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/event_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/work_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/folder.cpp
    ${CMAKE_SOURCE_DIR}/src/core/notebook.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bundled_notebook.cpp
    ${CMAKE_SOURCE_DIR}/src/core/raw_notebook.cpp
    ${CMAKE_SOURCE_DIR}/src/core/notebook_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/config_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vxcore_config.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vxcore_session_config.cpp
    ${CMAKE_SOURCE_DIR}/src/core/filetype_config.cpp
    ${CMAKE_SOURCE_DIR}/src/core/buffer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/standard_buffer_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/core/external_buffer_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/core/workspace.cpp
    ${CMAKE_SOURCE_DIR}/src/db/db_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/db/file_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/tag_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/notebook_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/sqlite_metadata_store.cpp
    ${CMAKE_SOURCE_DIR}/src/search/search_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/search/simple_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/rg_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/search_file_info.cpp
    ${CMAKE_SOURCE_DIR}/src/search/search_query.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/path_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/process_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/core/content_processor/asset_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/core/content_processor/content_processor.cpp
    ${CMAKE_SOURCE_DIR}/src/core/content_processor/markdown_handler.cpp)
target_include_directories(test_folder_manager PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_folder_manager PRIVATE sqlite3 nlohmann_json cmark)
add_test(NAME test_folder_manager COMMAND test_folder_manager)

# test_sync_manager_dispatch: T9 of git-sync-backend plan. Hosts MockSyncBackend
# tests and (in later tasks) SyncManager dispatch tests via the test-only
# RegisterBackendForTesting / EnableSyncWithBackendForTesting hooks.
add_executable(test_sync_manager_dispatch test_sync_manager_dispatch.cpp
    mock_sync_backend.cpp)
target_include_directories(test_sync_manager_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_sync_manager_dispatch PRIVATE vxcore nlohmann_json)
add_test(NAME test_sync_manager_dispatch COMMAND test_sync_manager_dispatch)

# git_sync_test_helpers: T10 of git-sync-backend plan. STATIC library providing
# libgit2-based fixture helpers (create_bare_repo / commit_file / git_head_sha /
# git_status_clean) for all test_git_sync_* binaries plus
# test_sync_manager_dispatch. Compiles libgit2_init.cpp directly (matching the
# test_libgit2_init pattern) so the helper owns its own copy of the libgit2
# refcount statics independent of vxcore.dll.
add_library(git_sync_test_helpers STATIC
    test_git_sync_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(git_sync_test_helpers PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(git_sync_test_helpers PUBLIC libgit2package)

# Link git_sync_test_helpers into test_sync_manager_dispatch so its T10
# RUN_TEST(test_helper_*) cases can use the helpers.
target_link_libraries(test_sync_manager_dispatch PRIVATE git_sync_test_helpers)

# test_git_sync_credentials: T27 (credential callback selection) and T28
# (libgit2 -> VxCoreError translation) of the git-sync-backend plan.
# Compiles git_sync_backend.cpp + libgit2_init.cpp directly (mirroring
# test_libgit2_init) instead of linking vxcore.dll, so this binary owns
# its own copies of the GitSyncBackend symbols and the libgit2 ref-count
# statics. Avoids needing dllexport for the file-scope credential callback
# and the test-only TranslateGitErrorForTesting hook.
# test_git_sync_init: T14-T19 of git-sync-backend plan. Initialize matrix
# (re-open / clone / init+push / reject / config / shutdown). T14 owns the
# re-open + URL-mismatch cases. Compiles git_sync_backend.cpp directly and
# pulls in libgit2_init.cpp / logger.cpp / file_utils.cpp via the
# git_sync_test_helpers static library — this binary owns its own copies of
# the GitSyncBackend symbols and libgit2 ref-count statics, avoiding the
# Windows DLL static-storage duplication problem.
add_executable(test_git_sync_init test_git_sync_init.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_git_sync_init PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_init PRIVATE
    git_sync_test_helpers
    vxcore_test_internals
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_init COMMAND test_git_sync_init)

add_executable(test_git_sync_credentials test_git_sync_credentials.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_git_sync_credentials PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_credentials PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_credentials COMMAND test_git_sync_credentials)

add_executable(test_git_sync_status test_git_sync_status.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_git_sync_status PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_status PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_status COMMAND test_git_sync_status)

# test_git_sync_roundtrip: T21 (StageAll), T22 (CommitIndex), T23
# (FetchOrigin) of the git-sync-backend plan. Drives the Sync sub-helpers
# through the public *ForTesting hooks. Compiled the same way as
# test_git_sync_status.
add_executable(test_git_sync_roundtrip test_git_sync_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_git_sync_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_roundtrip PRIVATE
    git_sync_test_helpers
    vxcore_test_internals
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_roundtrip COMMAND test_git_sync_roundtrip)

# test_git_sync_conflicts: T29 (GetConflicts) + T30/T31/T32 (ResolveConflict
# variants) of git-sync-backend plan. Same compile pattern as
# test_git_sync_roundtrip — owns its own GitSyncBackend statics.
add_executable(test_git_sync_conflicts test_git_sync_conflicts.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_git_sync_conflicts PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_conflicts PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_conflicts COMMAND test_git_sync_conflicts)

# test_resolve_result: B6 of sync-backend-phase4 plan. Exercises the new
# tri-state ResolveResult enum returned by GitConflictResolver::ResolveConflictEx.
# Direct-compiles the resolver + its transitive deps (same pattern as
# test_git_sync_conflicts) because the new enum entry point is not exported
# through vxcore.dll.
add_executable(test_resolve_result test_resolve_result.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_resolve_result PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_resolve_result PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_resolve_result COMMAND test_resolve_result)

# test_git_sync_pushpull + test_sync_pull_auto_commit: REMOVED in Wave 4.5
# (F1.3 of sync-backend-phase4). These two test binaries exclusively exercised
# ISyncBackend::Push and ISyncBackend::Pull, which were deleted from the
# interface (rip-and-replace, no shims). Sync() — the composite roundtrip —
# remains and is covered end-to-end by test_git_sync_roundtrip.

# test_gitkeep_basic: gitkeep injection plan — verifies .gitkeep is added
# to empty user folders during sync, respects the skip-list, and is
# idempotent. Same compile pattern as test_git_sync_roundtrip.
add_executable(test_gitkeep_basic test_gitkeep_basic.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_gitkeep_basic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_gitkeep_basic PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_gitkeep_basic COMMAND test_gitkeep_basic)

# test_gitkeep_cleanup: gitkeep injection plan — auto-cleanup when folder
# becomes non-empty, marker-ownership preservation (non-empty .gitkeep
# never deleted), effective-emptiness for excluded-only folders.
add_executable(test_gitkeep_cleanup test_gitkeep_cleanup.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_gitkeep_cleanup PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_gitkeep_cleanup PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_gitkeep_cleanup COMMAND test_gitkeep_cleanup)

# test_gitkeep_roundtrip: gitkeep injection plan — full push + clone
# roundtrip via libgit2; verifies empty folders survive on the remote.
add_executable(test_gitkeep_roundtrip test_gitkeep_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_gitkeep_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_gitkeep_roundtrip PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_gitkeep_roundtrip COMMAND test_gitkeep_roundtrip)

# test_git_sync_clone: T13 of open-notebook-remote-readonly plan. Exercises
# GitSyncBackend::Clone, the wrapper around BootstrapFromEmptyRemote that
# materializes a notebook from a remote sync URL into an empty target dir.
# Same direct-compile pattern as test_gitkeep_roundtrip (this binary owns
# its own GitSyncBackend + libgit2 ref-count statics, sidestepping the
# Windows DLL static-storage duplication problem).
add_executable(test_git_sync_clone test_git_sync_clone.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_git_sync_clone PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_clone PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_clone COMMAND test_git_sync_clone)

# test_sync_clone_orchestrator: T18 of open-notebook-remote-readonly plan.
# Exercises the backend-agnostic SyncManager::CloneNotebook orchestrator
# (defined in src/sync/sync_manager.cpp). Links via add_vxcore_test() so
# vxcore.dll provides SyncManager + GitSyncBackend registration; adds
# git_sync_test_helpers for the bare-repo fixture and nlohmann_json for
# seeding vx_notebook/config.json content.
add_vxcore_test(test_sync_clone_orchestrator)
target_link_libraries(test_sync_clone_orchestrator PRIVATE git_sync_test_helpers nlohmann_json)

# test_sync_credentials_api: T1+T2 of notebook-git-sync-ui plan. Exercises the
# C ABI wrappers vxcore_sync_set_credentials and vxcore_sync_enable_with_credentials
# end-to-end against a real libgit2 file:// bare-repo fixture. Reuses the
# git_sync_test_helpers static library (defined above) for create_bare_repo +
# commit_file. Linked against vxcore (via add_vxcore_test) for the C ABI under
# test, plus git_sync_test_helpers + nlohmann_json for the fixture and JSON.
add_vxcore_test(test_sync_credentials_api)
target_link_libraries(test_sync_credentials_api PRIVATE git_sync_test_helpers nlohmann_json)

# test_vxcore_sync_cancellable_api: Wave 12.2 / F5.9 of sync-backend-phase4.
# C ABI tests for vxcore_sync_create_cancellation /
# vxcore_sync_cancel / vxcore_sync_free_cancellation /
# vxcore_sync_trigger_cancellable lifecycle and null-safety. No libgit2
# fixture needed — exercises the C ABI plumbing on a non-enabled notebook
# (the SYNC_NOT_ENABLED path proves the dispatch reaches SyncManager
# without going through any backend).
add_vxcore_test(test_vxcore_sync_cancellable_api)

# test_sync_backend_metadata: Task 4.1 (F1.2) of sync-backend-phase4. Verifies
# the new ISyncBackend identity methods (GetName / GetCapabilities /
# IsInitialized) on GitSyncBackend (direct-compiled) and the configurable
# MockSyncBackend from vxcore_test_internals. Same direct-compile pattern as
# test_git_sync_init — owns its own GitSyncBackend + libgit2 ref-count
# statics, bypassing the Windows DLL static-storage duplication problem.
add_executable(test_sync_backend_metadata test_sync_backend_metadata.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_backend_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/retry_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_config_fixer.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_conflict_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_credential_callback.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/sync_cancellation.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_defaults.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_error_translator.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_options.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_repo_bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/git_sync_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git/gitkeep_sweeper.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_sync_backend_metadata PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_sync_backend_metadata PRIVATE
    git_sync_test_helpers
    vxcore_test_internals
    libgit2package
    nlohmann_json)
add_test(NAME test_sync_backend_metadata COMMAND test_sync_backend_metadata)

# test_vxcore_sync_clone_capi: T19 of open-notebook-remote-readonly plan.
# C-ABI tests for vxcore_sync_clone -- the C wrapper around
# SyncManager::CloneNotebook (T18). Same shape as test_sync_clone_orchestrator
# but exercises the public C entry point: null-arg hygiene, missing/unknown
# backend, malformed JSON, empty + NULL credentials, and the file:// happy
# path that round-trips through vxcore_notebook_list. Links via
# add_vxcore_test() so vxcore.dll provides the SyncManager + backend
# registration; adds git_sync_test_helpers for create_bare_repo/commit_file
# and nlohmann_json for seeding vx_notebook/config.json content.
add_vxcore_test(test_vxcore_sync_clone_capi)
target_link_libraries(test_vxcore_sync_clone_capi PRIVATE git_sync_test_helpers nlohmann_json)

# test_vxcore_sync_clone_cancellable: openurl-followups Item 2.
# Exercises the new vxcore_sync_clone_cancellable C ABI surface — the
# cancellation overload that mirrors vxcore_sync_clone but accepts an
# optional VxCoreSyncCancellation* token. Three subtests:
#   1. BEFORE-clone cancel (strict): token cancelled before invocation.
#   2. MID-clone cancel (race-tolerant): worker thread fires Cancel mid-
#      clone against a file:// fixture; tolerant of races since file://
#      is fast enough to legitimately complete before Cancel lands.
#   3. null-token equivalence: proves the shim relationship between
#      vxcore_sync_clone (legacy) and vxcore_sync_clone_cancellable.
# Same linkage pattern as test_vxcore_sync_clone_capi: vxcore.dll +
# git_sync_test_helpers (bare-repo fixture) + nlohmann_json.
add_vxcore_test(test_vxcore_sync_clone_cancellable)
target_link_libraries(test_vxcore_sync_clone_cancellable PRIVATE git_sync_test_helpers nlohmann_json)
