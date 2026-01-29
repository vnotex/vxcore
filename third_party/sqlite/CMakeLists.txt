# SQLite3 Amalgamation
# Build as static library with FTS5 and JSON1 extensions enabled

add_library(sqlite3 STATIC
  sqlite3.c
  sqlite3.h
  sqlite3ext.h
)

# Enable FTS5 (Full-Text Search) and JSON1 extensions
target_compile_definitions(sqlite3 PUBLIC
  SQLITE_ENABLE_FTS5
  SQLITE_ENABLE_JSON1
)

# Platform-specific settings
if(WIN32)
  # Disable warnings for SQLite3 amalgamation on Windows
  target_compile_options(sqlite3 PRIVATE /W0)
  # Enable thread-safe mode
  target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1)
else()
  # Disable warnings for SQLite3 amalgamation on Unix
  target_compile_options(sqlite3 PRIVATE -w)
  # Enable thread-safe mode
  target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1)
  # Link pthread for Unix platforms
  target_link_libraries(sqlite3 PUBLIC pthread ${CMAKE_DL_LIBS})
endif()

# Export include directory
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
