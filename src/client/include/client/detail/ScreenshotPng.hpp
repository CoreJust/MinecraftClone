#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace client::detail {

[[nodiscard]] std::optional<std::vector<uint8_t>> encodeRgba8Png(
    uint32_t width,
    uint32_t height,
    std::span<uint8_t const> rgba8,
    std::string& error
);

[[nodiscard]] bool copyScreenshotPngToClipboard(
    std::span<uint8_t const> png,
    std::string& error,
    std::optional<std::string_view> pasteboard_name = std::nullopt
);

#if defined(__APPLE__)
[[nodiscard]] std::optional<std::vector<uint8_t>> takeScreenshotPngFromPasteboard(
    std::string_view pasteboard_name,
    std::string& error
);
#endif

} // namespace client::detail
