# Agent Guidelines for vxcore

## Build Commands
- **Configure**: `cmake -B build -DVXCORE_BUILD_TESTS=ON`
- **Build**: `cmake --build build`
- **Run all tests**: `ctest --test-dir build -C Debug` (Windows) or `ctest --test-dir build` (Unix)
- **Run single module**: `ctest --test-dir build -C Debug -R test_core` or `test_notebook`
- **Run with verbose**: `ctest --test-dir build -C Debug -V`

## Test Structure
- Each module (core, notebook) has its own test executable
- Individual test cases are tracked within each module's main()
- Add new test cases to existing test_*.cpp files or create new modules as needed

## Code Style
- **Language**: C++17 with C ABI for public API
- **Formatting**: Use `.clang-format` based on Google style (indent 2 spaces, 100 col limit, pointer alignment right)
- **Headers**: Sort includes; public API in `include/vxcore/`, implementation in `src/`
- **Naming** (following Google C++ Style Guide):
  - **C API**: `snake_case` (e.g., `vxcore_context_create`)
  - **Types** (classes/structs/enums): `PascalCase` (e.g., `NotebookManager`, `VxCoreError`)
  - **Functions/Methods**: `PascalCase` (e.g., `GetNotebook()`, `CreateContext()`)
  - **Variables** (local/global): `snake_case` (e.g., `notebook_id`, `root_folder`)
  - **Class data members**: `snake_case_` with trailing underscore (e.g., `config_`, `notebooks_`)
  - **Struct data members**: `snake_case` without trailing underscore (e.g., `assets_folder`, `last_opened_timestamp`)
  - **Constants**: `kPascalCase` with leading k (e.g., `kMaxPathLength`, `kDefaultTimeout`)
  - **Enumerators**: `kPascalCase` with leading k (e.g., `kSuccess`, `kErrorInvalidArgument`)
  - **Macros**: `UPPER_CASE` (e.g., `VXCORE_API`, `RUN_TEST`)
  - **Namespaces**: `snake_case` (e.g., `vxcore`)
- **Types**: Opaque handles (`VxCoreContextHandle`), enums for errors (`VxCoreError`)
- **Error handling**: Return `VxCoreError` codes; check null pointers; use `(void)param` for unused
- **Memory**: Caller frees strings with `vxcore_string_free()`; context owns handles
- **Platform**: Use `VXCORE_API` macro for exports; Windows DLL-safe; no Qt dependencies
- **JSON**: Communication boundary uses JSON over C ABI for cross-platform stability
  - **JSON keys**: Always use `camelCase` (e.g., `createdUtc`, `modifiedUtc`, `assetsFolder`)
  - **C++ struct members**: Use `snake_case` (e.g., `created_utc`, `modified_utc`, `assets_folder`)
  - **Rationale**: User-facing files (config.json, vx.json) follow JavaScript conventions
- **Namespaces**: C++ code in `namespace vxcore`; close with `// namespace vxcore`
- **File Format**: Use Unix line ending

## Folder Structure
- **`include/vxcore/`**: Public C API headers (`vxcore.h`, `vxcore_types.h`, `vxcore_events.h`)
- **`src/api/`**: C API implementation (`vxcore_api.cpp`, `vxcore_*_api.cpp`, `error_handler.h`, `handle_manager.h`)
- **`src/core/`**: Core C++ logic (`context.h`, `config_manager.h`, `notebook_manager.h`, `notebook.h`)
  - **Notebook Implementation**: `notebook.h` contains `NotebookConfig`, `NotebookRecord`, and `Notebook` class
    - `NotebookConfig`: Persistent configuration (id, name, description, folders, metadata)
    - `NotebookRecord`: Session metadata (id, root_folder, type, last_opened_timestamp, raw_config)
    - `Notebook`: Represents a single notebook instance (bundled or raw type)
  - **NotebookManager**: Thread-safe manager for notebook lifecycle (create, open, close, list)
    - Uses `std::mutex` for thread-safety across all public methods
    - Manages in-memory notebook instances (`std::unordered_map<std::string, std::unique_ptr<Notebook>>`)
    - Syncs notebook records with session config via `session_config_updater_` callback
    - Bundled notebooks store config in `<root>/vx_notebook/config.json`
    - Raw notebooks store config in session config only
- **`src/platform/`**: Platform-specific utilities (`path_provider.h`)
- **`src/utils/`**: General utilities (`utils.h`)
- **`cli/`**: CLI tool implementation (`main.cpp`, `notebook_cmd.h`, `json_helpers.h`)
- **`tests/`**: Test executables (`test_core.cpp`, `test_notebook.cpp`)
- **`third_party/`**: External dependencies (`nlohmann/json.hpp`)

## Architecture Notes
- C ABI for cross-platform embedding (desktop/iOS/Android)
- File-based notes with SQLite metadata (WAL + FTS5)
- Event-driven with typed JSON notifications
- Core plugins: Lua (logic); UI plugins: Python (Qt customization)
