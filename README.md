# broclip

Standalone C++20 clipboard history and data-control engine for the bro ecosystem and modern desktop environments.

`broclip` preserves clipboard content across application lifecycles, synchronizes primary selections, and manages multi-MIME stream caching with strict memory quotas.

---

## Features

- **Application Lifecycle Preservation**: Preserves clipboard content in memory when source applications terminate by caching offered MIME streams and taking over selection ownership via Wayland data-control protocols.
- **Multi-MIME Stream Caching**: Concurrently reads and manages multiple representations (`text/plain`, `text/html`, `image/png`, `application/octet-stream`) via non-blocking streaming pipes.
- **Primary Selection Synchronization**: Bi-directional synchronization between the standard clipboard and primary selection buffers (`SelectionKind::Clipboard` and `SelectionKind::Primary`).
- **Thread-Safe Ring Buffer Storage (`RingStore`)**:
  - In-memory LRU ring buffer with bounded entry counts and byte quotas.
  - Configurable deduplication policies (`None`, `IgnoreIfLatest`, `MoveToFrontIfLatest`, `DeduplicateAll`).
  - Item pinning (pinned items are protected from LRU eviction).
  - Fast case-insensitive substring search, MIME filtering, and time-window queries.
- **Streaming Transfer & Helpers (`mime_stream`)**:
  - Non-blocking pipe streaming I/O with poll-based timeout protection.
  - Strict RFC 3629 UTF-8 validation and sanitization.
  - Image signature and PNG IHDR dimension parser.
  - Fast HTML tag stripper and HTML entity decoder.
- **Native OS Platform Backends**:
  - **Linux**: Wayland client integration supporting both `ext-data-control-v1` and `zwlr-data-control-v1`.
  - **Windows**: Win32 `AddClipboardFormatListener` message pump backend with UTF-16/HTML format conversion.
  - **macOS**: `NSPasteboard` listener with changeCount tracking.

---

## Architecture & Layout

```
broclip/
├── CMakeLists.txt            # CMake build configuration with memory-bounded job pools
├── include/
│   └── broclip/
│       ├── backend.h         # Abstract platform backend interface
│       ├── broclip.h         # Umbrella include
│       ├── clip.h            # ClipboardManager and RAII ListenerSlot
│       ├── mime_stream.h     # Pipe streaming, UTF-8 checks, PNG parser, HTML extractor
│       ├── ring_store.h      # ClipboardStore interface and RingStore LRU buffer
│       ├── types.h           # ClipEntry, MimeType, SelectionKind, ClipSource, HistoryQuery
│       ├── linux/
│       │   └── data_control.h # Wayland ext/zwlr data-control client backend
│       ├── mac/
│       │   └── pasteboard.h   # macOS NSPasteboard backend
│       └── win/
│           └── clipboard.h    # Windows Win32 clipboard backend
├── protocols/
│   ├── ext-data-control-v1.xml
│   └── wlr-data-control-unstable-v1.xml
├── src/
│   ├── backend.cpp           # Platform backend factory
│   ├── clip.cpp              # ClipboardManager implementation
│   ├── mime_stream.cpp       # Streaming pipes and data inspection
│   ├── ring_store.cpp        # Thread-safe LRU ring buffer and search
│   ├── types.cpp             # Core types, MIME matching, ClipEntry helpers
│   ├── linux/
│   │   └── data_control.cpp  # Wayland data-control client implementation
│   ├── mac/
│   │   ├── pasteboard.mm     # macOS NSPasteboard backend
│   │   └── pasteboard_stub.cpp
│   └── win/
│       └── clipboard.cpp     # Win32 clipboard backend
└── tests/
    ├── check.h               # Assertion-free test harness
    ├── CMakeLists.txt
    ├── test_clip_types.cpp
    ├── test_clipboard_manager.cpp
    ├── test_data_control.cpp
    ├── test_mime_stream.cpp
    └── test_ring_store.cpp
```

---

## Building and Testing

### Prerequisites

- C++20 compliant compiler (GCC 11+, Clang 14+, MSVC 2022)
- CMake 3.24+
- Ninja build system
- Linux dependencies: `libwayland-client`, `wayland-scanner`, `pkg-config`
- Nothing else to check out: bronze (for the JavaScript binding) is a `bro_dependency()` pin in
  `CMakeLists.txt` (`cmake/bro_deps.cmake`), taken from `../bronze` when that working tree
  exists and otherwise fetched at configure.

### Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

### Run Tests

```bash
ctest --test-dir build --output-on-failure
```

---

## Quick Example

```cpp
#include <broclip/broclip.h>
#include <iostream>

int main() {
    broclip::ManagerOptions options;
    options.sync_primary = true;
    options.auto_preserve = true;
    options.store_config.max_entries = 500;
    options.store_config.max_bytes = 32 * 1024 * 1024; // 32 MB

    broclip::ClipboardManager manager(options);
    manager.start();

    // Subscribe to clipboard updates with an RAII token
    broclip::ListenerSlot slot = manager.on_clip([](const broclip::ClipEntry& entry) {
        std::cout << "New clip: " << entry.preview(60) << std::endl;
        if (auto text = entry.text()) {
            std::cout << "Payload text: " << *text << std::endl;
        }
    });

    // Programmatically set clipboard content
    manager.set_text("Hello from broclip!");

    // Search clipboard history
    broclip::HistoryQuery query;
    query.filter_text = "hello";
    query.max_results = 10;
    auto results = manager.history(query);

    for (const auto& item : results) {
        std::cout << "Match [" << item.id << "]: " << item.preview() << std::endl;
    }

    manager.stop();
    return 0;
}
```

---

## License

Part of the bro ecosystem. Licensed under the MIT License.
