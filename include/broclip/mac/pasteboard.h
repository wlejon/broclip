#pragma once

#include "broclip/backend.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace broclip::mac {

class MacPasteboardBackend : public ClipboardBackend {
public:
    MacPasteboardBackend();
    ~MacPasteboardBackend() override;

    bool is_available() const override;
    const char* name() const override { return "mac_pasteboard"; }

    bool start() override;
    void stop() override;
    bool is_running() const override;

    bool set_selection(SelectionKind kind, const ClipEntry& entry) override;
    bool clear_selection(SelectionKind kind) override;

    void set_on_selection_change(SelectionChangeHandler handler) override;
    void set_on_selection_clear(SelectionClearHandler handler) override;

private:
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    int64_t last_change_count_{0};
    SelectionChangeHandler on_selection_change_;
    SelectionClearHandler on_selection_clear_;

#if defined(__APPLE__)
    void poll_loop();
    ClipEntry read_pasteboard_locked();
#endif
};

}  // namespace broclip::mac
