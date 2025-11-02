set(VXCORE_PUBLIC_HEADERS
    ${CMAKE_SOURCE_DIR}/include/vxcore/vxcore.h
    ${CMAKE_SOURCE_DIR}/include/vxcore/vxcore_types.h
    ${CMAKE_SOURCE_DIR}/include/vxcore/vxcore_events.h
)

set(VXCORE_SOURCES
    api/vxcore_api.cpp
    api/vxcore_events.cpp
    api/handle_manager.cpp
    api/error_handler.cpp
    core/context.cpp
    core/config_manager.cpp
    core/vxcore_config.cpp
    core/vxcore_session_config.cpp
    platform/path_provider.cpp
)

if(VXCORE_BUILD_SHARED)
    add_library(vxcore SHARED ${VXCORE_SOURCES} ${VXCORE_PUBLIC_HEADERS})
    target_compile_definitions(vxcore PRIVATE VXCORE_BUILD_DLL)
    target_compile_definitions(vxcore PUBLIC VXCORE_SHARED)
else()
    add_library(vxcore STATIC ${VXCORE_SOURCES} ${VXCORE_PUBLIC_HEADERS})
endif()

target_include_directories(vxcore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vxcore PRIVATE nlohmann_json)

if(WIN32)
    target_link_libraries(vxcore PRIVATE shell32)
    target_compile_definitions(vxcore PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(vxcore PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "${VXCORE_PUBLIC_HEADERS}"
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(MSVC)
    target_compile_options(vxcore PRIVATE /W4)
else()
    target_compile_options(vxcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS vxcore
    EXPORT vxcore-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vxcore
)

install(EXPORT vxcore-targets
    FILE vxcore-targets.cmake
    NAMESPACE vxcore::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vxcore
)
