#include <client/detail/ScreenshotDibV5.hpp>
#include <client/detail/ScreenshotPng.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
extern "C" bool mcCopyPngToMacClipboard(uint8_t const* bytes, size_t size, char const* pasteboard_name) noexcept;
#endif

namespace client::detail {
namespace {

#if defined(_WIN32)
class ClipboardGlobalMemory final {
public:
    explicit ClipboardGlobalMemory(HGLOBAL const handle) noexcept
        : m_handle(handle)
    { }

    ~ClipboardGlobalMemory()
    {
        if (m_handle != nullptr) {
            static_cast<void>(GlobalFree(m_handle));
        }
    }

    ClipboardGlobalMemory(ClipboardGlobalMemory const&) = delete;
    ClipboardGlobalMemory& operator=(ClipboardGlobalMemory const&) = delete;

    [[nodiscard]] HGLOBAL get() const noexcept { return m_handle; }

    void release() noexcept { m_handle = nullptr; }

private:
    HGLOBAL m_handle;
};

class ClipboardOpenGuard final {
public:
    explicit ClipboardOpenGuard(HWND const owner_window) noexcept
        : m_opened(OpenClipboard(owner_window) != FALSE)
    { }

    ~ClipboardOpenGuard()
    {
        if (m_opened) {
            static_cast<void>(CloseClipboard());
        }
    }

    ClipboardOpenGuard(ClipboardOpenGuard const&) = delete;
    ClipboardOpenGuard& operator=(ClipboardOpenGuard const&) = delete;

    [[nodiscard]] bool opened() const noexcept { return m_opened; }

    [[nodiscard]] bool close() noexcept
    {
        if (!m_opened) {
            return true;
        }
        if (CloseClipboard() == FALSE) {
            return false;
        }
        m_opened = false;
        return true;
    }

private:
    bool m_opened;
};

[[nodiscard]] std::string windowsClipboardError(std::string_view const operation, DWORD const error_code)
{
    return std::string(operation) + " (Win32 error " + std::to_string(error_code) + ")";
}

[[nodiscard]] bool copyDibV5ToWindowsClipboard(
    std::span<uint8_t const> const dib,
    HWND const owner_window,
    std::string& error
)
{
    static_assert(sizeof(BITMAPV5HEADER) == 124U);
    static_assert(CF_DIBV5 == 17U);

    ClipboardGlobalMemory memory{ GlobalAlloc(GMEM_MOVEABLE, dib.size()) };
    if (memory.get() == nullptr) {
        error = windowsClipboardError("allocating Windows clipboard image memory failed", GetLastError());
        return false;
    }
    void* const locked_memory = GlobalLock(memory.get());
    if (locked_memory == nullptr) {
        error = windowsClipboardError("locking Windows clipboard image memory failed", GetLastError());
        return false;
    }
    std::memcpy(locked_memory, dib.data(), dib.size());
    static_cast<void>(GlobalUnlock(memory.get()));

    ClipboardOpenGuard clipboard{ owner_window };
    if (!clipboard.opened()) {
        error = windowsClipboardError("opening the Windows clipboard failed", GetLastError());
        return false;
    }
    if (EmptyClipboard() == FALSE) {
        error = windowsClipboardError("clearing the Windows clipboard failed", GetLastError());
        return false;
    }
    if (SetClipboardData(CF_DIBV5, memory.get()) == nullptr) {
        error = windowsClipboardError("setting the Windows clipboard image failed", GetLastError());
        return false;
    }
    memory.release();
    if (!clipboard.close()) {
        error = windowsClipboardError("closing the Windows clipboard failed", GetLastError());
        return false;
    }
    error.clear();
    return true;
}
#endif

} // namespace

bool copyScreenshotPngToClipboard(
    std::span<uint8_t const> const png,
    std::string& error,
    std::optional<std::string_view> const pasteboard_name
)
{
    if (png.empty()) {
        error = "PNG image is empty";
        return false;
    }
    if (pasteboard_name && pasteboard_name->empty()) {
        error = "pasteboard name is empty";
        return false;
    }
#if defined(__APPLE__)
    std::string name_storage;
    char const* name = nullptr;
    if (pasteboard_name) {
        name_storage.assign(pasteboard_name->data(), pasteboard_name->size());
        name = name_storage.c_str();
    }
    if (!mcCopyPngToMacClipboard(png.data(), png.size(), name)) {
        error = "macOS clipboard rejected the PNG image";
        return false;
    }
    error.clear();
    return true;
#else
    error = "image clipboard screenshots are supported on macOS only";
    return false;
#endif
}

bool copyScreenshotRgba8ToClipboard(
    uint32_t const width,
    uint32_t const height,
    std::span<uint8_t const> const rgba8,
    std::string& error,
    void* const native_clipboard_owner,
    std::optional<std::string_view> const pasteboard_name
)
{
    if (pasteboard_name && pasteboard_name->empty()) {
        error = "pasteboard name is empty";
        return false;
    }
#if defined(__APPLE__)
    static_cast<void>(native_clipboard_owner);
    std::optional<std::vector<uint8_t>> const png = encodeRgba8Png(width, height, rgba8, error);
    if (!png.has_value()) {
        return false;
    }
    return copyScreenshotPngToClipboard(*png, error, pasteboard_name);
#elif defined(_WIN32)
    if (native_clipboard_owner == nullptr) {
        error = "game window handle is unavailable for Windows clipboard ownership";
        return false;
    }
    if (pasteboard_name.has_value()) {
        error = "named screenshot pasteboards are supported only on macOS";
        return false;
    }
    std::optional<std::vector<uint8_t>> const dib = encodeRgba8DibV5(width, height, rgba8, error);
    if (!dib.has_value()) {
        return false;
    }
    return copyDibV5ToWindowsClipboard(*dib, static_cast<HWND>(native_clipboard_owner), error);
#else
    static_cast<void>(width);
    static_cast<void>(height);
    static_cast<void>(rgba8);
    static_cast<void>(native_clipboard_owner);
    error = "image clipboard screenshots are supported on macOS and Windows only";
    return false;
#endif
}

} // namespace client::detail
