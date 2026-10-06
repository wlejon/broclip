#include "broclip/linux/data_control.h"

#include "broclip/mime_stream.h"
#include "broclip/types.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#if defined(__linux__)
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

namespace broclip::linux_backend {

struct WaylandCallbacks {
    // Registry listener
    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t version) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
            uint32_t v = std::min(version, 1u);
            self->ext_manager_ = static_cast<ext_data_control_manager_v1*>(
                wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, v));
        } else if (std::strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
            uint32_t v = std::min(version, 2u);
            self->wlr_manager_ = static_cast<zwlr_data_control_manager_v1*>(
                wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, v));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
            if (!self->seat_) {
                self->seat_ = static_cast<wl_seat*>(
                    wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
            }
        }
    }

    static void registry_global_remove(void*, wl_registry*, uint32_t) {}

    // ext-data-control offer listener
    static void ext_offer_offer(void* data, ext_data_control_offer_v1*, const char* mime_type) {
        auto* vec = static_cast<std::vector<std::string>*>(data);
        if (vec && mime_type) {
            vec->emplace_back(mime_type);
        }
    }

    // ext-data-control device listener
    static void ext_device_data_offer(void* data, ext_data_control_device_v1*,
                                      ext_data_control_offer_v1* id) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        std::lock_guard lock(self->mutex_);
        auto* mimes = new std::vector<std::string>();
        static const ext_data_control_offer_v1_listener offer_listener = {
            .offer = ext_offer_offer,
        };
        ext_data_control_offer_v1_add_listener(id, &offer_listener, mimes);
        self->pending_offers_[reinterpret_cast<uintptr_t>(id)] = {
            SelectionKind::Clipboard,
            {},
        };
        ext_data_control_offer_v1_set_user_data(id, mimes);
    }

    static void ext_device_selection(void* data, ext_data_control_device_v1*,
                                     ext_data_control_offer_v1* id) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (!id) {
            std::lock_guard lock(self->mutex_);
            if (self->on_selection_clear_) {
                self->on_selection_clear_(SelectionKind::Clipboard);
            }
            return;
        }

        std::vector<std::string> mimes;
        {
            std::lock_guard lock(self->mutex_);
            auto* p = static_cast<std::vector<std::string>*>(ext_data_control_offer_v1_get_user_data(id));
            if (p) {
                mimes = *p;
                delete p;
                ext_data_control_offer_v1_set_user_data(id, nullptr);
            }
            self->pending_offers_.erase(reinterpret_cast<uintptr_t>(id));
        }

        auto entry = self->read_offer(reinterpret_cast<uintptr_t>(id), SelectionKind::Clipboard, mimes);
        ext_data_control_offer_v1_destroy(id);

        std::lock_guard lock(self->mutex_);
        if (self->on_selection_change_) {
            self->on_selection_change_(std::move(entry));
        }
    }

    static void ext_device_finished(void*, ext_data_control_device_v1*) {}

    static void ext_device_primary_selection(void* data, ext_data_control_device_v1*,
                                             ext_data_control_offer_v1* id) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (!id) {
            std::lock_guard lock(self->mutex_);
            if (self->on_selection_clear_) {
                self->on_selection_clear_(SelectionKind::Primary);
            }
            return;
        }

        std::vector<std::string> mimes;
        {
            std::lock_guard lock(self->mutex_);
            auto* p = static_cast<std::vector<std::string>*>(ext_data_control_offer_v1_get_user_data(id));
            if (p) {
                mimes = *p;
                delete p;
                ext_data_control_offer_v1_set_user_data(id, nullptr);
            }
            self->pending_offers_.erase(reinterpret_cast<uintptr_t>(id));
        }

        auto entry = self->read_offer(reinterpret_cast<uintptr_t>(id), SelectionKind::Primary, mimes);
        ext_data_control_offer_v1_destroy(id);

        std::lock_guard lock(self->mutex_);
        if (self->on_selection_change_) {
            self->on_selection_change_(std::move(entry));
        }
    }

    // ext source listener
    static void ext_source_send(void* data, ext_data_control_source_v1*, const char* mime_type, int32_t fd) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (self && mime_type) {
            self->handle_send_request(SelectionKind::Clipboard, mime_type, fd);
        } else if (fd >= 0) {
            ::close(fd);
        }
    }

    static void ext_source_cancelled(void* data, ext_data_control_source_v1* source) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (self) {
            std::lock_guard lock(self->mutex_);
            if (self->ext_clipboard_source_ == source) {
                self->ext_clipboard_source_ = nullptr;
            }
            if (self->ext_primary_source_ == source) {
                self->ext_primary_source_ = nullptr;
            }
        }
        ext_data_control_source_v1_destroy(source);
    }

    // zwlr-data-control offer listener
    static void wlr_offer_offer(void* data, zwlr_data_control_offer_v1*, const char* mime_type) {
        auto* vec = static_cast<std::vector<std::string>*>(data);
        if (vec && mime_type) {
            vec->emplace_back(mime_type);
        }
    }

    // zwlr-data-control device listener
    static void wlr_device_data_offer(void* data, zwlr_data_control_device_v1*,
                                      zwlr_data_control_offer_v1* id) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        std::lock_guard lock(self->mutex_);
        auto* mimes = new std::vector<std::string>();
        static const zwlr_data_control_offer_v1_listener offer_listener = {
            .offer = wlr_offer_offer,
        };
        zwlr_data_control_offer_v1_add_listener(id, &offer_listener, mimes);
        self->pending_offers_[reinterpret_cast<uintptr_t>(id)] = {
            SelectionKind::Clipboard,
            {},
        };
        zwlr_data_control_offer_v1_set_user_data(id, mimes);
    }

    static void wlr_device_selection(void* data, zwlr_data_control_device_v1*,
                                     zwlr_data_control_offer_v1* id) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (!id) {
            std::lock_guard lock(self->mutex_);
            if (self->on_selection_clear_) {
                self->on_selection_clear_(SelectionKind::Clipboard);
            }
            return;
        }

        std::vector<std::string> mimes;
        {
            std::lock_guard lock(self->mutex_);
            auto* p = static_cast<std::vector<std::string>*>(zwlr_data_control_offer_v1_get_user_data(id));
            if (p) {
                mimes = *p;
                delete p;
                zwlr_data_control_offer_v1_set_user_data(id, nullptr);
            }
            self->pending_offers_.erase(reinterpret_cast<uintptr_t>(id));
        }

        auto entry = self->read_offer(reinterpret_cast<uintptr_t>(id), SelectionKind::Clipboard, mimes);
        zwlr_data_control_offer_v1_destroy(id);

        std::lock_guard lock(self->mutex_);
        if (self->on_selection_change_) {
            self->on_selection_change_(std::move(entry));
        }
    }

    static void wlr_device_finished(void*, zwlr_data_control_device_v1*) {}

    static void wlr_device_primary_selection(void* data, zwlr_data_control_device_v1*,
                                             zwlr_data_control_offer_v1* id) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (!id) {
            std::lock_guard lock(self->mutex_);
            if (self->on_selection_clear_) {
                self->on_selection_clear_(SelectionKind::Primary);
            }
            return;
        }

        std::vector<std::string> mimes;
        {
            std::lock_guard lock(self->mutex_);
            auto* p = static_cast<std::vector<std::string>*>(zwlr_data_control_offer_v1_get_user_data(id));
            if (p) {
                mimes = *p;
                delete p;
                zwlr_data_control_offer_v1_set_user_data(id, nullptr);
            }
            self->pending_offers_.erase(reinterpret_cast<uintptr_t>(id));
        }

        auto entry = self->read_offer(reinterpret_cast<uintptr_t>(id), SelectionKind::Primary, mimes);
        zwlr_data_control_offer_v1_destroy(id);

        std::lock_guard lock(self->mutex_);
        if (self->on_selection_change_) {
            self->on_selection_change_(std::move(entry));
        }
    }

    // zwlr source listener
    static void wlr_source_send(void* data, zwlr_data_control_source_v1*, const char* mime_type, int32_t fd) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (self && mime_type) {
            self->handle_send_request(SelectionKind::Clipboard, mime_type, fd);
        } else if (fd >= 0) {
            ::close(fd);
        }
    }

    static void wlr_source_cancelled(void* data, zwlr_data_control_source_v1* source) {
        auto* self = static_cast<WaylandDataControlBackend*>(data);
        if (self) {
            std::lock_guard lock(self->mutex_);
            if (self->wlr_clipboard_source_ == source) {
                self->wlr_clipboard_source_ = nullptr;
            }
            if (self->wlr_primary_source_ == source) {
                self->wlr_primary_source_ = nullptr;
            }
        }
        zwlr_data_control_source_v1_destroy(source);
    }
};

