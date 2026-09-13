#include <acceptance/ImageEvidence.hpp>

#include <fstream>
#include <limits>

namespace acceptance {

namespace {

bool isValidExtent(uint32_t const width, uint32_t const height, std::span<uint8_t const> const rgba8)
{
    uint64_t const pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    return width != 0U && height != 0U
        && pixel_count <= std::numeric_limits<uint64_t>::max() / 4U
        && rgba8.size() == pixel_count * 4U;
}

} // namespace

std::expected<void, std::string> writeRgba8Ppm(
    std::filesystem::path const& path,
    uint32_t const width,
    uint32_t const height,
    std::span<uint8_t const> const rgba8
)
{
    if (!isValidExtent(width, height, rgba8)) {
        return std::unexpected("frame capture does not match its declared extent");
    }
    uint64_t const pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);

    std::ofstream output{ path, std::ios::binary | std::ios::trunc };
    if (!output) {
        return std::unexpected("cannot open frame evidence output: " + path.string());
    }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
        uint64_t const offset = pixel * 4U;
        output.put(static_cast<char>(rgba8[offset]));
        output.put(static_cast<char>(rgba8[offset + 1U]));
        output.put(static_cast<char>(rgba8[offset + 2U]));
    }
    if (!output) {
        return std::unexpected("cannot write frame evidence output: " + path.string());
    }
    return { };
}

std::expected<void, std::string> validateGameplayFrameCapture(
    uint32_t const width,
    uint32_t const height,
    std::span<uint8_t const> const rgba8,
    bool const
)
{
    if (!isValidExtent(width, height, rgba8)) {
        return std::unexpected("frame capture does not match its declared extent");
    }
    bool has_background{ false };
    bool has_grid{ false };
    bool has_red_player{ false };
    bool has_green_player{ false };
    for (uint64_t offset{ 0 }; offset < static_cast<uint64_t>(rgba8.size()); offset += 4U) {
        uint8_t const red = rgba8[offset];
        uint8_t const green = rgba8[offset + 1U];
        uint8_t const blue = rgba8[offset + 2U];
        has_background = has_background || (
            blue > green + 20U && green > red + 20U
        );
        has_grid = has_grid || (
            red < 80U && green < 85U && blue < 100U
            && green >= red && blue >= green
        );
        has_red_player = has_red_player || (
            red > green * 2U + 30U && red > blue * 2U + 30U
        );
        has_green_player = has_green_player || (
            green > red * 2U + 30U && green > blue * 2U + 30U
        );
    }
    if (!has_background || !has_grid || !has_red_player || !has_green_player) {
        return std::unexpected(
            "frame capture is missing required background, grid, or expected player-region pixels"
        );
    }
    return { };
}

} // namespace acceptance
