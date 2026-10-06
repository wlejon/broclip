#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace broclip {

enum class SelectionKind : uint8_t {
    Clipboard = 0,
    Primary = 1,
};

inline std::string_view to_string(SelectionKind kind) noexcept {
    switch (kind) {
    case SelectionKind::Clipboard:
        return "clipboard";
    case SelectionKind::Primary:
        return "primary";
    }
    return "unknown";
}

enum class ClipSourceKind : uint8_t {
    Unknown = 0,
    LocalApp,
    WaylandDataControl,
    X11,
    Win32,
    MacPasteboard,
    Portal,
    Internal,
};

inline std::string_view to_string(ClipSourceKind kind) noexcept {
    switch (kind) {
    case ClipSourceKind::Unknown:
        return "unknown";
    case ClipSourceKind::LocalApp:
        return "local_app";
    case ClipSourceKind::WaylandDataControl:
        return "wayland_data_control";
    case ClipSourceKind::X11:
        return "x11";
    case ClipSourceKind::Win32:
        return "win32";
    case ClipSourceKind::MacPasteboard:
        return "mac_pasteboard";
    case ClipSourceKind::Portal:
        return "portal";
    case ClipSourceKind::Internal:
        return "internal";
    }
    return "unknown";
}

using MimeType = std::string;

namespace mime {
inline constexpr std::string_view kTextPlain = "text/plain";
inline constexpr std::string_view kTextPlainUtf8 = "text/plain;charset=utf-8";
inline constexpr std::string_view kUtf8String = "UTF8_STRING";
inline constexpr std::string_view kString = "STRING";
inline constexpr std::string_view kTextHtml = "text/html";
inline constexpr std::string_view kImagePng = "image/png";
inline constexpr std::string_view kImageJpeg = "image/jpeg";
inline constexpr std::string_view kImageBmp = "image/bmp";
inline constexpr std::string_view kUriList = "text/uri-list";
inline constexpr std::string_view kOctetStream = "application/octet-stream";

bool is_text(std::string_view mime) noexcept;
bool is_image(std::string_view mime) noexcept;
bool is_html(std::string_view mime) noexcept;
std::string normalize(std::string_view mime);
bool matches(std::string_view requested, std::string_view offered) noexcept;
}  // namespace mime

struct ClipSource {
    ClipSourceKind kind = ClipSourceKind::Unknown;
    std::string app_name;
    std::string window_title;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

struct ClipPayload {
    MimeType mime;
    std::vector<uint8_t> data;

    std::string_view as_string_view() const noexcept {
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }

    std::string as_string() const {
        return std::string(as_string_view());
    }

    size_t size() const noexcept {
        return data.size();
    }

    bool empty() const noexcept {
        return data.empty();
    }
};

struct ClipEntry {
    uint64_t id = 0;
    SelectionKind kind = SelectionKind::Clipboard;
    ClipSource source;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::vector<ClipPayload> payloads;
    bool pinned = false;
    std::vector<std::string> tags;

    bool has_mime(std::string_view mime) const noexcept;
    const ClipPayload* find_payload(std::string_view mime) const noexcept;
    std::optional<std::string> text() const;
    std::optional<std::vector<uint8_t>> get_bytes(std::string_view mime) const;

    void set_text(std::string_view text, std::string_view mime = mime::kTextPlainUtf8);
    void add_payload(std::string mime, std::vector<uint8_t> data);

    size_t size_bytes() const noexcept;
    std::string preview(size_t max_len = 80) const;
    std::string fingerprint() const;
};

struct HistoryQuery {
    std::optional<std::string> filter_text;
    std::optional<std::string> mime_filter;
    std::optional<SelectionKind> selection_kind;
    std::optional<bool> pinned_only;
    std::optional<std::chrono::system_clock::time_point> since;
    size_t max_results = 50;
    size_t offset = 0;
};

}  // namespace broclip
