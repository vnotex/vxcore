add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# BS::thread_pool library
add_library(bs_thread_pool INTERFACE)
target_include_directories(bs_thread_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# SQLite3 library
add_subdirectory(sqlite)
