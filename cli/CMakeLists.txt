add_executable(vxcore-cli
    main.cpp
    args.cpp
    json_helpers.cpp
    notebook_cmd.cpp
    tag_cmd.cpp
    config_cmd.cpp
)

target_include_directories(vxcore-cli
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(vxcore-cli
    PRIVATE
        vxcore
)

set_target_properties(vxcore-cli PROPERTIES
    OUTPUT_NAME vxcli
)

if(MSVC)
    target_compile_options(vxcore-cli PRIVATE /W4)
else()
    target_compile_options(vxcore-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS vxcore-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
