# Database layer
add_library(vxcore_db STATIC
  db_manager.cpp
  db_manager.h
  db_schema.h
  file_db.cpp
  file_db.h
  tag_db.cpp
  tag_db.h
  sync_manager.cpp
  sync_manager.h
  sqlite_metadata_store.cpp
  sqlite_metadata_store.h
)

target_link_libraries(vxcore_db
  PRIVATE sqlite3
)

target_include_directories(vxcore_db
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
  PUBLIC ${CMAKE_SOURCE_DIR}/third_party
  PUBLIC ${CMAKE_SOURCE_DIR}/include
)
