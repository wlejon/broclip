#include "broclip/ring_store.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace broclip {

static bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

RingStore::RingStore(StoreConfig config) : config_(config) {}

uint64_t RingStore::put(ClipEntry entry) {
    std::unique_lock lock(mutex_);

    if (entry.id == 0) {
        entry.id = next_id_.fetch_add(1, std::memory_order_relaxed);
    }
    std::string fp = entry.fingerprint();

    // Deduplication check
    if (config_.dedup_policy == DedupPolicy::IgnoreIfLatest) {
        for (const auto& e : entries_) {
            if (e.kind == entry.kind) {
                if (e.fingerprint() == fp) {
                    return e.id;
                }
                break;
            }
        }
    } else if (config_.dedup_policy == DedupPolicy::MoveToFrontIfLatest) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->kind == entry.kind) {
                if (it->fingerprint() == fp) {
                    it->timestamp = std::chrono::system_clock::now();
                    if (it != entries_.begin()) {
                        entries_.splice(entries_.begin(), entries_, it);
                    }
                    return it->id;
                }
                break;
            }
        }
    } else if (config_.dedup_policy == DedupPolicy::DeduplicateAll) {
        auto f_it = fingerprint_map_.find(fp);
        if (f_it != fingerprint_map_.end()) {
            auto it = f_it->second;
            it->timestamp = std::chrono::system_clock::now();
            if (it != entries_.begin()) {
                entries_.splice(entries_.begin(), entries_, it);
            }
            return it->id;
        }
    }

    size_t needed_bytes = entry.size_bytes();
    if (config_.max_bytes > 0 && needed_bytes > config_.max_bytes) {
        // Entry itself exceeds entire byte quota
        return 0;
    }

    evict_to_fit(needed_bytes);

    uint64_t id = entry.id;
    entries_.push_front(std::move(entry));
    auto it = entries_.begin();
    id_map_[id] = it;
    fingerprint_map_[fp] = it;
    total_bytes_ += needed_bytes;

    return id;
}

std::optional<ClipEntry> RingStore::get(uint64_t id) const {
    std::shared_lock lock(mutex_);
    auto it = id_map_.find(id);
    if (it != id_map_.end()) {
        return *it->second;
    }
    return std::nullopt;
}

std::optional<ClipEntry> RingStore::latest(SelectionKind kind) const {
    std::shared_lock lock(mutex_);
    for (const auto& e : entries_) {
        if (e.kind == kind) {
            return e;
        }
    }
    return std::nullopt;
}

std::vector<ClipEntry> RingStore::query(const HistoryQuery& q) const {
    std::shared_lock lock(mutex_);
    std::vector<ClipEntry> results;

    size_t skipped = 0;
    for (const auto& e : entries_) {
        if (q.selection_kind.has_value() && e.kind != *q.selection_kind) {
            continue;
        }
        if (q.pinned_only.value_or(false) && !e.pinned) {
            continue;
        }
        if (q.since.has_value() && e.timestamp < *q.since) {
            continue;
        }
        if (q.mime_filter.has_value() && !e.has_mime(*q.mime_filter)) {
            continue;
        }
        if (q.filter_text.has_value() && !q.filter_text->empty()) {
            bool matches_text = false;
            if (auto txt = e.text()) {
                matches_text = contains_ci(*txt, *q.filter_text);
            }
            if (!matches_text) {
                matches_text = contains_ci(e.preview(256), *q.filter_text);
            }
            if (!matches_text) {
                continue;
            }
        }

        if (skipped < q.offset) {
            skipped++;
            continue;
        }

        results.push_back(e);
        if (q.max_results > 0 && results.size() >= q.max_results) {
            break;
        }
    }

    return results;
}

bool RingStore::remove(uint64_t id) {
    std::unique_lock lock(mutex_);
    auto it = id_map_.find(id);
    if (it == id_map_.end()) {
        return false;
    }
    remove_entry_locked(it->second);
    return true;
}

void RingStore::clear(std::optional<SelectionKind> kind) {
    std::unique_lock lock(mutex_);
    if (!kind.has_value()) {
        entries_.clear();
        id_map_.clear();
        fingerprint_map_.clear();
        total_bytes_ = 0;
        return;
    }

    auto it = entries_.begin();
    while (it != entries_.end()) {
        if (it->kind == *kind) {
            auto to_erase = it++;
            remove_entry_locked(to_erase);
        } else {
            ++it;
        }
    }
}

size_t RingStore::count(std::optional<SelectionKind> kind) const {
    std::shared_lock lock(mutex_);
    if (!kind.has_value()) {
        return entries_.size();
    }
    size_t n = 0;
    for (const auto& e : entries_) {
        if (e.kind == *kind) {
            n++;
        }
    }
    return n;
}

size_t RingStore::total_bytes() const {
    std::shared_lock lock(mutex_);
    return total_bytes_;
}

void RingStore::set_max_entries(size_t limit) {
    std::unique_lock lock(mutex_);
    config_.max_entries = limit;
    evict_to_fit(0);
}

void RingStore::set_max_bytes(size_t quota_bytes) {
    std::unique_lock lock(mutex_);
    config_.max_bytes = quota_bytes;
    evict_to_fit(0);
}

size_t RingStore::max_entries() const {
    std::shared_lock lock(mutex_);
    return config_.max_entries;
}

size_t RingStore::max_bytes() const {
    std::shared_lock lock(mutex_);
    return config_.max_bytes;
}

bool RingStore::set_pinned(uint64_t id, bool pinned) {
    std::unique_lock lock(mutex_);
    auto it = id_map_.find(id);
    if (it == id_map_.end()) {
        return false;
    }
    it->second->pinned = pinned;
    return true;
}

DedupPolicy RingStore::dedup_policy() const {
    std::shared_lock lock(mutex_);
    return config_.dedup_policy;
}

void RingStore::set_dedup_policy(DedupPolicy policy) {
    std::unique_lock lock(mutex_);
    config_.dedup_policy = policy;
}

void RingStore::remove_entry_locked(std::list<ClipEntry>::iterator it) {
    size_t sz = it->size_bytes();
    if (total_bytes_ >= sz) {
        total_bytes_ -= sz;
    } else {
        total_bytes_ = 0;
    }

    id_map_.erase(it->id);
    fingerprint_map_.erase(it->fingerprint());
    entries_.erase(it);
}

void RingStore::evict_to_fit(size_t needed_bytes) {
    while (!entries_.empty()) {
        bool entry_limit_exceeded = (config_.max_entries > 0 && entries_.size() + 1 > config_.max_entries);
        bool byte_limit_exceeded = (config_.max_bytes > 0 && total_bytes_ + needed_bytes > config_.max_bytes);

        if (!entry_limit_exceeded && !byte_limit_exceeded) {
            break;
        }

        // Find oldest non-pinned item from back
        auto candidate = entries_.end();
        for (auto cur = entries_.rbegin(); cur != entries_.rend(); ++cur) {
            if (!cur->pinned) {
                candidate = std::next(cur).base();
                break;
            }
        }

        // If all items are pinned, evict oldest pinned to honor hard limit
        if (candidate == entries_.end()) {
            candidate = std::prev(entries_.end());
        }

        remove_entry_locked(candidate);
    }
}

}  // namespace broclip
