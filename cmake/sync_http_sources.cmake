# Shared by the vxcore library and direct-compile HTTP sync consumers.
set(VXCORE_SYNC_HTTP_SOURCES_RELATIVE
    sync/http/sync_http_client.cpp
)

set(VXCORE_SYNC_HTTP_SOURCES_ABS "")
foreach(_rel ${VXCORE_SYNC_HTTP_SOURCES_RELATIVE})
    list(APPEND VXCORE_SYNC_HTTP_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/../src/${_rel}")
endforeach()
unset(_rel)
