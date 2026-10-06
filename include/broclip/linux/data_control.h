#pragma once

#include "broclip/backend.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_source_v1;
struct ext_data_control_offer_v1;
struct zwlr_data_control_manager_v1;
struct zwlr_data_control_device_v1;
struct zwlr_data_control_source_v1;
struct zwlr_data_control_offer_v1;

namespace broclip::linux_backend {

class WaylandDataControlBackend : public ClipboardBackend {
public:
    WaylandDataControlBackend();
    ~WaylandDataControlBackend() override;

    bool is_available() const override;
    const char* name() const override { return "wayland_data_control"; }

    bool start() override;
    void stop() override;
    bool is_running() const override;

    bool set_selection(SelectionKind kind, const ClipEntry& entry) override;
    bool clear_selection(SelectionKind kind) override;

    void set_on_selection_change(SelectionChangeHandler handler) override;
    void set_on_selection_clear(SelectionClearHandler handler) override;

    // Direct fetch helper for a specific offer
    ClipEntry read_offer(uintptr_t offer_id, SelectionKind kind,
                         const std::vector<std::string>& mime_types);

    // Write helper when compositor requests a MIME send
    bool handle_send_request(SelectionKind kind, const std::string& mime, int fd);

private:
    void event_loop();
    void cleanup();

    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    int wakeup_pipe_[2]{-1, -1};

    wl_display* display_{nullptr};
    wl_registry* registry_{nullptr};
    wl_seat* seat_{nullptr};

    // ext-data-control-v1
    ext_data_control_manager_v1* ext_manager_{nullptr};
    ext_data_control_device_v1* ext_device_{nullptr};
    ext_data_control_source_v1* ext_clipboard_source_{nullptr};
    ext_data_control_source_v1* ext_primary_source_{nullptr};

    // zwlr-data-control-v1
    zwlr_data_control_manager_v1* wlr_manager_{nullptr};
    zwlr_data_control_device_v1* wlr_device_{nullptr};
    zwlr_data_control_source_v1* wlr_clipboard_source_{nullptr};
    zwlr_data_control_source_v1* wlr_primary_source_{nullptr};

    // Active owned selections to serve upon requests
    ClipEntry active_clipboard_entry_;
    ClipEntry active_primary_entry_;

    SelectionChangeHandler on_selection_change_;
    SelectionClearHandler on_selection_clear_;

    // Pending offers during receipt
    struct PendingOffer {
        SelectionKind kind = SelectionKind::Clipboard;
        std::vector<std::string> mimes;
    };
    std::unordered_map<uintptr_t, PendingOffer> pending_offers_;

    // Friend functions for Wayland C callbacks
    friend struct WaylandCallbacks;
};

}  // namespace broclip::linux_backend
