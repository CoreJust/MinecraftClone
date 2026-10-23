#include <client/detail/ScreenshotPng.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace client::detail {
namespace {

constexpr std::array<uint8_t, 8U> PNG_SIGNATURE{ 137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U };
constexpr uint64_t DEFLATE_BLOCK_MAXIMUM = 65'535U;
constexpr uint32_t ADLER_MODULUS = 65'521U;

void appendBigEndian32(std::vector<uint8_t>& output, uint32_t const value)
{
    output.push_back(static_cast<uint8_t>(value >> 24U));
    output.push_back(static_cast<uint8_t>(value >> 16U));
    output.push_back(static_cast<uint8_t>(value >> 8U));
    output.push_back(static_cast<uint8_t>(value));
}

void appendLittleEndian16(std::vector<uint8_t>& output, uint16_t const value)
{
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8U));
}

uint32_t crc32(std::span<uint8_t const> bytes) noexcept
{
    uint32_t crc = 0xFFFF'FFFFU;
    for (uint8_t const byte : bytes) {
        crc ^= byte;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            uint32_t const mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB8'8320U & mask);
        }
    }
    return ~crc;
}

void appendChunk(std::vector<uint8_t>& output, std::array<uint8_t, 4U> const type,
    std::span<uint8_t const> const data)
{
    appendBigEndian32(output, static_cast<uint32_t>(data.size()));
    uint64_t const checksum_start = output.size();
    output.insert(output.end(), type.begin(), type.end());
    output.insert(output.end(), data.begin(), data.end());
    appendBigEndian32(output, crc32(std::span<uint8_t const>{ output }.subspan(checksum_start)));
}

uint32_t adler32(std::span<uint8_t const> bytes) noexcept
{
    uint32_t first = 1U;
    uint32_t second = 0U;
    for (uint8_t const byte : bytes) {
        first = (first + byte) % ADLER_MODULUS;
        second = (second + first) % ADLER_MODULUS;
    }
    return (second << 16U) | first;
}

std::vector<uint8_t> encodeStoredZlib(std::span<uint8_t const> bytes)
{
    uint64_t const byte_count = bytes.size();
    uint64_t const block_count = (byte_count + DEFLATE_BLOCK_MAXIMUM - 1U) / DEFLATE_BLOCK_MAXIMUM;
    std::vector<uint8_t> output;
    output.reserve(2U + bytes.size() + block_count * 5U + 4U);
    output.push_back(0x78U);
    output.push_back(0x01U);
    for (uint64_t offset = 0U; offset < byte_count;) {
        uint64_t const block_size = std::min(DEFLATE_BLOCK_MAXIMUM, byte_count - offset);
        bool const final_block = offset + block_size == byte_count;
        output.push_back(final_block ? 0x01U : 0x00U);
        uint16_t const length = static_cast<uint16_t>(block_size);
        appendLittleEndian16(output, length);
        appendLittleEndian16(output, static_cast<uint16_t>(~length));
        std::span<uint8_t const> const block = bytes.subspan(offset, block_size);
        output.insert(output.end(), block.begin(), block.end());
        offset += block_size;
    }
    appendBigEndian32(output, adler32(bytes));
    return output;
}

} // namespace

std::optional<std::vector<uint8_t>> encodeRgba8Png(
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
    uint64_t const maximum = std::numeric_limits<decltype(rgba8.size())>::max();
    uint64_t const width_64 = width;
    uint64_t const height_64 = height;
    if (width_64 > (maximum - 1U) / 4U) {
        error = "image row size overflows addressable storage";
        return std::nullopt;
    }
    uint64_t const row_bytes = width_64 * 4U;
    uint64_t const filtered_row_bytes = row_bytes + 1U;
    if (height_64 > maximum / filtered_row_bytes) {
        error = "image size overflows addressable storage";
        return std::nullopt;
    }
    uint64_t const expected_bytes = filtered_row_bytes * height_64;
    uint64_t const maximum_chunk_bytes = std::numeric_limits<uint32_t>::max();
    uint64_t const expected_bytes_64 = expected_bytes;
    if (expected_bytes_64 > maximum_chunk_bytes - 6U) {
        error = "image is too large for one PNG data chunk";
        return std::nullopt;
    }
    uint64_t const deflate_block_count = (expected_bytes_64 + DEFLATE_BLOCK_MAXIMUM - 1U)
        / DEFLATE_BLOCK_MAXIMUM;
    if (expected_bytes_64 > maximum_chunk_bytes - 6U - deflate_block_count * 5U) {
        error = "image is too large for one PNG data chunk";
        return std::nullopt;
    }
    if (rgba8.size() != row_bytes * height_64) {
        error = "RGBA storage does not match image dimensions";
        return std::nullopt;
    }

    std::vector<uint8_t> scanlines;
    scanlines.reserve(expected_bytes);
    for (uint32_t y = 0U; y < height; ++y) {
        scanlines.push_back(0U);
        uint64_t const row_offset = static_cast<uint64_t>(y) * row_bytes;
        std::span<uint8_t const> const row = rgba8.subspan(row_offset, row_bytes);
        scanlines.insert(scanlines.end(), row.begin(), row.end());
    }
    std::vector<uint8_t> const compressed = encodeStoredZlib(scanlines);

    std::vector<uint8_t> png;
    png.reserve(PNG_SIGNATURE.size() + 25U + compressed.size() + 12U);
    png.insert(png.end(), PNG_SIGNATURE.begin(), PNG_SIGNATURE.end());
    std::array<uint8_t, 13U> header{};
    header[0] = static_cast<uint8_t>(width >> 24U);
    header[1] = static_cast<uint8_t>(width >> 16U);
    header[2] = static_cast<uint8_t>(width >> 8U);
    header[3] = static_cast<uint8_t>(width);
    header[4] = static_cast<uint8_t>(height >> 24U);
    header[5] = static_cast<uint8_t>(height >> 16U);
    header[6] = static_cast<uint8_t>(height >> 8U);
    header[7] = static_cast<uint8_t>(height);
    header[8] = 8U;
    header[9] = 6U;
    appendChunk(png, { 'I', 'H', 'D', 'R' }, header);
    appendChunk(png, { 'I', 'D', 'A', 'T' }, compressed);
    appendChunk(png, { 'I', 'E', 'N', 'D' }, {});
    return png;
}

} // namespace client::detail