WaylandDataControlBackend::WaylandDataControlBackend() = default;

WaylandDataControlBackend::~WaylandDataControlBackend() {
    stop();
}

bool WaylandDataControlBackend::is_available() const {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (!display || *display == '\0') {
        return false;
    }
    wl_display* test = wl_display_connect(nullptr);
    if (!test) return false;
    wl_display_disconnect(test);
    return true;
}

bool WaylandDataControlBackend::start() {
    if (running_.load()) return true;

    display_ = wl_display_connect(nullptr);
    if (!display_) return false;

    registry_ = wl_display_get_registry(display_);
    if (!registry_) {
        cleanup();
        return false;
    }

    static const wl_registry_listener reg_listener = {
        .global = WaylandCallbacks::registry_global,
        .global_remove = WaylandCallbacks::registry_global_remove,
    };
    wl_registry_add_listener(registry_, &reg_listener, this);

    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);

    if (!seat_ || (!ext_manager_ && !wlr_manager_)) {
        cleanup();
        return false;
    }

    if (ext_manager_) {
        ext_device_ = ext_data_control_manager_v1_get_data_device(ext_manager_, seat_);
        static const ext_data_control_device_v1_listener dev_listener = {
            .data_offer = WaylandCallbacks::ext_device_data_offer,
            .selection = WaylandCallbacks::ext_device_selection,
            .finished = WaylandCallbacks::ext_device_finished,
            .primary_selection = WaylandCallbacks::ext_device_primary_selection,
        };
        ext_data_control_device_v1_add_listener(ext_device_, &dev_listener, this);
    } else if (wlr_manager_) {
        wlr_device_ = zwlr_data_control_manager_v1_get_data_device(wlr_manager_, seat_);
        static const zwlr_data_control_device_v1_listener dev_listener = {
            .data_offer = WaylandCallbacks::wlr_device_data_offer,
            .selection = WaylandCallbacks::wlr_device_selection,
            .finished = WaylandCallbacks::wlr_device_finished,
            .primary_selection = WaylandCallbacks::wlr_device_primary_selection,
        };
        zwlr_data_control_device_v1_add_listener(wlr_device_, &dev_listener, this);
    }

    if (::pipe2(wakeup_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
        cleanup();
        return false;
    }

    running_.store(true);
    worker_thread_ = std::thread([this]() { event_loop(); });

    wl_display_flush(display_);
    return true;
}

