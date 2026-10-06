#include "broclip/backend.h"

#if defined(__linux__)
#include "broclip/linux/data_control.h"
#elif defined(_WIN32)
#include "broclip/win/clipboard.h"
#elif defined(__APPLE__)
#include "broclip/mac/pasteboard.h"
#endif

namespace broclip {

std::unique_ptr<ClipboardBackend> create_platform_backend() {
#if defined(__linux__)
    return std::make_unique<linux_backend::WaylandDataControlBackend>();
#elif defined(_WIN32)
    return std::make_unique<win::Win32ClipboardBackend>();
#elif defined(__APPLE__)
    return std::make_unique<mac::MacPasteboardBackend>();
#else
    return nullptr;
#endif
}

}  // namespace broclip
