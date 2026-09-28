# Shared by vxcore and the direct-compile managed Jianguoyun tests.
set(VXCORE_JIANGUOYUN_SOURCES_RELATIVE
    sync/jianguoyun/jianguoyun_transport.cpp
    sync/jianguoyun/jianguoyun_state.cpp
    sync/jianguoyun/jianguoyun_sync_backend.cpp
)

set(VXCORE_JIANGUOYUN_SOURCES_ABS "")
foreach(_rel ${VXCORE_JIANGUOYUN_SOURCES_RELATIVE})
    list(APPEND VXCORE_JIANGUOYUN_SOURCES_ABS "${CMAKE_CURRENT_LIST_DIR}/../src/${_rel}")
endforeach()
unset(_rel)