void WaylandDataControlBackend::stop() {
    if (!running_.exchange(false)) return;

    if (wakeup_pipe_[1] >= 0) {
        char b = 1;
        (void)::write(wakeup_pipe_[1], &b, 1);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    cleanup();
}

bool WaylandDataControlBackend::is_running() const {
    return running_.load();
}

void WaylandDataControlBackend::cleanup() {
    std::lock_guard lock(mutex_);

    if (wakeup_pipe_[0] >= 0) { ::close(wakeup_pipe_[0]); wakeup_pipe_[0] = -1; }
    if (wakeup_pipe_[1] >= 0) { ::close(wakeup_pipe_[1]); wakeup_pipe_[1] = -1; }

    if (ext_clipboard_source_) { ext_data_control_source_v1_destroy(ext_clipboard_source_); ext_clipboard_source_ = nullptr; }
    if (ext_primary_source_) { ext_data_control_source_v1_destroy(ext_primary_source_); ext_primary_source_ = nullptr; }
    if (ext_device_) { ext_data_control_device_v1_destroy(ext_device_); ext_device_ = nullptr; }
    if (ext_manager_) { ext_data_control_manager_v1_destroy(ext_manager_); ext_manager_ = nullptr; }

    if (wlr_clipboard_source_) { zwlr_data_control_source_v1_destroy(wlr_clipboard_source_); wlr_clipboard_source_ = nullptr; }
    if (wlr_primary_source_) { zwlr_data_control_source_v1_destroy(wlr_primary_source_); wlr_primary_source_ = nullptr; }
    if (wlr_device_) { zwlr_data_control_device_v1_destroy(wlr_device_); wlr_device_ = nullptr; }
    if (wlr_manager_) { zwlr_data_control_manager_v1_destroy(wlr_manager_); wlr_manager_ = nullptr; }

    if (seat_) { wl_seat_destroy(seat_); seat_ = nullptr; }
    if (registry_) { wl_registry_destroy(registry_); registry_ = nullptr; }
    if (display_) { wl_display_disconnect(display_); display_ = nullptr; }
}

void WaylandDataControlBackend::set_on_selection_change(SelectionChangeHandler handler) {
    std::lock_guard lock(mutex_);
    on_selection_change_ = std::move(handler);
}

void WaylandDataControlBackend::set_on_selection_clear(SelectionClearHandler handler) {
    std::lock_guard lock(mutex_);
    on_selection_clear_ = std::move(handler);
}

ClipEntry WaylandDataControlBackend::read_offer(uintptr_t offer_id, SelectionKind kind,
                                               const std::vector<std::string>& mime_types) {
    ClipEntry entry;
    entry.kind = kind;
    entry.source.kind = ClipSourceKind::WaylandDataControl;
    entry.timestamp = std::chrono::system_clock::now();

    for (const auto& mime : mime_types) {
        PipePair pipe;
        if (!pipe.is_valid()) continue;

        int write_fd = pipe.release_write();
        if (ext_manager_) {
            auto* offer = reinterpret_cast<ext_data_control_offer_v1*>(offer_id);
            ext_data_control_offer_v1_receive(offer, mime.c_str(), write_fd);
        } else if (wlr_manager_) {
            auto* offer = reinterpret_cast<zwlr_data_control_offer_v1*>(offer_id);
            zwlr_data_control_offer_v1_receive(offer, mime.c_str(), write_fd);
        }
        ::close(write_fd);

        if (display_) {
            wl_display_flush(display_);
        }

        auto read_res = read_stream_all(pipe.read_fd(), 32 * 1024 * 1024);
        if (read_res.success) {
            entry.add_payload(mime, std::move(read_res.data));
        }
    }

    return entry;
}

bool WaylandDataControlBackend::handle_send_request(SelectionKind kind, const std::string& mime, int fd) {
    std::lock_guard lock(mutex_);
    const auto& entry = (kind == SelectionKind::Clipboard) ? active_clipboard_entry_ : active_primary_entry_;

    const auto* payload = entry.find_payload(mime);
    if (!payload) {
        if (fd >= 0) ::close(fd);
        return false;
    }

    write_stream_all(fd, payload->data.data(), payload->data.size());
    if (fd >= 0) ::close(fd);
    return true;
}

bool WaylandDataControlBackend::set_selection(SelectionKind kind, const ClipEntry& entry) {
    std::lock_guard lock(mutex_);
    if (kind == SelectionKind::Clipboard) {
        active_clipboard_entry_ = entry;
    } else {
        active_primary_entry_ = entry;
    }

    if (!display_) return true;

    if (ext_manager_ && ext_device_) {
        auto* src = ext_data_control_manager_v1_create_data_source(ext_manager_);
        for (const auto& p : entry.payloads) {
            ext_data_control_source_v1_offer(src, p.mime.c_str());
        }

        static const ext_data_control_source_v1_listener src_listener = {
            .send = WaylandCallbacks::ext_source_send,
            .cancelled = WaylandCallbacks::ext_source_cancelled,
        };
        ext_data_control_source_v1_add_listener(src, &src_listener, this);

        if (kind == SelectionKind::Clipboard) {
            if (ext_clipboard_source_) {
                ext_data_control_source_v1_destroy(ext_clipboard_source_);
            }
            ext_clipboard_source_ = src;
            active_clipboard_entry_ = entry;
            ext_data_control_device_v1_set_selection(ext_device_, src);
        } else {
            if (ext_primary_source_) {
                ext_data_control_source_v1_destroy(ext_primary_source_);
            }
            ext_primary_source_ = src;
            active_primary_entry_ = entry;
            ext_data_control_device_v1_set_primary_selection(ext_device_, src);
        }
        wl_display_flush(display_);
        return true;
    }

    if (wlr_manager_ && wlr_device_) {
        auto* src = zwlr_data_control_manager_v1_create_data_source(wlr_manager_);
        for (const auto& p : entry.payloads) {
            zwlr_data_control_source_v1_offer(src, p.mime.c_str());
        }

        static const zwlr_data_control_source_v1_listener src_listener = {
            .send = WaylandCallbacks::wlr_source_send,
            .cancelled = WaylandCallbacks::wlr_source_cancelled,
        };
        zwlr_data_control_source_v1_add_listener(src, &src_listener, this);

        if (kind == SelectionKind::Clipboard) {
            if (wlr_clipboard_source_) {
                zwlr_data_control_source_v1_destroy(wlr_clipboard_source_);
            }
            wlr_clipboard_source_ = src;
            active_clipboard_entry_ = entry;
            zwlr_data_control_device_v1_set_selection(wlr_device_, src);
        } else {
            if (wlr_primary_source_) {
                zwlr_data_control_source_v1_destroy(wlr_primary_source_);
            }
            wlr_primary_source_ = src;
            active_primary_entry_ = entry;
            zwlr_data_control_device_v1_set_primary_selection(wlr_device_, src);
        }
        wl_display_flush(display_);
        return true;
    }

    return false;
}

bool WaylandDataControlBackend::clear_selection(SelectionKind kind) {
    std::lock_guard lock(mutex_);
    if (kind == SelectionKind::Clipboard) {
        active_clipboard_entry_ = {};
    } else {
        active_primary_entry_ = {};
    }

    if (!display_) return true;

    if (ext_manager_ && ext_device_) {
        if (kind == SelectionKind::Clipboard) {
            if (ext_clipboard_source_) {
                ext_data_control_source_v1_destroy(ext_clipboard_source_);
                ext_clipboard_source_ = nullptr;
            }
            active_clipboard_entry_ = {};
            ext_data_control_device_v1_set_selection(ext_device_, nullptr);
        } else {
            if (ext_primary_source_) {
                ext_data_control_source_v1_destroy(ext_primary_source_);
                ext_primary_source_ = nullptr;
            }
            active_primary_entry_ = {};
            ext_data_control_device_v1_set_primary_selection(ext_device_, nullptr);
        }
        wl_display_flush(display_);
        return true;
    }

    if (wlr_manager_ && wlr_device_) {
        if (kind == SelectionKind::Clipboard) {
            if (wlr_clipboard_source_) {
                zwlr_data_control_source_v1_destroy(wlr_clipboard_source_);
                wlr_clipboard_source_ = nullptr;
            }
            active_clipboard_entry_ = {};
            zwlr_data_control_device_v1_set_selection(wlr_device_, nullptr);
        } else {
            if (wlr_primary_source_) {
                zwlr_data_control_source_v1_destroy(wlr_primary_source_);
                wlr_primary_source_ = nullptr;
            }
            active_primary_entry_ = {};
            zwlr_data_control_device_v1_set_primary_selection(wlr_device_, nullptr);
        }
        wl_display_flush(display_);
        return true;
    }

    return false;
}

void WaylandDataControlBackend::event_loop() {
    int wl_fd = wl_display_get_fd(display_);

    while (running_.load()) {
        while (wl_display_prepare_read(display_) != 0) {
            if (wl_display_dispatch_pending(display_) < 0) {
                return;
            }
        }

        wl_display_flush(display_);

        struct pollfd fds[2];
        fds[0].fd = wl_fd;
        fds[0].events = POLLIN | POLLERR | POLLHUP;
        fds[1].fd = wakeup_pipe_[0];
        fds[1].events = POLLIN;

        int ret = ::poll(fds, 2, -1);
        if (ret < 0) {
            wl_display_cancel_read(display_);
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN) {
            wl_display_cancel_read(display_);
            char buf[16];
            (void)::read(wakeup_pipe_[0], buf, sizeof(buf));
            break;
        }

        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(display_) < 0) {
                break;
            }
            if (wl_display_dispatch_pending(display_) < 0) {
                break;
            }
        } else {
            wl_display_cancel_read(display_);
        }
    }
}

}  // namespace broclip::linux_backend

#else

namespace broclip::linux_backend {

WaylandDataControlBackend::WaylandDataControlBackend() = default;
WaylandDataControlBackend::~WaylandDataControlBackend() = default;
bool WaylandDataControlBackend::is_available() const { return false; }
bool WaylandDataControlBackend::start() { return false; }
void WaylandDataControlBackend::stop() {}
bool WaylandDataControlBackend::is_running() const { return false; }
bool WaylandDataControlBackend::set_selection(SelectionKind, const ClipEntry&) { return false; }
bool WaylandDataControlBackend::clear_selection(SelectionKind) { return false; }
void WaylandDataControlBackend::set_on_selection_change(SelectionChangeHandler) {}
void WaylandDataControlBackend::set_on_selection_clear(SelectionClearHandler) {}
ClipEntry WaylandDataControlBackend::read_offer(uintptr_t, SelectionKind, const std::vector<std::string>&) { return {}; }
bool WaylandDataControlBackend::handle_send_request(SelectionKind, const std::string&, int) { return false; }
void WaylandDataControlBackend::event_loop() {}
void WaylandDataControlBackend::cleanup() {}

}  // namespace broclip::linux_backend

#endif
