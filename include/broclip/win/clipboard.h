#pragma once

#include "broclip/backend.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace broclip::win {

class Win32ClipboardBackend : public ClipboardBackend {
public:
    Win32ClipboardBackend();
    ~Win32ClipboardBackend() override;

    bool is_available() const override;
    const char* name() const override { return "win32_clipboard"; }

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
    void* hwnd_{nullptr};
    SelectionChangeHandler on_selection_change_;
    SelectionClearHandler on_selection_clear_;

#if defined(_WIN32)
    void message_loop();
    ClipEntry read_clipboard_locked();
#endif
};

}  // namespace broclip::win
