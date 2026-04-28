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
