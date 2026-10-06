#include "check.h"
#include "broclip/clip.h"

#include <atomic>
#include <string>
#include <vector>

class TestBackend : public broclip::ClipboardBackend {
public:
    bool is_available() const override { return true; }
    const char* name() const override { return "test_backend"; }

    bool start() override {
        running_ = true;
        return true;
    }

    void stop() override {
        running_ = false;
    }

    bool is_running() const override {
        return running_;
    }

    bool set_selection(broclip::SelectionKind kind, const broclip::ClipEntry& entry) override {
        if (kind == broclip::SelectionKind::Clipboard) {
            last_clipboard_ = entry;
            clipboard_set_count_++;
        } else {
            last_primary_ = entry;
            primary_set_count_++;
        }
        return true;
    }

    bool clear_selection(broclip::SelectionKind kind) override {
        if (kind == broclip::SelectionKind::Clipboard) {
            last_clipboard_ = {};
        } else {
            last_primary_ = {};
        }
        return true;
    }

    void set_on_selection_change(SelectionChangeHandler handler) override {
        change_handler_ = std::move(handler);
    }

    void set_on_selection_clear(SelectionClearHandler handler) override {
        clear_handler_ = std::move(handler);
    }

    void simulate_incoming_clip(broclip::ClipEntry entry) {
        if (change_handler_) {
            change_handler_(std::move(entry));
        }
    }

    void simulate_incoming_clear(broclip::SelectionKind kind) {
        if (clear_handler_) {
            clear_handler_(kind);
        }
    }

    bool running_{false};
    broclip::ClipEntry last_clipboard_;
    broclip::ClipEntry last_primary_;
    int clipboard_set_count_{0};
    int primary_set_count_{0};
    SelectionChangeHandler change_handler_;
    SelectionClearHandler clear_handler_;
};

static void test_manager_basic_lifecycle() {
    auto backend = std::make_unique<TestBackend>();
    auto* raw_backend = backend.get();

    broclip::ManagerOptions opts;
    opts.sync_primary = false;
    opts.auto_preserve = true;

    broclip::ClipboardManager mgr(opts, nullptr, std::move(backend));
    CHECK(!mgr.is_running());

    CHECK(mgr.start());
    CHECK(mgr.is_running());
    CHECK(raw_backend->is_running());

    mgr.stop();
    CHECK(!mgr.is_running());
    CHECK(!raw_backend->is_running());
}

static void test_manager_listener_slots() {
    auto backend = std::make_unique<TestBackend>();
    broclip::ClipboardManager mgr({}, nullptr, std::move(backend));

    std::vector<std::string> received;
    {
        broclip::ListenerSlot slot = mgr.on_clip([&received](const broclip::ClipEntry& e) {
            if (auto t = e.text()) {
                received.push_back(*t);
            }
        });

        CHECK(slot.is_connected());
        mgr.set_text("First Text");
        CHECK_EQ(received.size(), 1u);
        CHECK_EQ(received[0], "First Text");

        // Disconnect manually
        slot.disconnect();
        CHECK(!slot.is_connected());

        mgr.set_text("Second Text");
        // Should not receive because disconnected
        CHECK_EQ(received.size(), 1u);
    }

    // RAII disconnect on scope exit
    {
        broclip::ListenerSlot slot2 = mgr.on_clip([&received](const broclip::ClipEntry& e) {
            if (auto t = e.text()) received.push_back(*t);
        });
        mgr.set_text("Third Text");
        CHECK_EQ(received.size(), 2u);
    }
    // Now slot2 is destructed
    mgr.set_text("Fourth Text");
    CHECK_EQ(received.size(), 2u);
}

static void test_manager_sync_primary() {
    auto backend = std::make_unique<TestBackend>();
    auto* raw = backend.get();

    broclip::ManagerOptions opts;
    opts.sync_primary = true;
    broclip::ClipboardManager mgr(opts, nullptr, std::move(backend));

    mgr.set_text("Synced Selection", broclip::SelectionKind::Clipboard);

    CHECK_EQ(raw->clipboard_set_count_, 1);
    CHECK_EQ(raw->primary_set_count_, 1);
    CHECK_EQ(raw->last_clipboard_.text().value_or(""), "Synced Selection");
    CHECK_EQ(raw->last_primary_.text().value_or(""), "Synced Selection");

    // Both selections now present in store
    auto clip = mgr.current(broclip::SelectionKind::Clipboard);
    auto pri = mgr.current(broclip::SelectionKind::Primary);
    REQUIRE(clip.has_value());
    REQUIRE(pri.has_value());
    CHECK_EQ(clip->text().value_or(""), "Synced Selection");
    CHECK_EQ(pri->text().value_or(""), "Synced Selection");
}

static void test_manager_incoming_backend_events() {
    auto backend = std::make_unique<TestBackend>();
    auto* raw = backend.get();

    broclip::ClipboardManager mgr({}, nullptr, std::move(backend));

    std::string received;
    auto slot = mgr.on_clip([&received](const broclip::ClipEntry& e) {
        if (auto t = e.text()) received = *t;
    });

    broclip::ClipEntry ext;
    ext.source.kind = broclip::ClipSourceKind::WaylandDataControl;
    ext.source.app_name = "firefox";
    ext.set_text("Copied from browser");

    raw->simulate_incoming_clip(ext);

    CHECK_EQ(received, "Copied from browser");
    auto cur = mgr.current();
    REQUIRE(cur.has_value());
    CHECK_EQ(cur->source.app_name, "firefox");
    CHECK_EQ(cur->source.kind, broclip::ClipSourceKind::WaylandDataControl);
}

int main() {
    test_manager_basic_lifecycle();
    test_manager_listener_slots();
    test_manager_sync_primary();
    test_manager_incoming_backend_events();

    return bstest::finish("test_clipboard_manager");
}
