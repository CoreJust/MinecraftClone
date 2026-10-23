#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace client::detail {

class MapImageData final {
public:
    using Pixel = std::array<uint8_t, 4>;
    static constexpr uint32_t MAXIMUM_DIMENSION = 512U;

    [[nodiscard]] bool set(
        uint32_t const width,
        uint32_t const height,
        std::span<Pixel const> const pixels
    )
    {
        if (width == 0U || height == 0U
            || width > MAXIMUM_DIMENSION || height > MAXIMUM_DIMENSION) {
            return false;
        }
        uint64_t const expected_pixel_count = static_cast<uint64_t>(width) * height;
        if (static_cast<uint64_t>(pixels.size()) != expected_pixel_count
            || std::any_of(pixels.begin(), pixels.end(), [](Pixel const& pixel) { return pixel[3] != 255U; })) {
            return false;
        }
        if (m_width == width && m_height == height && m_pixels.size() == pixels.size()
            && std::equal(m_pixels.begin(), m_pixels.end(), pixels.begin())) {
            return true;
        }
        m_pixels.assign(pixels.begin(), pixels.end());
        m_width = width;
        m_height = height;
        advanceRevision();
        return true;
    }

    void clear() noexcept
    {
        if (empty()) {
            return;
        }
        std::vector<Pixel>{}.swap(m_pixels);
        m_width = 0U;
        m_height = 0U;
        advanceRevision();
    }

    [[nodiscard]] bool empty() const noexcept { return m_pixels.empty(); }
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] uint64_t revision() const noexcept { return m_revision; }
    [[nodiscard]] std::span<Pixel const> pixels() const noexcept { return m_pixels; }

private:
    void advanceRevision() noexcept
    {
        ++m_revision;
        if (m_revision == 0U) {
            m_revision = 1U;
        }
    }

    uint32_t m_width = 0U;
    uint32_t m_height = 0U;
    uint64_t m_revision = 0U;
    std::vector<Pixel> m_pixels;
};

} // namespace client::detail
