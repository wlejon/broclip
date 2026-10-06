#include "check.h"
#include "broclip/mime_stream.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

static void test_pipe_stream_transfer() {
    broclip::PipePair pipe;
    REQUIRE(pipe.is_valid());

    const std::string msg = "Hello from streaming pipe!";
    auto write_res = broclip::write_stream_all(pipe.write_fd(), msg);
    CHECK(write_res.success);
    CHECK_EQ(write_res.bytes_transferred, msg.size());

    pipe.close_write();

    auto read_res = broclip::read_stream_all(pipe.read_fd());
    CHECK(read_res.success);
    CHECK_EQ(read_res.as_string(), msg);
}

static void test_pipe_stream_large_payload() {
    broclip::PipePair pipe;
    REQUIRE(pipe.is_valid());

    // 256 KB binary transfer
    const size_t kSize = 256 * 1024;
    std::vector<uint8_t> send_data(kSize);
    for (size_t i = 0; i < kSize; ++i) {
        send_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::thread writer([&pipe, &send_data]() {
        broclip::write_stream_all(pipe.write_fd(), send_data);
        pipe.close_write();
    });

    auto read_res = broclip::read_stream_all(pipe.read_fd());
    writer.join();

    CHECK(read_res.success);
    CHECK_EQ(read_res.data.size(), kSize);
    CHECK_EQ(std::memcmp(read_res.data.data(), send_data.data(), kSize), 0);
}

static void test_utf8_validation() {
    // 1. ASCII
    CHECK(broclip::is_valid_utf8("Hello, World!"));
    CHECK(broclip::is_valid_utf8(""));

    // 2. Valid multi-byte
    CHECK(broclip::is_valid_utf8("Привет мир"));            // Russian 2-byte
    CHECK(broclip::is_valid_utf8("こんにちは世界"));          // Japanese 3-byte
    CHECK(broclip::is_valid_utf8("🦀 Rust / C++ 🚀"));       // 4-byte emoji

    // 3. Invalid single continuation byte
    CHECK(!broclip::is_valid_utf8("\x80"));
    CHECK(!broclip::is_valid_utf8("\xBF"));

    // 4. Invalid overlong sequences
    CHECK(!broclip::is_valid_utf8("\xC0\xAF"));
    CHECK(!broclip::is_valid_utf8("\xC1\xBF"));
    CHECK(!broclip::is_valid_utf8(std::string_view("\xE0\x80\xAF", 3)));
    CHECK(!broclip::is_valid_utf8(std::string_view("\xF0\x80\x80\xAF", 4)));

    // 5. UTF-16 surrogate halves (0xD800 - 0xDFFF)
    CHECK(!broclip::is_valid_utf8(std::string_view("\xED\xA0\x80", 3)));  // U+D800
    CHECK(!broclip::is_valid_utf8(std::string_view("\xED\xBF\xBF", 3)));  // U+DFFF

    // 6. Beyond U+10FFFF
    CHECK(!broclip::is_valid_utf8(std::string_view("\xF4\x90\x80\x80", 4))); // U+110000
    CHECK(!broclip::is_valid_utf8(std::string_view("\xF5\x80\x80\x80", 4)));

    // 7. Sanitization
    std::string bad_str = "Clean\x80" "Text\xC0\xAF" "End";
    std::string clean = broclip::sanitize_utf8(bad_str);
    CHECK(broclip::is_valid_utf8(clean));
    CHECK(clean.find("Clean") != std::string::npos);
    CHECK(clean.find("Text") != std::string::npos);
    CHECK(clean.find("End") != std::string::npos);
}

static void test_image_png_detection() {
    // Valid PNG signature + IHDR chunk with dimensions 800 x 600
    std::vector<uint8_t> valid_png = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,  // Magic
        0x00, 0x00, 0x00, 0x0D,                       // Chunk length (13)
        'I', 'H', 'D', 'R',                           // Chunk type
        0x00, 0x00, 0x03, 0x20,                       // Width = 800 (0x320)
        0x00, 0x00, 0x02, 0x58,                       // Height = 600 (0x258)
        0x08, 0x02, 0x00, 0x00, 0x00,                 // bit depth, color type, etc.
        0x4B, 0x8C, 0x69, 0x12                        // CRC
    };

    CHECK(broclip::is_png(valid_png));
    CHECK(!broclip::is_jpeg(valid_png));
    CHECK(!broclip::is_bmp(valid_png));
    CHECK_EQ(broclip::detect_image_mime(valid_png), std::string(broclip::mime::kImagePng));

    auto dims = broclip::parse_png_dimensions(valid_png);
    REQUIRE(dims.has_value());
    CHECK_EQ(dims->width, 800u);
    CHECK_EQ(dims->height, 600u);

    // Corrupt signature
    std::vector<uint8_t> corrupt_png = valid_png;
    corrupt_png[0] = 0x00;
    CHECK(!broclip::is_png(corrupt_png));
    CHECK(!broclip::parse_png_dimensions(corrupt_png).has_value());

    // JPEG detection
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F'};
    CHECK(broclip::is_jpeg(jpeg));
    CHECK(!broclip::is_png(jpeg));
    CHECK_EQ(broclip::detect_image_mime(jpeg), std::string(broclip::mime::kImageJpeg));

    // BMP detection
    std::vector<uint8_t> bmp(20, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    CHECK(broclip::is_bmp(bmp));
    CHECK_EQ(broclip::detect_image_mime(bmp), std::string(broclip::mime::kImageBmp));
}

static void test_html_extraction() {
    std::string html =
        "<!DOCTYPE html><html><head><title>Test</title><style>.hidden { display: none; }</style></head>"
        "<body>"
        "<script>console.log('secret');</script>"
        "<h1>Title Header</h1>"
        "<p>Hello &amp; welcome to &quot;broclip&quot;!<br>New line here.</p>"
        "<div>Item 1 &lt;special&gt; &apos;quotes&apos; &#65; &#x42;</div>"
        "</body></html>";

    std::string text = broclip::extract_text_from_html(html);

    // Ensure script and style were stripped
    CHECK(text.find("console.log") == std::string::npos);
    CHECK(text.find(".hidden") == std::string::npos);
    CHECK(text.find("<script>") == std::string::npos);
    CHECK(text.find("</style>") == std::string::npos);

    // Ensure entities were decoded
    CHECK(text.find("Hello & welcome to \"broclip\"!") != std::string::npos);
    CHECK(text.find("Item 1 <special> 'quotes' A B") != std::string::npos);
    CHECK(text.find("Title Header") != std::string::npos);
    CHECK(text.find("New line here.") != std::string::npos);
}

static void test_mime_negotiation() {
    CHECK(broclip::mime::is_text("text/plain"));
    CHECK(broclip::mime::is_text("TEXT/PLAIN; charset=utf-8"));
    CHECK(broclip::mime::is_text("UTF8_STRING"));
    CHECK(!broclip::mime::is_text("image/png"));

    CHECK(broclip::mime::is_image("image/png"));
    CHECK(broclip::mime::is_image("IMAGE/JPEG"));
    CHECK(!broclip::mime::is_image("text/html"));

    CHECK(broclip::mime::is_html("text/html"));
    CHECK(broclip::mime::is_html("text/html;charset=utf-8"));

    // Synonym matches
    CHECK(broclip::mime::matches("text/plain", "text/plain;charset=utf-8"));
    CHECK(broclip::mime::matches("text/plain", "UTF8_STRING"));
    CHECK(broclip::mime::matches("image/*", "image/png"));
    CHECK(broclip::mime::matches("image/*", "image/jpeg"));
    CHECK(!broclip::mime::matches("image/*", "text/plain"));

    // Best MIME selection
    std::vector<std::string> offered = {
        "text/html",
        "image/png",
        "text/plain;charset=utf-8"
    };

    auto best1 = broclip::select_best_mime(offered, {"image/png", "text/plain"});
    REQUIRE(best1.has_value());
    CHECK_EQ(*best1, "image/png");

    auto best2 = broclip::select_best_mime(offered, {"text/plain", "text/html"});
    REQUIRE(best2.has_value());
    CHECK_EQ(*best2, "text/plain;charset=utf-8");

    auto best3 = broclip::select_best_mime(offered, {"application/pdf"});
    CHECK(!best3.has_value());
}

int main() {
    test_pipe_stream_transfer();
    test_pipe_stream_large_payload();
    test_utf8_validation();
    test_image_png_detection();
    test_html_extraction();
    test_mime_negotiation();

    return bstest::finish("test_mime_stream");
}
