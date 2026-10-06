#pragma once

#include "broclip/backend.h"
#include "broclip/ring_store.h"
#include "broclip/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace broclip {

class ListenerSlot {
public:
    ListenerSlot() = default;
    explicit ListenerSlot(std::function<void()> disconnect_fn);
    ~ListenerSlot();

    ListenerSlot(const ListenerSlot&) = delete;
    ListenerSlot& operator=(const ListenerSlot&) = delete;

    ListenerSlot(ListenerSlot&& other) noexcept;
    ListenerSlot& operator=(ListenerSlot&& other) noexcept;

    void disconnect();
    bool is_connected() const noexcept { return static_cast<bool>(disconnect_fn_); }

private:
    std::function<void()> disconnect_fn_;
};

struct ManagerOptions {
    bool sync_primary = false;
    bool auto_preserve = true;
    StoreConfig store_config;
};

class ClipboardManager {
public:
    using ClipCallback = std::function<void(const ClipEntry&)>;
    using ClearCallback = std::function<void(SelectionKind)>;

    explicit ClipboardManager(ManagerOptions options = {},
                              std::shared_ptr<ClipboardStore> store = nullptr,
                              std::unique_ptr<ClipboardBackend> backend = nullptr);
    ~ClipboardManager();

    ClipboardManager(const ClipboardManager&) = delete;
    ClipboardManager& operator=(const ClipboardManager&) = delete;

    bool start();
    void stop();
    bool is_running() const noexcept { return running_.load(); }

    bool sync_primary() const noexcept { return sync_primary_.load(); }
    void set_sync_primary(bool enable) noexcept { sync_primary_.store(enable); }

    bool auto_preserve() const noexcept { return auto_preserve_.load(); }
    void set_auto_preserve(bool enable) noexcept { auto_preserve_.store(enable); }

    // History and retrieval
    std::vector<ClipEntry> history(const HistoryQuery& query = {}) const;
    std::optional<ClipEntry> current(SelectionKind kind = SelectionKind::Clipboard) const;
    std::optional<ClipEntry> get(uint64_t id) const;
    bool remove(uint64_t id);
    void clear(std::optional<SelectionKind> kind = std::nullopt);
    size_t count(std::optional<SelectionKind> kind = std::nullopt) const;
    bool set_pinned(uint64_t id, bool pinned);

    ClipboardStore& store() noexcept { return *store_; }
    const ClipboardStore& store() const noexcept { return *store_; }

    ClipboardBackend* backend() noexcept { return backend_.get(); }
    const ClipboardBackend* backend() const noexcept { return backend_.get(); }

    // Programmatic clipboard updates
    bool set_clip(ClipEntry entry);
    bool set_text(std::string_view text, SelectionKind kind = SelectionKind::Clipboard);
    bool set_payload(std::string mime, std::vector<uint8_t> data,
                     SelectionKind kind = SelectionKind::Clipboard);
    bool clear_selection(SelectionKind kind = SelectionKind::Clipboard);

    // Event listeners
    ListenerSlot on_clip(ClipCallback cb);
    ListenerSlot on_clear(ClearCallback cb);

    // Internal hook called when backend delivers a new entry or clear
    void handle_backend_entry(ClipEntry entry);
    void handle_backend_clear(SelectionKind kind);

private:
    void notify_clip(const ClipEntry& entry);
    void notify_clear(SelectionKind kind);

    ManagerOptions options_;
    std::shared_ptr<ClipboardStore> store_;
    std::unique_ptr<ClipboardBackend> backend_;

    std::atomic<bool> running_{false};
    std::atomic<bool> sync_primary_{false};
    std::atomic<bool> auto_preserve_{true};

    mutable std::mutex listener_mutex_;
    uint64_t next_listener_id_{1};
    std::unordered_map<uint64_t, ClipCallback> clip_listeners_;
    std::unordered_map<uint64_t, ClearCallback> clear_listeners_;
};

}  // namespace broclip
