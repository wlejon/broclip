#include "broclip/mime_stream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace broclip {

PipePair::PipePair() {
#if defined(__linux__)
    if (::pipe2(fds_, O_CLOEXEC | O_NONBLOCK) != 0) {
        fds_[0] = -1;
        fds_[1] = -1;
    }
#elif defined(_WIN32)
    // On Windows, pipe emulation if needed
    fds_[0] = -1;
    fds_[1] = -1;
#else
    if (::pipe(fds_) == 0) {
        ::fcntl(fds_[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds_[1], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds_[0], F_SETFL, O_NONBLOCK);
        ::fcntl(fds_[1], F_SETFL, O_NONBLOCK);
    } else {
        fds_[0] = -1;
        fds_[1] = -1;
    }
#endif
}

PipePair::~PipePair() {
    close();
}

PipePair::PipePair(PipePair&& other) noexcept {
    fds_[0] = other.fds_[0];
    fds_[1] = other.fds_[1];
    other.fds_[0] = -1;
    other.fds_[1] = -1;
}

PipePair& PipePair::operator=(PipePair&& other) noexcept {
    if (this != &other) {
        close();
        fds_[0] = other.fds_[0];
        fds_[1] = other.fds_[1];
        other.fds_[0] = -1;
        other.fds_[1] = -1;
    }
    return *this;
}

int PipePair::release_read() noexcept {
    int fd = fds_[0];
    fds_[0] = -1;
    return fd;
}

int PipePair::release_write() noexcept {
    int fd = fds_[1];
    fds_[1] = -1;
    return fd;
}

void PipePair::close_read() noexcept {
#if !defined(_WIN32)
    if (fds_[0] >= 0) {
        ::close(fds_[0]);
        fds_[0] = -1;
    }
#endif
}

void PipePair::close_write() noexcept {
#if !defined(_WIN32)
    if (fds_[1] >= 0) {
        ::close(fds_[1]);
        fds_[1] = -1;
    }
#endif
}

void PipePair::close() noexcept {
    close_read();
    close_write();
}

ReadResult read_stream_all(int fd, size_t max_bytes, std::chrono::milliseconds timeout) {
    ReadResult res;
    if (fd < 0) {
        res.error_message = "invalid file descriptor";
        return res;
    }

#if defined(_WIN32)
    res.error_message = "pipes unsupported on windows in this mode";
    return res;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<uint8_t, 8192> chunk{};

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            res.error_message = "read timed out";
            return res;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        int timeout_ms = static_cast<int>(std::max<int64_t>(1, remaining.count()));

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP | POLLERR;

        int pret = ::poll(&pfd, 1, timeout_ms);
        if (pret < 0) {
            if (errno == EINTR) continue;
            res.error_message = std::strerror(errno);
            return res;
        }
        if (pret == 0) {
            res.error_message = "read poll timed out";
            return res;
        }

        ssize_t n = ::read(fd, chunk.data(), chunk.size());
        if (n == 0) {
            // EOF reached
            res.success = true;
            return res;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (pfd.revents & POLLHUP) {
                    res.success = true;
                    return res;
                }
                continue;
            }
            res.error_message = std::strerror(errno);
            return res;
        }

        if (res.data.size() + static_cast<size_t>(n) > max_bytes) {
            res.error_message = "stream exceeded max_bytes limit";
            return res;
        }

        res.data.insert(res.data.end(), chunk.begin(), chunk.begin() + n);
    }
#endif
}

