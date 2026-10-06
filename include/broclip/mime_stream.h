#pragma once

#include "broclip/types.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace broclip {

struct StreamResult {
    size_t bytes_transferred = 0;
    bool success = false;
    std::string error_message;
};

struct ReadResult {
    std::vector<uint8_t> data;
    bool success = false;
    std::string error_message;

    std::string_view as_string_view() const noexcept {
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
    std::string as_string() const {
        return std::string(as_string_view());
    }
};

class PipePair {
public:
    PipePair();
    ~PipePair();

    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;

    PipePair(PipePair&& other) noexcept;
    PipePair& operator=(PipePair&& other) noexcept;

    bool is_valid() const noexcept { return fds_[0] >= 0 && fds_[1] >= 0; }

    int read_fd() const noexcept { return fds_[0]; }
    int write_fd() const noexcept { return fds_[1]; }

    int release_read() noexcept;
    int release_write() noexcept;

    void close_read() noexcept;
    void close_write() noexcept;
    void close() noexcept;

private:
    int fds_[2]{-1, -1};
};

ReadResult read_stream_all(int fd, size_t max_bytes = 64 * 1024 * 1024,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

StreamResult write_stream_all(int fd, const void* data, size_t len,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

inline StreamResult write_stream_all(int fd, std::span<const uint8_t> data,
                                     std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    return write_stream_all(fd, data.data(), data.size(), timeout);
}

inline StreamResult write_stream_all(int fd, std::string_view text,
                                     std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    return write_stream_all(fd, text.data(), text.size(), timeout);
}

// UTF-8 Validation and Sanitization
bool is_valid_utf8(std::string_view text) noexcept;
std::string sanitize_utf8(std::string_view text, std::string_view replacement = "\xEF\xBF\xBD");

// Image Inspection
struct ImageDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
};

bool is_png(std::span<const uint8_t> data) noexcept;
bool is_jpeg(std::span<const uint8_t> data) noexcept;
bool is_bmp(std::span<const uint8_t> data) noexcept;
std::optional<ImageDimensions> parse_png_dimensions(std::span<const uint8_t> data) noexcept;
std::string detect_image_mime(std::span<const uint8_t> data);

// HTML Plaintext Extraction
std::string extract_text_from_html(std::string_view html);

// MIME Negotiation
std::optional<std::string> select_best_mime(const std::vector<std::string>& offered,
                                            const std::vector<std::string>& preferred);

}  // namespace broclip
