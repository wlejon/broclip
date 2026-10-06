#include "broclip/clip.h"

namespace broclip {

ListenerSlot::ListenerSlot(std::function<void()> disconnect_fn)
    : disconnect_fn_(std::move(disconnect_fn)) {}

ListenerSlot::~ListenerSlot() {
    disconnect();
}

ListenerSlot::ListenerSlot(ListenerSlot&& other) noexcept
    : disconnect_fn_(std::move(other.disconnect_fn_)) {
    other.disconnect_fn_ = nullptr;
}

ListenerSlot& ListenerSlot::operator=(ListenerSlot&& other) noexcept {
    if (this != &other) {
        disconnect();
        disconnect_fn_ = std::move(other.disconnect_fn_);
        other.disconnect_fn_ = nullptr;
    }
    return *this;
}

void ListenerSlot::disconnect() {
    if (disconnect_fn_) {
        auto fn = std::move(disconnect_fn_);
        disconnect_fn_ = nullptr;
        fn();
    }
}

ClipboardManager::ClipboardManager(ManagerOptions options,
                                   std::shared_ptr<ClipboardStore> store,
                                   std::unique_ptr<ClipboardBackend> backend)
    : options_(std::move(options)),
      store_(store ? std::move(store) : std::make_shared<RingStore>(options_.store_config)),
      backend_(backend ? std::move(backend) : create_platform_backend()),
      sync_primary_(options_.sync_primary),
      auto_preserve_(options_.auto_preserve) {
    if (backend_) {
        backend_->set_on_selection_change([this](ClipEntry entry) {
            handle_backend_entry(std::move(entry));
        });
        backend_->set_on_selection_clear([this](SelectionKind kind) {
            handle_backend_clear(kind);
        });
    }
}

ClipboardManager::~ClipboardManager() {
    stop();
}

bool ClipboardManager::start() {
    if (running_.exchange(true)) return true;

    if (backend_ && backend_->is_available()) {
        backend_->start();
    }
    return true;
}

void ClipboardManager::stop() {
    if (!running_.exchange(false)) return;

    if (backend_) {
        backend_->stop();
    }
}

std::vector<ClipEntry> ClipboardManager::history(const HistoryQuery& query) const {
    return store_->query(query);
}

std::optional<ClipEntry> ClipboardManager::current(SelectionKind kind) const {
    return store_->latest(kind);
}

std::optional<ClipEntry> ClipboardManager::get(uint64_t id) const {
    return store_->get(id);
}

bool ClipboardManager::remove(uint64_t id) {
    return store_->remove(id);
}

void ClipboardManager::clear(std::optional<SelectionKind> kind) {
    store_->clear(kind);
    if (kind.has_value()) {
        notify_clear(*kind);
    } else {
        notify_clear(SelectionKind::Clipboard);
        notify_clear(SelectionKind::Primary);
    }
}

size_t ClipboardManager::count(std::optional<SelectionKind> kind) const {
    return store_->count(kind);
}

bool ClipboardManager::set_pinned(uint64_t id, bool pinned) {
    return store_->set_pinned(id, pinned);
}

bool ClipboardManager::set_clip(ClipEntry entry) {
    SelectionKind kind = entry.kind;
    uint64_t id = store_->put(entry);
    auto stored = store_->get(id);

    if (backend_) {
        backend_->set_selection(kind, stored ? *stored : entry);
    }

    if (stored) {
        notify_clip(*stored);
    }

    if (sync_primary_.load()) {
        SelectionKind other_kind = (kind == SelectionKind::Clipboard) ? SelectionKind::Primary : SelectionKind::Clipboard;
        ClipEntry synced = stored ? *stored : entry;
        synced.id = 0;
        synced.kind = other_kind;
        uint64_t synced_id = store_->put(synced);
        auto synced_stored = store_->get(synced_id);
        if (backend_) {
            backend_->set_selection(other_kind, synced_stored ? *synced_stored : synced);
        }
        if (synced_stored) {
            notify_clip(*synced_stored);
        }
    }

    return true;
}

bool ClipboardManager::set_text(std::string_view text, SelectionKind kind) {
    ClipEntry entry;
    entry.kind = kind;
    entry.source.kind = ClipSourceKind::LocalApp;
    entry.set_text(text);
    return set_clip(std::move(entry));
}

bool ClipboardManager::set_payload(std::string mime, std::vector<uint8_t> data,
                                   SelectionKind kind) {
    ClipEntry entry;
    entry.kind = kind;
    entry.source.kind = ClipSourceKind::LocalApp;
    entry.add_payload(std::move(mime), std::move(data));
    return set_clip(std::move(entry));
}

bool ClipboardManager::clear_selection(SelectionKind kind) {
    if (backend_) {
        backend_->clear_selection(kind);
    }
    notify_clear(kind);
    return true;
}

ListenerSlot ClipboardManager::on_clip(ClipCallback cb) {
    std::lock_guard lock(listener_mutex_);
    uint64_t id = next_listener_id_++;
    clip_listeners_[id] = std::move(cb);

    return ListenerSlot([this, id]() {
        std::lock_guard lock(listener_mutex_);
        clip_listeners_.erase(id);
    });
}

ListenerSlot ClipboardManager::on_clear(ClearCallback cb) {
    std::lock_guard lock(listener_mutex_);
    uint64_t id = next_listener_id_++;
    clear_listeners_[id] = std::move(cb);

    return ListenerSlot([this, id]() {
        std::lock_guard lock(listener_mutex_);
        clear_listeners_.erase(id);
    });
}

void ClipboardManager::handle_backend_entry(ClipEntry entry) {
    SelectionKind kind = entry.kind;
    uint64_t id = store_->put(entry);
    auto stored = store_->get(id);

    if (stored) {
        notify_clip(*stored);
    }

    if (sync_primary_.load()) {
        SelectionKind other_kind = (kind == SelectionKind::Clipboard) ? SelectionKind::Primary : SelectionKind::Clipboard;
        ClipEntry synced = stored ? *stored : entry;
        synced.id = 0;
        synced.kind = other_kind;
        uint64_t synced_id = store_->put(synced);
        auto synced_stored = store_->get(synced_id);
        if (backend_ && auto_preserve_.load()) {
            backend_->set_selection(other_kind, synced_stored ? *synced_stored : synced);
        }
        if (synced_stored) {
            notify_clip(*synced_stored);
        }
    }
}

void ClipboardManager::handle_backend_clear(SelectionKind kind) {
    notify_clear(kind);
}

void ClipboardManager::notify_clip(const ClipEntry& entry) {
    std::vector<ClipCallback> callbacks;
    {
        std::lock_guard lock(listener_mutex_);
        callbacks.reserve(clip_listeners_.size());
        for (const auto& [_, cb] : clip_listeners_) {
            callbacks.push_back(cb);
        }
    }
    for (const auto& cb : callbacks) {
        cb(entry);
    }
}

void ClipboardManager::notify_clear(SelectionKind kind) {
    std::vector<ClearCallback> callbacks;
    {
        std::lock_guard lock(listener_mutex_);
        callbacks.reserve(clear_listeners_.size());
        for (const auto& [_, cb] : clear_listeners_) {
            callbacks.push_back(cb);
        }
    }
    for (const auto& cb : callbacks) {
        cb(kind);
    }
}

}  // namespace broclip
