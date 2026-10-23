#include <client/detail/ScreenshotPng.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if defined(__APPLE__)
extern "C" bool mcCopyPngToMacClipboard(uint8_t const* bytes, size_t size, char const* pasteboard_name) noexcept;
#endif

namespace client::detail {

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

} // namespace client::detail
