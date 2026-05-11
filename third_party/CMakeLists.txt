add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# BS::thread_pool library
add_library(bs_thread_pool INTERFACE)
target_include_directories(bs_thread_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# SQLite3 library
add_subdirectory(sqlite)

# cmark library (VNote fork) — git submodule
# Guard: when built inside VNote, vtextedit may already provide this target
if(NOT TARGET cmark)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory(cmark EXCLUDE_FROM_ALL)
  # Suppress warnings for third-party code
  if(MSVC)
    target_compile_options(cmark PRIVATE /W0)
  else()
    target_compile_options(cmark PRIVATE -w)
  endif()
endif()

# libgit2 — git submodule, statically linked, HTTPS only (no SSH).
# Cache overrides MUST be set BEFORE add_subdirectory so libgit2's CMake sees them.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(USE_SSH OFF CACHE BOOL "" FORCE)
set(USE_BUNDLED_ZLIB ON CACHE BOOL "" FORCE)
set(REGEX_BACKEND "builtin" CACHE STRING "" FORCE)
if(WIN32)
  set(USE_HTTPS "WinHTTP" CACHE STRING "" FORCE)
elseif(APPLE)
  set(USE_HTTPS "SecureTransport" CACHE STRING "" FORCE)
else()
  set(USE_HTTPS "OpenSSL" CACHE STRING "" FORCE)
endif()
add_subdirectory(libgit2 EXCLUDE_FROM_ALL)
# libgit2 v1.7 names its public static-library target "libgit2package"; expose
# its public include directory as an INTERFACE usage requirement so downstream
# consumers (vxcore + the spike test) only need to link against the target.
target_include_directories(libgit2package INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/libgit2/include
)
# Suppress warnings for third-party code so vxcore's strict warning levels
# (/W4 on MSVC, -Wall -Wextra -Wpedantic on GCC/Clang) don't drown the build.
if(MSVC)
  target_compile_options(libgit2package PRIVATE /W0)
else()
  target_compile_options(libgit2package PRIVATE -w)
endif()
