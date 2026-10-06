#include "check.h"
#include "broclip/ring_store.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

static void test_basic_operations() {
    broclip::RingStore store;
    CHECK_EQ(store.count(), 0u);
    CHECK_EQ(store.total_bytes(), 0u);

    broclip::ClipEntry e1;
    e1.set_text("Hello World");
    uint64_t id1 = store.put(e1);
    CHECK(id1 > 0);
    CHECK_EQ(store.count(), 1u);
    CHECK(store.total_bytes() > 0);

    auto got1 = store.get(id1);
    REQUIRE(got1.has_value());
    CHECK_EQ(got1->text().value_or(""), "Hello World");

    auto latest1 = store.latest(broclip::SelectionKind::Clipboard);
    REQUIRE(latest1.has_value());
    CHECK_EQ(latest1->id, id1);

    broclip::ClipEntry e2;
    e2.kind = broclip::SelectionKind::Primary;
    e2.set_text("Primary Selection");
    uint64_t id2 = store.put(e2);
    CHECK(id2 > id1);
    CHECK_EQ(store.count(), 2u);
    CHECK_EQ(store.count(broclip::SelectionKind::Primary), 1u);
    CHECK_EQ(store.count(broclip::SelectionKind::Clipboard), 1u);

    auto latest_pri = store.latest(broclip::SelectionKind::Primary);
    REQUIRE(latest_pri.has_value());
    CHECK_EQ(latest_pri->text().value_or(""), "Primary Selection");

    // Remove
    CHECK(store.remove(id1));
    CHECK_EQ(store.count(broclip::SelectionKind::Clipboard), 0u);
    CHECK(!store.get(id1).has_value());

    // Clear
    store.clear();
    CHECK_EQ(store.count(), 0u);
    CHECK_EQ(store.total_bytes(), 0u);
}

static void test_max_entries_eviction() {
    broclip::StoreConfig cfg;
    cfg.max_entries = 4;
    cfg.dedup_policy = broclip::DedupPolicy::None;
    broclip::RingStore store(cfg);

    std::vector<uint64_t> ids;
    for (int i = 0; i < 8; ++i) {
        broclip::ClipEntry e;
        e.set_text("Entry " + std::to_string(i));
        ids.push_back(store.put(e));
    }

    CHECK_EQ(store.count(), 4u);
    // Oldest 4 (0, 1, 2, 3) must have been evicted, 4, 5, 6, 7 must remain
    CHECK(!store.get(ids[0]).has_value());
    CHECK(!store.get(ids[1]).has_value());
    CHECK(!store.get(ids[2]).has_value());
    CHECK(!store.get(ids[3]).has_value());

    CHECK(store.get(ids[4]).has_value());
    CHECK(store.get(ids[5]).has_value());
    CHECK(store.get(ids[6]).has_value());
    CHECK(store.get(ids[7]).has_value());

    auto q = store.query({});
    REQUIRE(q.size() == 4u);
    CHECK_EQ(q[0].id, ids[7]);  // Most recent first
    CHECK_EQ(q[1].id, ids[6]);
    CHECK_EQ(q[2].id, ids[5]);
    CHECK_EQ(q[3].id, ids[4]);
}

static void test_byte_quota_eviction() {
    broclip::StoreConfig cfg;
    cfg.max_entries = 100;
    cfg.max_bytes = 4000;
    cfg.dedup_policy = broclip::DedupPolicy::None;
    broclip::RingStore store(cfg);

    // Single entry that exceeds max_bytes directly should be rejected
    broclip::ClipEntry huge;
    std::vector<uint8_t> huge_data(5000, 0xAA);
    huge.add_payload("application/octet-stream", std::move(huge_data));
    uint64_t huge_id = store.put(huge);
    CHECK_EQ(huge_id, 0u);
    CHECK_EQ(store.count(), 0u);

    // Insert entries each ~1000 bytes
    std::vector<uint64_t> ids;
    for (int i = 0; i < 6; ++i) {
        broclip::ClipEntry e;
        std::vector<uint8_t> data(1000, static_cast<uint8_t>(i));
        e.add_payload("application/octet-stream", std::move(data));
        ids.push_back(store.put(e));
        CHECK(store.total_bytes() <= 4000);
    }

    CHECK(store.total_bytes() <= 4000);
    // Older ones should have been evicted to respect 4000 bytes
    CHECK(!store.get(ids[0]).has_value());
    CHECK(!store.get(ids[1]).has_value());
    CHECK(store.get(ids[5]).has_value());
}

static void test_pinned_protection() {
    broclip::StoreConfig cfg;
    cfg.max_entries = 3;
    cfg.dedup_policy = broclip::DedupPolicy::None;
    broclip::RingStore store(cfg);

    broclip::ClipEntry e0;
    e0.set_text("Special Pinned Entry");
    uint64_t id0 = store.put(e0);
    CHECK(store.set_pinned(id0, true));

    broclip::ClipEntry e1;
    e1.set_text("Normal 1");
    uint64_t id1 = store.put(e1);

    broclip::ClipEntry e2;
    e2.set_text("Normal 2");
    uint64_t id2 = store.put(e2);

    CHECK_EQ(store.count(), 3u);

    // Adding Normal 3 will exceed max_entries (3). It should evict Normal 1, NOT the pinned item!
    broclip::ClipEntry e3;
    e3.set_text("Normal 3");
    uint64_t id3 = store.put(e3);

    CHECK_EQ(store.count(), 3u);
    CHECK(store.get(id0).has_value());   // Pinned remains!
    CHECK(!store.get(id1).has_value());  // Normal 1 was evicted!
    CHECK(store.get(id2).has_value());   // Normal 2 remains!
    CHECK(store.get(id3).has_value());   // Normal 3 remains!
}

