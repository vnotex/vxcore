add_executable(vxcore-tests
    test_main.cpp
    test_core.cpp
    test_notebook.cpp
)

target_link_libraries(vxcore-tests
    PRIVATE
        vxcore
)

target_include_directories(vxcore-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME vxcore-tests COMMAND vxcore-tests)

