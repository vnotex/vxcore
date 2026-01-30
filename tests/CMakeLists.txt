function(add_vxcore_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE vxcore)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_vxcore_test(test_core)
add_vxcore_test(test_notebook)
add_vxcore_test(test_folder)
add_vxcore_test(test_search)

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
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_rg_search_backend PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_rg_search_backend PRIVATE nlohmann_json)
add_test(NAME test_rg_search_backend COMMAND test_rg_search_backend)

add_executable(test_simple_search_backend test_simple_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/simple_search_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/search/search_file_info.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp)
target_include_directories(test_simple_search_backend PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_simple_search_backend PRIVATE nlohmann_json)
add_test(NAME test_simple_search_backend COMMAND test_simple_search_backend)

add_executable(test_db test_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/db_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/db/file_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/tag_db.cpp
    ${CMAKE_SOURCE_DIR}/src/db/sync_manager.cpp
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
    ${CMAKE_SOURCE_DIR}/src/db/sqlite_metadata_store.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/file_utils.cpp)
target_include_directories(test_metadata_store PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(test_metadata_store PRIVATE sqlite3 nlohmann_json)
add_test(NAME test_metadata_store COMMAND test_metadata_store)

