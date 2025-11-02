cmake_minimum_required(VERSION 3.18)

project(vxcore
    VERSION 0.1.0
    DESCRIPTION "VNote Core Library - Cross-platform note management engine"
    LANGUAGES CXX C
)

option(VXCORE_BUILD_SHARED "Build shared library" ON)
option(VXCORE_BUILD_CLI "Build command-line interface" ON)
option(VXCORE_BUILD_TESTS "Build tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

add_subdirectory(third_party)
add_subdirectory(src)

if(VXCORE_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(VXCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY data/
    DESTINATION ${CMAKE_INSTALL_BINDIR}/data
    FILES_MATCHING PATTERN "*.json"
)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/vxcore-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/vxcore-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vxcore
)
