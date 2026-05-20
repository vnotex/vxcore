# vxcore_test_internals — STATIC library of shared test helpers.
#
# Task 0.1 of .sisyphus/plans/sync-backend-phase4.md.
#
# Tests opt in to this library by passing INTERNALS to add_vxcore_test():
#   add_vxcore_test(test_foo INTERNALS)
#
# It deliberately stays minimal (sync_test_helpers only). Later Wave-0 tasks
# add the mock backend + credential provider here.
#
# Constraints (see libs/vxcore/AGENTS.md "Standalone test pattern"):
#   - Pure C++17 + libgit2 + std. NO Qt deps.
#   - Owns its own utility translation units so the library is self-contained
#     when linked into tests that direct-compile internal vxcore sources.

add_library(vxcore_test_internals STATIC
    src/sync_test_helpers.cpp
    src/mock_sync_backend.cpp
    src/mock_credential_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp)

target_include_directories(vxcore_test_internals PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(vxcore_test_internals PUBLIC libgit2package)

if(MSVC)
    target_compile_definitions(vxcore_test_internals PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
