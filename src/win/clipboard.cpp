#include "broclip/win/clipboard.h"

#include "broclip/types.h"

#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace broclip::win {

static const wchar_t* kWindowClassName = L"BroclipClipboardListenerWindow";

static std::string utf16_to_utf8(const wchar_t* wstr, int len = -1) {
    if (!wstr || len == 0) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return {};
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, str.data(), size_needed, nullptr, nullptr);
    if (len < 0 && !str.empty() && str.back() == '\0') {
        str.pop_back();
    }
    return str;
}

static std::wstring utf8_to_utf16(std::string_view str) {
    if (str.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (size_needed <= 0) return {};
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), wstr.data(), size_needed);
    return wstr;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    auto* backend = reinterpret_cast<Win32ClipboardBackend*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_CLIPBOARDUPDATE) {
        if (backend) backend->handle_clipboard_update();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void Win32ClipboardBackend::handle_clipboard_update() {
    ClipEntry entry;
    {
        std::lock_guard lock(mutex_);
        entry = read_clipboard_locked();
    }
    if (entry.payloads.empty()) return;
    SelectionChangeHandler cb;
    {
        std::lock_guard lock(mutex_);
        cb = on_selection_change_;
    }
    if (cb) cb(std::move(entry));
}

Win32ClipboardBackend::Win32ClipboardBackend() = default;

Win32ClipboardBackend::~Win32ClipboardBackend() {
    stop();
}

bool Win32ClipboardBackend::is_available() const {
    return true;
}

bool Win32ClipboardBackend::start() {
    if (running_.load()) return true;

    running_.store(true);
    worker_thread_ = std::thread([this]() { message_loop(); });
    return true;
}

void Win32ClipboardBackend::stop() {
    if (!running_.exchange(false)) return;

    if (hwnd_) {
        PostMessage(static_cast<HWND>(hwnd_), WM_QUIT, 0, 0);
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool Win32ClipboardBackend::is_running() const {
    return running_.load();
}

void Win32ClipboardBackend::set_on_selection_change(SelectionChangeHandler handler) {
    std::lock_guard lock(mutex_);
    on_selection_change_ = std::move(handler);
}

void Win32ClipboardBackend::set_on_selection_clear(SelectionClearHandler handler) {
    std::lock_guard lock(mutex_);
    on_selection_clear_ = std::move(handler);
}

ClipEntry Win32ClipboardBackend::read_clipboard_locked() {
    ClipEntry entry;
    entry.kind = SelectionKind::Clipboard;
    entry.source.kind = ClipSourceKind::Win32;
    entry.timestamp = std::chrono::system_clock::now();

    if (!OpenClipboard(static_cast<HWND>(hwnd_))) {
        return entry;
    }

    // Read CF_UNICODETEXT
    HANDLE hText = GetClipboardData(CF_UNICODETEXT);
    if (hText) {
        const auto* wstr = static_cast<const wchar_t*>(GlobalLock(hText));
        if (wstr) {
            std::string utf8 = utf16_to_utf8(wstr);
            GlobalUnlock(hText);
            if (!utf8.empty()) {
                entry.set_text(utf8);
            }
        }
    }

    // Read HTML format if present
    UINT html_format = RegisterClipboardFormatA("HTML Format");
    if (html_format != 0) {
        HANDLE hHtml = GetClipboardData(html_format);
        if (hHtml) {
            const auto* hdata = static_cast<const char*>(GlobalLock(hHtml));
            if (hdata) {
                size_t len = std::strlen(hdata);
                std::vector<uint8_t> bytes(hdata, hdata + len);
                entry.add_payload(std::string(mime::kTextHtml), std::move(bytes));
                GlobalUnlock(hHtml);
            }
        }
    }

    // Read PNG format if present
    UINT png_format = RegisterClipboardFormatA("PNG");
    if (png_format != 0) {
        HANDLE hPng = GetClipboardData(png_format);
        if (hPng) {
            size_t size = GlobalSize(hPng);
            const auto* pdata = static_cast<const uint8_t*>(GlobalLock(hPng));
            if (pdata && size > 0) {
                std::vector<uint8_t> bytes(pdata, pdata + size);
                entry.add_payload(std::string(mime::kImagePng), std::move(bytes));
                GlobalUnlock(hPng);
            }
        }
    }

    CloseClipboard();
    return entry;
}

bool Win32ClipboardBackend::set_selection(SelectionKind kind, const ClipEntry& entry) {
    if (kind != SelectionKind::Clipboard) {
        // Windows only has one clipboard (no primary selection)
        return false;
    }

    std::lock_guard lock(mutex_);
    if (!OpenClipboard(static_cast<HWND>(hwnd_))) {
        return false;
    }

    EmptyClipboard();

    if (auto txt = entry.text()) {
        std::wstring wstr = utf8_to_utf16(*txt);
        size_t byte_len = (wstr.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byte_len);
        if (hMem) {
            void* dst = GlobalLock(hMem);
            if (dst) {
                std::memcpy(dst, wstr.c_str(), byte_len);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            } else {
                GlobalFree(hMem);
            }
        }
    }

    if (const auto* png = entry.find_payload(mime::kImagePng)) {
        UINT png_format = RegisterClipboardFormatA("PNG");
        if (png_format != 0 && !png->data.empty()) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, png->data.size());
            if (hMem) {
                void* dst = GlobalLock(hMem);
                if (dst) {
                    std::memcpy(dst, png->data.data(), png->data.size());
                    GlobalUnlock(hMem);
                    SetClipboardData(png_format, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
        }
    }

    CloseClipboard();
    return true;
}

bool Win32ClipboardBackend::clear_selection(SelectionKind kind) {
    if (kind != SelectionKind::Clipboard) return false;
    std::lock_guard lock(mutex_);
    if (!OpenClipboard(static_cast<HWND>(hwnd_))) return false;
    EmptyClipboard();
    CloseClipboard();
    return true;
}

void Win32ClipboardBackend::message_loop() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kWindowClassName;

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"BroclipListener", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), this);

    hwnd_ = hwnd;
    if (!hwnd) {
        running_.store(false);
        return;
    }

    AddClipboardFormatListener(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    RemoveClipboardFormatListener(hwnd);
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClassName, GetModuleHandle(nullptr));
    hwnd_ = nullptr;
}

}  // namespace broclip::win

#else

namespace broclip::win {

Win32ClipboardBackend::Win32ClipboardBackend() = default;
Win32ClipboardBackend::~Win32ClipboardBackend() = default;
bool Win32ClipboardBackend::is_available() const { return false; }
bool Win32ClipboardBackend::start() { return false; }
void Win32ClipboardBackend::stop() {}
bool Win32ClipboardBackend::is_running() const { return false; }
bool Win32ClipboardBackend::set_selection(SelectionKind, const ClipEntry&) { return false; }
bool Win32ClipboardBackend::clear_selection(SelectionKind) { return false; }
void Win32ClipboardBackend::set_on_selection_change(SelectionChangeHandler) {}
void Win32ClipboardBackend::set_on_selection_clear(SelectionClearHandler) {}

}  // namespace broclip::win

#endif
