add_executable(vxsearch-validate
    main.cpp
)

target_include_directories(vxsearch-validate
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(vxsearch-validate
    PRIVATE
        vxcore
)

if(WIN32)
    # CommandLineToArgvW / LocalFree for UTF-8-safe wide argv acquisition.
    target_link_libraries(vxsearch-validate PRIVATE shell32)
endif()

set_target_properties(vxsearch-validate PROPERTIES
    OUTPUT_NAME vxsearch-validate
)

if(MSVC)
    target_compile_options(vxsearch-validate PRIVATE /W4)
else()
    target_compile_options(vxsearch-validate PRIVATE -Wall -Wextra -Wpedantic)
endif()
