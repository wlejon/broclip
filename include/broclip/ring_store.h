#pragma once

#include "broclip/types.h"

#include <atomic>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace broclip {

enum class DedupPolicy : uint8_t {
    None = 0,
    IgnoreIfLatest,
    MoveToFrontIfLatest,
    DeduplicateAll,
};

struct StoreConfig {
    size_t max_entries = 1000;
    size_t max_bytes = 64 * 1024 * 1024;  // 64 MB
    DedupPolicy dedup_policy = DedupPolicy::MoveToFrontIfLatest;
};

class ClipboardStore {
public:
    virtual ~ClipboardStore() = default;

    virtual uint64_t put(ClipEntry entry) = 0;
    virtual std::optional<ClipEntry> get(uint64_t id) const = 0;
    virtual std::optional<ClipEntry> latest(SelectionKind kind = SelectionKind::Clipboard) const = 0;
    virtual std::vector<ClipEntry> query(const HistoryQuery& q) const = 0;
    virtual bool remove(uint64_t id) = 0;
    virtual void clear(std::optional<SelectionKind> kind = std::nullopt) = 0;
    virtual size_t count(std::optional<SelectionKind> kind = std::nullopt) const = 0;
    virtual size_t total_bytes() const = 0;
    virtual void set_max_entries(size_t limit) = 0;
    virtual void set_max_bytes(size_t quota_bytes) = 0;
    virtual size_t max_entries() const = 0;
    virtual size_t max_bytes() const = 0;
    virtual bool set_pinned(uint64_t id, bool pinned) = 0;
    virtual DedupPolicy dedup_policy() const = 0;
    virtual void set_dedup_policy(DedupPolicy policy) = 0;
};

class RingStore : public ClipboardStore {
public:
    explicit RingStore(StoreConfig config = {});
    ~RingStore() override = default;

    RingStore(const RingStore&) = delete;
    RingStore& operator=(const RingStore&) = delete;

    uint64_t put(ClipEntry entry) override;
    std::optional<ClipEntry> get(uint64_t id) const override;
    std::optional<ClipEntry> latest(SelectionKind kind = SelectionKind::Clipboard) const override;
    std::vector<ClipEntry> query(const HistoryQuery& q) const override;
    bool remove(uint64_t id) override;
    void clear(std::optional<SelectionKind> kind = std::nullopt) override;
    size_t count(std::optional<SelectionKind> kind = std::nullopt) const override;
    size_t total_bytes() const override;
    void set_max_entries(size_t limit) override;
    void set_max_bytes(size_t quota_bytes) override;
    size_t max_entries() const override;
    size_t max_bytes() const override;
    bool set_pinned(uint64_t id, bool pinned) override;
    DedupPolicy dedup_policy() const override;
    void set_dedup_policy(DedupPolicy policy) override;

private:
    void evict_to_fit(size_t needed_bytes);
    void remove_entry_locked(std::list<ClipEntry>::iterator it);

    mutable std::shared_mutex mutex_;
    StoreConfig config_;
    size_t total_bytes_{0};
    std::atomic<uint64_t> next_id_{1};
    std::list<ClipEntry> entries_;  // Front is newest, back is oldest
    std::unordered_map<uint64_t, std::list<ClipEntry>::iterator> id_map_;
    std::unordered_map<std::string, std::list<ClipEntry>::iterator> fingerprint_map_;
};

}  // namespace broclip
