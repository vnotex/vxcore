# Agent Guidelines for vxcore

## Build Commands
- **Configure**: `cmake -B build -DVXCORE_BUILD_TESTS=ON`
- **Build**: `cmake --build build`
- **Run all tests**: `ctest --test-dir build -C Debug` (Windows) or `ctest --test-dir build` (Unix)
- **Run single test**: `ctest --test-dir build -C Debug -R test_name`
- **Run test category**: `ctest --test-dir build -C Debug -R notebook` (runs all notebook tests)

## Code Style
- **Language**: C++17 with C ABI for public API
- **Formatting**: Use `.clang-format` (indent 2 spaces, 100 col limit, pointer alignment right)
- **Headers**: Sort includes; public API in `include/vxcore/`, implementation in `src/`
- **Naming**: `snake_case` for C API, `PascalCase` for C++ classes, `camelCase` for methods
- **Types**: Opaque handles (`VxCoreContextHandle`), enums for errors (`VxCoreError`)
- **Error handling**: Return `VxCoreError` codes; check null pointers; use `(void)param` for unused
- **Memory**: Caller frees strings with `vxcore_string_free()`; context owns handles
- **Platform**: Use `VXCORE_API` macro for exports; Windows DLL-safe; no Qt dependencies
- **JSON**: Communication boundary uses JSON over C ABI for cross-platform stability
- **Namespaces**: C++ code in `namespace vxcore`; close with `// namespace vxcore`
- **File Format**: Use Unix line ending

## Architecture Notes
- C ABI for cross-platform embedding (desktop/iOS/Android)
- File-based notes with SQLite metadata (WAL + FTS5)
- Event-driven with typed JSON notifications
- Core plugins: Lua (logic); UI plugins: Python (Qt customization)
