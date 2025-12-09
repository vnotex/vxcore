# vxcore
vxcore is a cross-platform C/C++ library providing notebook management functionality for VNote. It offers a stable C ABI for embedding in desktop (Qt), iOS (Swift), and Android (Kotlin) applications.

## Features
- **Notebook Management**: Create, open, close notebooks (bundled & raw types)
- **Folder Operations**: Create, delete, rename, move, copy folders
- **File Operations**: Create, delete, rename, move, copy files with metadata
- **Tag System**: Create tags at notebook level, apply/remove tags on files
- **Configuration**: JSON-based configuration with session persistence
- **Events**: Typed event notifications for UI synchronization (future)
- **Search**: Integration with external search tools (future)

## Build

### Configure
```bash
cmake -B build -DVXCORE_BUILD_TESTS=ON
```

### Build
```bash
cmake --build build
```

### Run Tests
```bash
# All tests
ctest --test-dir build -C Debug  # Windows
ctest --test-dir build           # Unix

# Single module
ctest --test-dir build -C Debug -R test_core
```

## License
See LICENSE file
