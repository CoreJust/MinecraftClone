#include <client/detail/ScreenshotDibV5.hpp>

#include <limits>
#include <span>
#include <string>
#include <vector>

namespace client::detail {
namespace {

constexpr uint32_t DIB_V5_HEADER_SIZE = 124U;
constexpr uint32_t DIB_V5_BI_BITFIELDS = 3U;
constexpr uint32_t DIB_V5_LCS_SRGB = 0x7352'4742U;

void appendLittleEndian16(std::vector<uint8_t>& bytes, uint16_t const value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendLittleEndian32(std::vector<uint8_t>& bytes, uint32_t const value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
    bytes.push_back(static_cast<uint8_t>(value >> 16U));
    bytes.push_back(static_cast<uint8_t>(value >> 24U));
}

} // namespace

std::optional<std::vector<uint8_t>> encodeRgba8DibV5(
    uint32_t const width,
    uint32_t const height,
    std::span<uint8_t const> const rgba8,
    std::string& error
)
{
    if (width == 0U || height == 0U) {
        error = "image dimensions must be nonzero";
        return std::nullopt;
    }
    constexpr uint32_t DIB_DIMENSION_MAXIMUM = 0x7FFF'FFFFU;
    if (width > DIB_DIMENSION_MAXIMUM || height > DIB_DIMENSION_MAXIMUM) {
        error = "image dimensions exceed DIBV5 limits";
        return std::nullopt;
    }

    uint64_t const addressable_size = std::numeric_limits<decltype(rgba8.size())>::max();
    uint64_t const width_64 = width;
    uint64_t const height_64 = height;
    uint64_t const row_bytes = width_64 * 4U;
    uint64_t const maximum_image_bytes = std::numeric_limits<uint32_t>::max();
    if (row_bytes > addressable_size || row_bytes > maximum_image_bytes) {
        error = "image row size exceeds DIBV5 limits";
        return std::nullopt;
    }
    if (height_64 > addressable_size / row_bytes || height_64 > maximum_image_bytes / row_bytes) {
        error = "image size exceeds DIBV5 limits";
        return std::nullopt;
    }
    uint64_t const pixel_bytes = row_bytes * height_64;
    if (pixel_bytes > addressable_size - DIB_V5_HEADER_SIZE) {
        error = "image size overflows addressable storage";
        return std::nullopt;
    }
    if (rgba8.size() != pixel_bytes) {
        error = "RGBA storage does not match image dimensions";
        return std::nullopt;
    }

    std::vector<uint8_t> dib;
    dib.reserve(static_cast<decltype(dib.size())>(DIB_V5_HEADER_SIZE + pixel_bytes));
    appendLittleEndian32(dib, DIB_V5_HEADER_SIZE);
    appendLittleEndian32(dib, width);
    appendLittleEndian32(dib, height);
    appendLittleEndian16(dib, 1U);
    appendLittleEndian16(dib, 32U);
    appendLittleEndian32(dib, DIB_V5_BI_BITFIELDS);
    appendLittleEndian32(dib, static_cast<uint32_t>(pixel_bytes));
    appendLittleEndian32(dib, 0U);
    appendLittleEndian32(dib, 0U);
    appendLittleEndian32(dib, 0U);
    appendLittleEndian32(dib, 0U);
    appendLittleEndian32(dib, 0x00FF'0000U);
    appendLittleEndian32(dib, 0x0000'FF00U);
    appendLittleEndian32(dib, 0x0000'00FFU);
    appendLittleEndian32(dib, 0xFF00'0000U);
    appendLittleEndian32(dib, DIB_V5_LCS_SRGB);
    for (uint32_t field = 0U; field < 12U; ++field) {
        appendLittleEndian32(dib, 0U);
    }
    appendLittleEndian32(dib, 4U);
    appendLittleEndian32(dib, 0U);
    appendLittleEndian32(dib, 0U);
    appendLittleEndian32(dib, 0U);

    for (uint32_t y = height; y > 0U; --y) {
        uint64_t const row_offset = (static_cast<uint64_t>(y) - 1U) * row_bytes;
        for (uint32_t x = 0U; x < width; ++x) {
            uint64_t const pixel_offset = row_offset + static_cast<uint64_t>(x) * 4U;
            dib.push_back(rgba8[pixel_offset + 2U]);
            dib.push_back(rgba8[pixel_offset + 1U]);
            dib.push_back(rgba8[pixel_offset]);
            dib.push_back(rgba8[pixel_offset + 3U]);
        }
    }
    error.clear();
    return dib;
}

} // namespace client::detail
