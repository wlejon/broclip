#include "broclip/mac/pasteboard.h"

#if !defined(__APPLE__)
namespace broclip::mac {

MacPasteboardBackend::MacPasteboardBackend() = default;
MacPasteboardBackend::~MacPasteboardBackend() = default;
bool MacPasteboardBackend::is_available() const { return false; }
bool MacPasteboardBackend::start() { return false; }
void MacPasteboardBackend::stop() {}
bool MacPasteboardBackend::is_running() const { return false; }
bool MacPasteboardBackend::set_selection(SelectionKind, const ClipEntry&) { return false; }
bool MacPasteboardBackend::clear_selection(SelectionKind) { return false; }
void MacPasteboardBackend::set_on_selection_change(SelectionChangeHandler) {}
void MacPasteboardBackend::set_on_selection_clear(SelectionClearHandler) {}

}  // namespace broclip::mac
#endif
