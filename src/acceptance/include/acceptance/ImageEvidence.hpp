#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace acceptance {

[[nodiscard]]
std::expected<void, std::string> writeRgba8Ppm(
    std::filesystem::path const& path,
    uint32_t width,
    uint32_t height,
    std::span<uint8_t const> rgba8
);

[[nodiscard]]
std::expected<void, std::string> validateGameplayFrameCapture(
    uint32_t width,
    uint32_t height,
    std::span<uint8_t const> rgba8,
    bool srgb_encoded
);

} // namespace acceptance
