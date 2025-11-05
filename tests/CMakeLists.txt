function(add_vxcore_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE vxcore)
    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_vxcore_test(test_core)
add_vxcore_test(test_notebook)