StreamResult write_stream_all(int fd, const void* data, size_t len,
                              std::chrono::milliseconds timeout) {
    StreamResult res;
    if (fd < 0) {
        res.error_message = "invalid file descriptor";
        return res;
    }

#if defined(_WIN32)
    res.error_message = "pipes unsupported on windows in this mode";
    return res;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;

    while (written < len) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            res.error_message = "write timed out";
            res.bytes_transferred = written;
            return res;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        int timeout_ms = static_cast<int>(std::max<int64_t>(1, remaining.count()));

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT | POLLERR | POLLHUP;

        int pret = ::poll(&pfd, 1, timeout_ms);
        if (pret < 0) {
            if (errno == EINTR) continue;
            res.error_message = std::strerror(errno);
            res.bytes_transferred = written;
            return res;
        }
        if (pret == 0) {
            res.error_message = "write poll timed out";
            res.bytes_transferred = written;
            return res;
        }

        if (pfd.revents & (POLLERR | POLLHUP)) {
            res.error_message = "peer closed pipe";
            res.bytes_transferred = written;
            return res;
        }

        ssize_t n = ::write(fd, ptr + written, len - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            res.error_message = std::strerror(errno);
            res.bytes_transferred = written;
            return res;
        } else {
            // n == 0 on write
            res.error_message = "write returned 0";
            res.bytes_transferred = written;
            return res;
        }
    }

    res.success = true;
    res.bytes_transferred = written;
    return res;
#endif
}

