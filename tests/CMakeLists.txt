function(add_vxcore_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE vxcore)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_vxcore_test(test_core)
add_vxcore_test(test_notebook)
add_vxcore_test(test_folder)
add_vxcore_test(test_node)
add_vxcore_test(test_search)
add_vxcore_test(test_tag_sync)
add_vxcore_test(test_filetype)
add_vxcore_test(test_workspace)
add_vxcore_test(test_buffer)
add_vxcore_test(test_history)
add_vxcore_test(test_session_persistence)
add_vxcore_test(test_template)
add_vxcore_test(test_e2e_assets)
add_vxcore_test(test_sync)

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
    ${CMAKE_SOURCE_DIR}/src/sync/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_libgit2_init PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_libgit2_init PRIVATE libgit2package)
add_test(NAME test_libgit2_init COMMAND test_libgit2_init)

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
    ${CMAKE_SOURCE_DIR}/src/sync/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
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
    ${CMAKE_SOURCE_DIR}/src/sync/git_sync_backend.cpp)
target_include_directories(test_git_sync_init PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_init PRIVATE
    git_sync_test_helpers
    libgit2package
    nlohmann_json)
add_test(NAME test_git_sync_init COMMAND test_git_sync_init)

add_executable(test_git_sync_credentials test_git_sync_credentials.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/git_sync_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/sync/libgit2_init.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_git_sync_credentials PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_git_sync_credentials PRIVATE libgit2package nlohmann_json)
add_test(NAME test_git_sync_credentials COMMAND test_git_sync_credentials)
