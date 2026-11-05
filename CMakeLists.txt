cmake_minimum_required(VERSION 3.18)

project(vxcore
    VERSION 0.1.0
    DESCRIPTION "VNote Core Library - Cross-platform note management engine"
    LANGUAGES CXX C
)

option(VXCORE_BUILD_SHARED "Build shared library" ON)
option(VXCORE_BUILD_CLI "Build command-line interface" ON)
option(VXCORE_BUILD_TESTS "Build tests" ON)
option(VXCORE_BUILD_TOOLS "Build diagnostic tools" OFF)
option(VXCORE_INSTALL "Enable install targets" ON)

# Select by target platform, never by the machine running CMake.
set(VXCORE_MOBILE_PLATFORM OFF)
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(VXCORE_MOBILE_PLATFORM ON)
endif()
option(VXCORE_ENABLE_MOBILE_TEST_API "Expose mobile context API for desktop-hosted tests only" OFF)
if(VXCORE_ENABLE_MOBILE_TEST_API AND NOT VXCORE_MOBILE_PLATFORM AND VXCORE_INSTALL)
    message(FATAL_ERROR "Desktop mobile-API tests require VXCORE_INSTALL=OFF")
endif()
set(VXCORE_MOBILE_API_ENABLED ${VXCORE_MOBILE_PLATFORM})
if(VXCORE_ENABLE_MOBILE_TEST_API)
    set(VXCORE_MOBILE_API_ENABLED ON)
endif()

# Shared with direct-compile tests. Test-only API opt-in NEVER selects this policy.
if(VXCORE_MOBILE_PLATFORM)
    set(VXCORE_BUFFER_SAVE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mobile/buffer_file_save_mobile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mobile/file_writer_mobile.cpp)
else()
    set(VXCORE_BUFFER_SAVE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/buffer_file_save_desktop.cpp)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable Qt's AUTOMOC/AUTOUIC/AUTORCC for this pure C/C++ library
# (prevents warnings when built as subdirectory of Qt projects)
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# Android, iOS and other Unix targets use OpenSSL; desktop Apple retains SecureTransport.
if((UNIX AND NOT APPLE) OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_package(OpenSSL REQUIRED)
endif()

# MSVC: enable C++ exception unwinding (/EHsc). Without this, try/catch blocks
# emit C4530 warnings AND don't actually unwind at runtime — uncaught exceptions
# trigger std::terminate() → abort() → "VC++ Runtime Library" modal dialog,
# breaking tests like test_git_options::malformed_json_falls_back_to_defaults
# that legitimately expect nlohmann::json::parse to throw.
# Using add_compile_options (NOT target_compile_options) propagates to vxcore
# lib + CLI + every test target via subdirectory inheritance.
if(MSVC)
    add_compile_options(/EHsc)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

add_subdirectory(third_party)

# Discover Iconv after libgit2: leaking ICONV_FOUND into its configuration
# enables Unicode precomposition on Linux and can close an uninitialized handle.
# Keep the imported target in this scope for both src and standalone tests.
if(NOT WIN32)
    find_package(Iconv REQUIRED)
endif()

add_subdirectory(src)

if(VXCORE_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(VXCORE_BUILD_TOOLS)
    add_subdirectory(tools/search-validate)
endif()

if(VXCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VXCORE_INSTALL)
    include(GNUInstallDirs)

    get_target_property(_vxcore_sodium_source_dir sodium SOURCE_DIR)
    install(FILES "${_vxcore_sodium_source_dir}/libsodium/LICENSE"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/vxcore RENAME LICENSE.libsodium)
    install(FILES "${_vxcore_sodium_source_dir}/LICENSE"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/vxcore RENAME LICENSE.libsodium-cmake)

    get_target_property(_vxcore_pugixml_license pugixml VXCORE_LICENSE_FILE)
    install(FILES "${_vxcore_pugixml_license}"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/vxcore RENAME LICENSE.pugixml)
    if(NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        get_target_property(_vxcore_curl_license vxcore_webdav_curl VXCORE_LICENSE_FILE)
        install(FILES "${_vxcore_curl_license}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/vxcore RENAME LICENSE.curl)
    endif()

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "mobile" EXCLUDE
    )
    if(VXCORE_MOBILE_PLATFORM)
        install(DIRECTORY include/vxcore/mobile/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vxcore/mobile
            FILES_MATCHING PATTERN "*.h")
    endif()

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
endif()
