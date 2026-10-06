#include "check.h"
#include "broclip/mime_stream.h"
#include "broclip/types.h"

#if defined(__linux__)
#include "broclip/linux/data_control.h"
#include <unistd.h>
#endif

#include <string>
#include <vector>

static void test_multi_mime_caching_simulation() {
    // Simulate multiple MIME transfers through pipes
    std::string text_content = "Broclip preserves your data across application exit.";
    std::string html_content = "<p>Broclip preserves your data across <b>application exit</b>.</p>";
    std::vector<uint8_t> png_content = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x01, 0x02, 0x03};

    broclip::ClipEntry cached_entry;
    cached_entry.kind = broclip::SelectionKind::Clipboard;
    cached_entry.source.kind = broclip::ClipSourceKind::WaylandDataControl;
    cached_entry.source.app_name = "test_app";

    // 1. Text transfer
    {
        broclip::PipePair text_pipe;
        REQUIRE(text_pipe.is_valid());
        broclip::write_stream_all(text_pipe.write_fd(), text_content);
        text_pipe.close_write();

        auto res = broclip::read_stream_all(text_pipe.read_fd());
        CHECK(res.success);
        cached_entry.add_payload(std::string(broclip::mime::kTextPlainUtf8), std::move(res.data));
    }

    // 2. HTML transfer
    {
        broclip::PipePair html_pipe;
        REQUIRE(html_pipe.is_valid());
        broclip::write_stream_all(html_pipe.write_fd(), html_content);
        html_pipe.close_write();

        auto res = broclip::read_stream_all(html_pipe.read_fd());
        CHECK(res.success);
        cached_entry.add_payload(std::string(broclip::mime::kTextHtml), std::move(res.data));
    }

    // 3. PNG transfer
    {
        broclip::PipePair png_pipe;
        REQUIRE(png_pipe.is_valid());
        broclip::write_stream_all(png_pipe.write_fd(), png_content);
        png_pipe.close_write();

        auto res = broclip::read_stream_all(png_pipe.read_fd());
        CHECK(res.success);
        cached_entry.add_payload(std::string(broclip::mime::kImagePng), std::move(res.data));
    }

    CHECK(cached_entry.has_mime(broclip::mime::kTextPlainUtf8));
    CHECK(cached_entry.has_mime(broclip::mime::kTextHtml));
    CHECK(cached_entry.has_mime(broclip::mime::kImagePng));
    CHECK_EQ(cached_entry.payloads.size(), 3u);
    CHECK_EQ(cached_entry.text().value_or(""), text_content);
    CHECK_EQ(cached_entry.get_bytes("image/png")->size(), png_content.size());
}

#if defined(__linux__)
static void test_wayland_data_control_backend() {
    broclip::linux_backend::WaylandDataControlBackend backend;
    CHECK_EQ(std::string(backend.name()), "wayland_data_control");

    // Test send handling when serving preserved entry
    broclip::ClipEntry entry;
    entry.set_text("Preserved selection payload");
    backend.set_selection(broclip::SelectionKind::Clipboard, entry);

    // Simulate compositor requesting MIME transfer via send callback
    broclip::PipePair send_pipe;
    REQUIRE(send_pipe.is_valid());

    int write_fd = send_pipe.release_write();
    bool handled = backend.handle_send_request(broclip::SelectionKind::Clipboard,
                                               std::string(broclip::mime::kTextPlainUtf8),
                                               write_fd);
    CHECK(handled);

    auto read_back = broclip::read_stream_all(send_pipe.read_fd());
    CHECK(read_back.success);
    CHECK_EQ(read_back.as_string(), "Preserved selection payload");

    // Test unavailable gracefully if no compositor
    if (!backend.is_available()) {
        CHECK(!backend.start());
        CHECK(!backend.is_running());
    }
}
#endif

int main() {
    test_multi_mime_caching_simulation();
#if defined(__linux__)
    test_wayland_data_control_backend();
#endif

    return bstest::finish("test_data_control");
}