bool is_valid_utf8(std::string_view text) noexcept {
    const auto* it = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = it + text.size();

    while (it < end) {
        unsigned char c = *it++;
        if (c <= 0x7F) {
            // 1-byte ASCII
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            // 2-byte sequence
            if (it >= end) return false;
            unsigned char c2 = *it++;
            if ((c2 & 0xC0) != 0x80) return false;
        } else if (c >= 0xE0 && c <= 0xEF) {
            // 3-byte sequence
            if (it + 1 >= end) return false;
            unsigned char c2 = *it++;
            unsigned char c3 = *it++;
            if ((c3 & 0xC0) != 0x80) return false;
            if (c == 0xE0) {
                if (c2 < 0xA0 || c2 > 0xBF) return false;  // Reject overlong
            } else if (c == 0xED) {
                if (c2 < 0x80 || c2 > 0x9F) return false;  // Reject UTF-16 surrogates
            } else {
                if ((c2 & 0xC0) != 0x80) return false;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            // 4-byte sequence
            if (it + 2 >= end) return false;
            unsigned char c2 = *it++;
            unsigned char c3 = *it++;
            unsigned char c4 = *it++;
            if ((c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) return false;
            if (c == 0xF0) {
                if (c2 < 0x90 || c2 > 0xBF) return false;  // Reject overlong
            } else if (c == 0xF4) {
                if (c2 < 0x80 || c2 > 0x8F) return false;  // Reject > U+10FFFF
            } else {
                if ((c2 & 0xC0) != 0x80) return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

std::string sanitize_utf8(std::string_view text, std::string_view replacement) {
    if (is_valid_utf8(text)) {
        return std::string(text);
    }

    std::string out;
    out.reserve(text.size() + replacement.size());

    const auto* it = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = it + text.size();

    while (it < end) {
        const auto* start = it;
        unsigned char c = *it++;

        if (c <= 0x7F) {
            out.push_back(static_cast<char>(c));
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (it < end && (*it & 0xC0) == 0x80) {
                out.append(reinterpret_cast<const char*>(start), 2);
                it++;
            } else {
                out.append(replacement);
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (it + 1 < end) {
                unsigned char c2 = it[0];
                unsigned char c3 = it[1];
                bool ok = ((c3 & 0xC0) == 0x80);
                if (c == 0xE0) ok = ok && (c2 >= 0xA0 && c2 <= 0xBF);
                else if (c == 0xED) ok = ok && (c2 >= 0x80 && c2 <= 0x9F);
                else ok = ok && ((c2 & 0xC0) == 0x80);

                if (ok) {
                    out.append(reinterpret_cast<const char*>(start), 3);
                    it += 2;
                } else {
                    out.append(replacement);
                }
            } else {
                out.append(replacement);
                it = end;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (it + 2 < end) {
                unsigned char c2 = it[0];
                unsigned char c3 = it[1];
                unsigned char c4 = it[2];
                bool ok = ((c3 & 0xC0) == 0x80 && (c4 & 0xC0) == 0x80);
                if (c == 0xF0) ok = ok && (c2 >= 0x90 && c2 <= 0xBF);
                else if (c == 0xF4) ok = ok && (c2 >= 0x80 && c2 <= 0x8F);
                else ok = ok && ((c2 & 0xC0) == 0x80);

                if (ok) {
                    out.append(reinterpret_cast<const char*>(start), 4);
                    it += 3;
                } else {
                    out.append(replacement);
                }
            } else {
                out.append(replacement);
                it = end;
            }
        } else {
            out.append(replacement);
        }
    }

    return out;
}

static constexpr uint8_t kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool is_png(std::span<const uint8_t> data) noexcept {
    if (data.size() < 24) return false;
    if (std::memcmp(data.data(), kPngMagic, 8) != 0) return false;
    // Check first chunk is IHDR
    return (data[12] == 'I' && data[13] == 'H' && data[14] == 'D' && data[15] == 'R');
}

bool is_jpeg(std::span<const uint8_t> data) noexcept {
    if (data.size() < 3) return false;
    return (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF);
}

bool is_bmp(std::span<const uint8_t> data) noexcept {
    if (data.size() < 14) return false;
    return (data[0] == 'B' && data[1] == 'M');
}

std::optional<ImageDimensions> parse_png_dimensions(std::span<const uint8_t> data) noexcept {
    if (!is_png(data) || data.size() < 24) {
        return std::nullopt;
    }
    // IHDR width at offset 16 (big endian), height at offset 20 (big endian)
    uint32_t width = (static_cast<uint32_t>(data[16]) << 24) |
                     (static_cast<uint32_t>(data[17]) << 16) |
                     (static_cast<uint32_t>(data[18]) << 8) |
                     static_cast<uint32_t>(data[19]);
    uint32_t height = (static_cast<uint32_t>(data[20]) << 24) |
                      (static_cast<uint32_t>(data[21]) << 16) |
                      (static_cast<uint32_t>(data[22]) << 8) |
                      static_cast<uint32_t>(data[23]);

    if (width == 0 || height == 0) return std::nullopt;
    return ImageDimensions{width, height};
}

std::string detect_image_mime(std::span<const uint8_t> data) {
    if (is_png(data)) return std::string(mime::kImagePng);
    if (is_jpeg(data)) return std::string(mime::kImageJpeg);
    if (is_bmp(data)) return std::string(mime::kImageBmp);
    if (data.size() >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return "image/webp";
    }
    return std::string(mime::kOctetStream);
}

static void append_codepoint_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return;  // surrogate
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static bool decode_entity(std::string_view ent, std::string& out) {
    if (ent.empty()) return false;
    if (ent == "amp") { out.push_back('&'); return true; }
    if (ent == "lt") { out.push_back('<'); return true; }
    if (ent == "gt") { out.push_back('>'); return true; }
    if (ent == "quot") { out.push_back('"'); return true; }
    if (ent == "apos") { out.push_back('\''); return true; }
    if (ent == "nbsp") { out.push_back(' '); return true; }

    if (ent.front() == '#') {
        auto num_part = ent.substr(1);
        if (num_part.empty()) return false;
        uint32_t cp = 0;
        if (num_part.front() == 'x' || num_part.front() == 'X') {
            auto hex = num_part.substr(1);
            if (hex.empty()) return false;
            for (char c : hex) {
                if (c >= '0' && c <= '9') cp = (cp << 4) | (c - '0');
                else if (c >= 'a' && c <= 'f') cp = (cp << 4) | (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp = (cp << 4) | (c - 'A' + 10);
                else return false;
            }
        } else {
            for (char c : num_part) {
                if (c >= '0' && c <= '9') cp = cp * 10 + (c - '0');
                else return false;
            }
        }
        append_codepoint_utf8(out, cp);
        return true;
    }
    return false;
}

std::string extract_text_from_html(std::string_view html) {
    std::string text;
    text.reserve(html.size());

    size_t i = 0;
    const size_t len = html.size();

    auto case_compare = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t k = 0; k < a.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(a[k])) !=
                std::tolower(static_cast<unsigned char>(b[k]))) {
                return false;
            }
        }
        return true;
    };

    auto skip_tag_content = [&](std::string_view tag_name) {
        std::string close_tag = "</";
        close_tag += tag_name;
        while (i < len) {
            if (html[i] == '<') {
                if (i + close_tag.size() <= len) {
                    if (case_compare(html.substr(i, close_tag.size()), close_tag)) {
                        while (i < len && html[i] != '>') i++;
                        if (i < len && html[i] == '>') i++;
                        return;
                    }
                }
            }
            i++;
        }
    };

    while (i < len) {
        if (html[i] == '<') {
            // Check for comment <!-- ... -->
            if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')) {
                    i++;
                }
                if (i + 2 < len) i += 3;
                continue;
            }

            size_t tag_start = ++i;
            while (i < len && html[i] != '>') i++;
            std::string_view tag_content = html.substr(tag_start, (i < len ? i : len) - tag_start);
            if (i < len) i++;  // Skip '>'

            // Trim leading slash if close tag
            bool is_closing = (!tag_content.empty() && tag_content.front() == '/');
            if (is_closing) tag_content.remove_prefix(1);

            // Extract tag name
            size_t space_pos = tag_content.find_first_of(" \t\r\n/");
            std::string_view tag_name = tag_content.substr(0, space_pos);

            if (case_compare(tag_name, "script")) {
                if (!is_closing) skip_tag_content("script");
            } else if (case_compare(tag_name, "style")) {
                if (!is_closing) skip_tag_content("style");
            } else if (case_compare(tag_name, "head")) {
                if (!is_closing) skip_tag_content("head");
            } else if (case_compare(tag_name, "br")) {
                text.push_back('\n');
            } else if (case_compare(tag_name, "p") || case_compare(tag_name, "div") ||
                       case_compare(tag_name, "tr") || case_compare(tag_name, "li") ||
                       case_compare(tag_name, "h1") || case_compare(tag_name, "h2") ||
                       case_compare(tag_name, "h3") || case_compare(tag_name, "h4") ||
                       case_compare(tag_name, "h5") || case_compare(tag_name, "h6")) {
                if (!text.empty() && text.back() != '\n') {
                    text.push_back('\n');
                }
            }
            continue;
        }

        if (html[i] == '&') {
            size_t ent_start = ++i;
            while (i < len && html[i] != ';' && html[i] != '&' && html[i] != '<' && (i - ent_start) < 12) {
                i++;
            }
            if (i < len && html[i] == ';') {
                std::string_view ent = html.substr(ent_start, i - ent_start);
                i++;  // Skip ';'
                if (!decode_entity(ent, text)) {
                    text.push_back('&');
                    text.append(ent);
                    text.push_back(';');
                }
            } else {
                text.push_back('&');
                // Backtrack to ent_start
                i = ent_start;
            }
            continue;
        }

        // Regular character
        text.push_back(html[i++]);
    }

    // Normalize spacing: collapse repeated spaces/tabs, reduce runs of >= 3 newlines to 2
    std::string clean;
    clean.reserve(text.size());
    int consecutive_newlines = 0;
    bool in_space = false;

    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            in_space = false;
            if (consecutive_newlines < 2) {
                clean.push_back('\n');
                consecutive_newlines++;
            }
        } else if (c == ' ' || c == '\t') {
            if (!in_space && consecutive_newlines == 0 && !clean.empty() && clean.back() != '\n') {
                clean.push_back(' ');
                in_space = true;
            }
        } else {
            in_space = false;
            consecutive_newlines = 0;
            clean.push_back(c);
        }
    }

    // Trim trailing whitespace
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\n')) {
        clean.pop_back();
    }
    return clean;
}

std::optional<std::string> select_best_mime(const std::vector<std::string>& offered,
                                            const std::vector<std::string>& preferred) {
    for (const auto& pref : preferred) {
        for (const auto& off : offered) {
            if (mime::matches(pref, off)) {
                return off;
            }
        }
    }
    return std::nullopt;
}

}  // namespace broclip
