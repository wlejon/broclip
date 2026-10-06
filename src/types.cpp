#include "broclip/types.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace broclip {

namespace mime {

static std::string to_lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

static std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string normalize(std::string_view mime) {
    auto trimmed = trim_ascii(mime);
    return to_lower_ascii(trimmed);
}

bool is_text(std::string_view mime) noexcept {
    auto norm = normalize(mime);
    if (norm.rfind("text/", 0) == 0) return true;
    if (norm == "utf8_string" || norm == "string" || norm == "text") return true;
    if (norm.find("charset=") != std::string::npos) return true;
    return false;
}

bool is_image(std::string_view mime) noexcept {
    auto norm = normalize(mime);
    return norm.rfind("image/", 0) == 0;
}

bool is_html(std::string_view mime) noexcept {
    auto norm = normalize(mime);
    return norm == "text/html" || norm.rfind("text/html;", 0) == 0;
}

bool matches(std::string_view requested, std::string_view offered) noexcept {
    auto req = normalize(requested);
    auto off = normalize(offered);

    if (req == off) return true;

    // Equivalences for plain text
    bool req_plain = (req == "text/plain" || req == "text/plain;charset=utf-8" ||
                      req == "utf8_string" || req == "string");
    bool off_plain = (off == "text/plain" || off == "text/plain;charset=utf-8" ||
                      off == "utf8_string" || off == "string");
    if (req_plain && off_plain) return true;

    // Wildcard match e.g. "image/*"
    if (req.ends_with("/*")) {
        auto prefix = req.substr(0, req.size() - 1);
        if (off.rfind(prefix, 0) == 0) return true;
    }

    return false;
}

}  // namespace mime

bool ClipEntry::has_mime(std::string_view mime) const noexcept {
    return find_payload(mime) != nullptr;
}

const ClipPayload* ClipEntry::find_payload(std::string_view mime) const noexcept {
    for (const auto& p : payloads) {
        if (mime::matches(mime, p.mime)) {
            return &p;
        }
    }
    return nullptr;
}

std::optional<std::string> ClipEntry::text() const {
    static constexpr std::string_view kTextMimes[] = {
        mime::kTextPlainUtf8,
        mime::kTextPlain,
        mime::kUtf8String,
        mime::kString,
    };
    for (auto m : kTextMimes) {
        if (const auto* p = find_payload(m)) {
            return p->as_string();
        }
    }
    // Check any text payload
    for (const auto& p : payloads) {
        if (mime::is_text(p.mime)) {
            return p.as_string();
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> ClipEntry::get_bytes(std::string_view mime) const {
    if (const auto* p = find_payload(mime)) {
        return p->data;
    }
    return std::nullopt;
}

void ClipEntry::set_text(std::string_view text, std::string_view mime) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    add_payload(std::string(mime), std::move(bytes));
}

void ClipEntry::add_payload(std::string mime_type, std::vector<uint8_t> data) {
    for (auto& p : payloads) {
        if (mime::matches(p.mime, mime_type)) {
            p.mime = std::move(mime_type);
            p.data = std::move(data);
            return;
        }
    }
    payloads.push_back(ClipPayload{std::move(mime_type), std::move(data)});
}

size_t ClipEntry::size_bytes() const noexcept {
    size_t total = sizeof(ClipEntry);
    for (const auto& p : payloads) {
        total += sizeof(ClipPayload) + p.mime.capacity() + p.data.capacity();
    }
    for (const auto& t : tags) {
        total += t.capacity();
    }
    total += source.app_name.capacity() + source.window_title.capacity();
    return total;
}

std::string ClipEntry::preview(size_t max_len) const {
    if (auto txt = text()) {
        std::string s;
        s.reserve(std::min(txt->size(), max_len));
        for (char c : *txt) {
            if (s.size() >= max_len) break;
            if (c == '\r' || c == '\n' || c == '\t') {
                if (!s.empty() && s.back() != ' ') {
                    s.push_back(' ');
                }
            } else if (static_cast<unsigned char>(c) >= 32) {
                s.push_back(c);
            }
        }
        if (txt->size() > max_len) {
            s += "...";
        }
        return s;
    }

    if (!payloads.empty()) {
        std::ostringstream ss;
        ss << "[" << payloads.front().mime << ": " << payloads.front().data.size() << " bytes]";
        return ss.str();
    }

    return "[empty]";
}

std::string ClipEntry::fingerprint() const {
    // 64-bit FNV-1a hash over kind and sorted payloads
    uint64_t hash = 14695981039346656037ULL;
    auto add_byte = [&hash](uint8_t b) {
        hash ^= b;
        hash *= 1099511628211ULL;
    };
    auto add_str = [&add_byte](std::string_view s) {
        for (char c : s) add_byte(static_cast<uint8_t>(c));
    };

    add_byte(static_cast<uint8_t>(kind));

    // Sort payload references by normalized mime for stable fingerprint
    std::vector<const ClipPayload*> sorted;
    sorted.reserve(payloads.size());
    for (const auto& p : payloads) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(), [](const ClipPayload* a, const ClipPayload* b) {
        return a->mime < b->mime;
    });

    for (const auto* p : sorted) {
        add_str(p->mime);
        add_byte(0xFF);
        for (uint8_t b : p->data) {
            add_byte(b);
        }
        add_byte(0xFE);
    }

    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

}  // namespace broclip