static void test_deduplication_policies() {
    // 1. None
    {
        broclip::StoreConfig cfg;
        cfg.dedup_policy = broclip::DedupPolicy::None;
        broclip::RingStore store(cfg);
        broclip::ClipEntry e;
        e.set_text("Same Content");
        uint64_t id1 = store.put(e);
        uint64_t id2 = store.put(e);
        CHECK(id1 != id2);
        CHECK_EQ(store.count(), 2u);
    }

    // 2. IgnoreIfLatest
    {
        broclip::StoreConfig cfg;
        cfg.dedup_policy = broclip::DedupPolicy::IgnoreIfLatest;
        broclip::RingStore store(cfg);
        broclip::ClipEntry e;
        e.set_text("Repeat Text");
        uint64_t id1 = store.put(e);
        uint64_t id2 = store.put(e);
        CHECK_EQ(id1, id2);
        CHECK_EQ(store.count(), 1u);

        // Different content should be added
        broclip::ClipEntry e2;
        e2.set_text("Other Text");
        uint64_t id3 = store.put(e2);
        CHECK(id3 != id1);
        CHECK_EQ(store.count(), 2u);
    }

    // 3. MoveToFrontIfLatest
    {
        broclip::StoreConfig cfg;
        cfg.dedup_policy = broclip::DedupPolicy::MoveToFrontIfLatest;
        broclip::RingStore store(cfg);
        broclip::ClipEntry e;
        e.set_text("Same Text");
        uint64_t id1 = store.put(e);
        uint64_t id2 = store.put(e);
        CHECK_EQ(id1, id2);
        CHECK_EQ(store.count(), 1u);
    }

    // 4. DeduplicateAll
    {
        broclip::StoreConfig cfg;
        cfg.dedup_policy = broclip::DedupPolicy::DeduplicateAll;
        broclip::RingStore store(cfg);

        broclip::ClipEntry e1;
        e1.set_text("Item A");
        uint64_t id1 = store.put(e1);

        broclip::ClipEntry e2;
        e2.set_text("Item B");
        uint64_t id2 = store.put(e2);
        CHECK(id2 != id1);

        // Put Item A again: should match older Item A, move it to front, and reuse id1
        broclip::ClipEntry e3;
        e3.set_text("Item A");
        uint64_t id3 = store.put(e3);

        CHECK_EQ(id3, id1);
        CHECK_EQ(store.count(), 2u);
        auto latest = store.latest();
        REQUIRE(latest.has_value());
        CHECK_EQ(latest->id, id1);
    }
}

static void test_search_and_query() {
    broclip::RingStore store;

    broclip::ClipEntry e1;
    e1.set_text("Apple Banana Orange");
    store.put(e1);

    broclip::ClipEntry e2;
    e2.set_text("Banana Cherry Date");
    e2.pinned = true;
    store.put(e2);

    broclip::ClipEntry e3;
    e3.kind = broclip::SelectionKind::Primary;
    e3.set_text("Pineapple Grapefruit");
    store.put(e3);

    broclip::ClipEntry e4;
    std::vector<uint8_t> png_dummy{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    e4.add_payload(std::string(broclip::mime::kImagePng), std::move(png_dummy));
    store.put(e4);

    // Text search: case-insensitive "banana"
    broclip::HistoryQuery q_text;
    q_text.filter_text = "banana";
    auto res_text = store.query(q_text);
    CHECK_EQ(res_text.size(), 2u);

    // Filter text "pineapple"
    broclip::HistoryQuery q_pine;
    q_pine.filter_text = "pine";
    auto res_pine = store.query(q_pine);
    CHECK_EQ(res_pine.size(), 1u);
    CHECK_EQ(res_pine[0].kind, broclip::SelectionKind::Primary);

    // MIME filter
    broclip::HistoryQuery q_mime;
    q_mime.mime_filter = std::string(broclip::mime::kImagePng);
    auto res_mime = store.query(q_mime);
    CHECK_EQ(res_mime.size(), 1u);

    // Pinned filter
    broclip::HistoryQuery q_pin;
    q_pin.pinned_only = true;
    auto res_pin = store.query(q_pin);
    CHECK_EQ(res_pin.size(), 1u);
    CHECK_EQ(res_pin[0].text().value_or(""), "Banana Cherry Date");

    // SelectionKind filter
    broclip::HistoryQuery q_pri;
    q_pri.selection_kind = broclip::SelectionKind::Primary;
    auto res_pri = store.query(q_pri);
    CHECK_EQ(res_pri.size(), 1u);

    // Pagination
    broclip::HistoryQuery q_page;
    q_page.max_results = 2;
    q_page.offset = 1;
    auto res_page = store.query(q_page);
    CHECK_EQ(res_page.size(), 2u);
}

static void test_concurrency() {
    broclip::StoreConfig cfg;
    cfg.max_entries = 200;
    cfg.dedup_policy = broclip::DedupPolicy::None;
    broclip::RingStore store(cfg);

    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&store, t]() {
            for (int i = 0; i < 50; ++i) {
                broclip::ClipEntry e;
                e.set_text("Thread " + std::to_string(t) + " Item " + std::to_string(i));
                store.put(e);
            }
        });
    }

    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t) {
        readers.emplace_back([&store]() {
            for (int i = 0; i < 50; ++i) {
                auto all = store.query({});
                (void)all.size();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    for (auto& w : writers) w.join();
    for (auto& r : readers) r.join();

    CHECK(store.count() <= 200);
    CHECK(store.count() > 0);
}

int main() {
    test_basic_operations();
    test_max_entries_eviction();
    test_byte_quota_eviction();
    test_pinned_protection();
    test_deduplication_policies();
    test_search_and_query();
    test_concurrency();

    return bstest::finish("test_ring_store");
}
