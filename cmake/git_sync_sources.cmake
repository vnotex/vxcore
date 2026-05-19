# Sources that constitute the git sync backend.
#
# Included from libs/vxcore/src/CMakeLists.txt (for the vxcore library) AND
# libs/vxcore/tests/CMakeLists.txt (for direct-compile tests that need access
# to non-VXCORE_API symbols). Centralizes the list to avoid the transitive-
# utility footgun documented in libs/vxcore/AGENTS.md when adding new helper
# .cpp files.
#
# Two variants are provided:
#   VXCORE_GIT_SYNC_SOURCES_RELATIVE  Paths relative to libs/vxcore/src/,
#                                     for use inside libs/vxcore/src/CMakeLists.txt
#                                     where VXCORE_SOURCES is composed.
#   VXCORE_GIT_SYNC_SOURCES_ABS       Absolute paths, for use in direct-compile
#                                     test targets in libs/vxcore/tests/CMakeLists.txt
#                                     (which add_executable outside the src/ subdir
#                                     and so cannot use src-relative paths).
#
# CMAKE_CURRENT_LIST_DIR inside this file resolves to libs/vxcore/cmake/, so
# ../src/ correctly points to libs/vxcore/src/.

set(VXCORE_GIT_SYNC_SOURCES_RELATIVE
    sync/git/libgit2_init.cpp
    sync/git_sync_backend.cpp
)

set(VXCORE_GIT_SYNC_SOURCES_ABS "")
foreach(_rel ${VXCORE_GIT_SYNC_SOURCES_RELATIVE})
    list(APPEND VXCORE_GIT_SYNC_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/../src/${_rel}")
endforeach()
unset(_rel)
