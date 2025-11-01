add_executable(vxcore-cli
    main.cpp
)

target_link_libraries(vxcore-cli
    PRIVATE
        vxcore
)

set_target_properties(vxcore-cli PROPERTIES
    OUTPUT_NAME vxcore
)

if(MSVC)
    target_compile_options(vxcore-cli PRIVATE /W4)
else()
    target_compile_options(vxcore-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS vxcore-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
