# cmark (VNote fork) - CommonMark parsing library
# Build as static library

add_library(cmark STATIC
  blocks.c
  buffer.c
  cmark.c
  cmark_ctype.c
  commonmark.c
  footnotes.c
  houdini_href_e.c
  houdini_html_e.c
  houdini_html_u.c
  html.c
  inlines.c
  iterator.c
  latex.c
  man.c
  node.c
  references.c
  render.c
  scanners.c
  utf8.c
  xml.c
)

# Static build - no dllexport needed
target_compile_definitions(cmark PUBLIC CMARK_STATIC_DEFINE)

# Platform-specific settings
if(WIN32)
  # Disable warnings for vendored cmark on Windows
  target_compile_options(cmark PRIVATE /W0)
else()
  # Disable warnings for vendored cmark on Unix
  target_compile_options(cmark PRIVATE -w)
endif()

# Export include directory
target_include_directories(cmark PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
