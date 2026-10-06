#include "check.h"
#include "broclip/types.h"

static void test_types_and_strings() {
    CHECK_EQ(broclip::to_string(broclip::SelectionKind::Clipboard), "clipboard");
    CHECK_EQ(broclip::to_string(broclip::SelectionKind::Primary), "primary");

    CHECK_EQ(broclip::to_string(broclip::ClipSourceKind::LocalApp), "local_app");
    CHECK_EQ(broclip::to_string(broclip::ClipSourceKind::WaylandDataControl), "wayland_data_control");
    CHECK_EQ(broclip::to_string(broclip::ClipSourceKind::Win32), "win32");
    CHECK_EQ(broclip::to_string(broclip::ClipSourceKind::MacPasteboard), "mac_pasteboard");
    CHECK_EQ(broclip::to_string(broclip::ClipSourceKind::Unknown), "unknown");
}

static void test_clip_entry_manipulation() {
    broclip::ClipEntry e;
    e.kind = broclip::SelectionKind::Clipboard;
    e.source.app_name = "broterm";
    e.set_text("First line\nSecond line");

    CHECK(e.has_mime(broclip::mime::kTextPlainUtf8));
    CHECK(e.has_mime("text/plain"));
    CHECK(!e.has_mime("image/png"));

    auto txt = e.text();
    REQUIRE(txt.has_value());
    CHECK_EQ(*txt, "First line\nSecond line");

    auto preview = e.preview(15);
    CHECK(preview.find("First line") != std::string::npos);

    // Multi-payload
    std::vector<uint8_t> png_bytes{0x89, 'P', 'N', 'G'};
    e.add_payload(std::string(broclip::mime::kImagePng), png_bytes);

    CHECK(e.has_mime("image/png"));
    auto got_png = e.get_bytes("image/png");
    REQUIRE(got_png.has_value());
    CHECK_EQ(got_png->size(), 4u);

    // Fingerprint determinism
    std::string fp1 = e.fingerprint();
    std::string fp2 = e.fingerprint();
    CHECK_EQ(fp1, fp2);

    // Different content produces different fingerprint
    broclip::ClipEntry e_other = e;
    e_other.set_text("Different content");
    CHECK(e.fingerprint() != e_other.fingerprint());
}

int main() {
    test_types_and_strings();
    test_clip_entry_manipulation();

    return bstest::finish("test_clip_types");
}
