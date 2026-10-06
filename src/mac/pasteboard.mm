#include "broclip/mac/pasteboard.h"

#include "broclip/types.h"

#include <chrono>

#if defined(__APPLE__)
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace broclip::mac {

MacPasteboardBackend::MacPasteboardBackend() = default;

MacPasteboardBackend::~MacPasteboardBackend() {
    stop();
}

bool MacPasteboardBackend::is_available() const {
    return true;
}

bool MacPasteboardBackend::start() {
    if (running_.load()) return true;

    running_.store(true);
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        last_change_count_ = [pb changeCount];
    }

    worker_thread_ = std::thread([this]() { poll_loop(); });
    return true;
}

void MacPasteboardBackend::stop() {
    if (!running_.exchange(false)) return;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool MacPasteboardBackend::is_running() const {
    return running_.load();
}

void MacPasteboardBackend::set_on_selection_change(SelectionChangeHandler handler) {
    std::lock_guard lock(mutex_);
    on_selection_change_ = std::move(handler);
}

void MacPasteboardBackend::set_on_selection_clear(SelectionClearHandler handler) {
    std::lock_guard lock(mutex_);
    on_selection_clear_ = std::move(handler);
}

ClipEntry MacPasteboardBackend::read_pasteboard_locked() {
    ClipEntry entry;
    entry.kind = SelectionKind::Clipboard;
    entry.source.kind = ClipSourceKind::MacPasteboard;
    entry.timestamp = std::chrono::system_clock::now();

    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];

        // Text
        NSString* str = [pb stringForType:NSPasteboardTypeString];
        if (str && [str length] > 0) {
            entry.set_text([str UTF8String]);
        }

        // HTML
        NSString* html = [pb stringForType:NSPasteboardTypeHTML];
        if (html && [html length] > 0) {
            const char* utf8 = [html UTF8String];
            size_t len = std::strlen(utf8);
            std::vector<uint8_t> bytes(utf8, utf8 + len);
            entry.add_payload(std::string(mime::kTextHtml), std::move(bytes));
        }

        // PNG
        NSData* pngData = [pb dataForType:NSPasteboardTypePNG];
        if (pngData && [pngData length] > 0) {
            const auto* bytes = static_cast<const uint8_t*>([pngData bytes]);
            size_t len = [pngData length];
            std::vector<uint8_t> vec(bytes, bytes + len);
            entry.add_payload(std::string(mime::kImagePng), std::move(vec));
        }
    }

    return entry;
}

bool MacPasteboardBackend::set_selection(SelectionKind kind, const ClipEntry& entry) {
    if (kind != SelectionKind::Clipboard) {
        return false;
    }

    std::lock_guard lock(mutex_);
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];

        if (auto txt = entry.text()) {
            NSString* str = [NSString stringWithUTF8String:txt->c_str()];
            if (str) {
                [pb setString:str forType:NSPasteboardTypeString];
            }
        }

        if (const auto* html = entry.find_payload(mime::kTextHtml)) {
            NSString* hstr = [[NSString alloc] initWithBytes:html->data.data()
                                                      length:html->data.size()
                                                    encoding:NSUTF8StringEncoding];
            if (hstr) {
                [pb setString:hstr forType:NSPasteboardTypeHTML];
            }
        }

        if (const auto* png = entry.find_payload(mime::kImagePng)) {
            NSData* data = [NSData dataWithBytes:png->data.data() length:png->data.size()];
            if (data) {
                [pb setData:data forType:NSPasteboardTypePNG];
            }
        }

        last_change_count_ = [pb changeCount];
    }
    return true;
}

bool MacPasteboardBackend::clear_selection(SelectionKind kind) {
    if (kind != SelectionKind::Clipboard) return false;
    std::lock_guard lock(mutex_);
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        last_change_count_ = [pb changeCount];
    }
    return true;
}

void MacPasteboardBackend::poll_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!running_.load()) break;

        int64_t current_count = 0;
        @autoreleasepool {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            current_count = [pb changeCount];
        }

        if (current_count != last_change_count_) {
            last_change_count_ = current_count;

            ClipEntry entry;
            {
                std::lock_guard lock(mutex_);
                entry = read_pasteboard_locked();
            }

            if (!entry.payloads.empty()) {
                SelectionChangeHandler cb;
                {
                    std::lock_guard lock(mutex_);
                    cb = on_selection_change_;
                }
                if (cb) {
                    cb(std::move(entry));
                }
            }
        }
    }
}

}  // namespace broclip::mac

#else

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
