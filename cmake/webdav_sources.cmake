# Shared by the vxcore library and direct-compile WebDAV tests.
set(VXCORE_WEBDAV_SOURCES_RELATIVE
    sync/webdav/webdav_transport.cpp
    sync/webdav/webdav_state.cpp
    sync/webdav/webdav_sync_backend.cpp
)

set(VXCORE_WEBDAV_SOURCES_ABS "")
foreach(_rel ${VXCORE_WEBDAV_SOURCES_RELATIVE})
    list(APPEND VXCORE_WEBDAV_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/../src/${_rel}")
endforeach()
unset(_rel)
