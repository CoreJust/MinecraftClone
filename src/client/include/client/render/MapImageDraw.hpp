#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace client {

inline constexpr uint32_t MAXIMUM_MAP_IMAGES = 2U;
inline constexpr uint32_t MAXIMUM_MAP_DRAWS = 8U;

// Coordinates share the GUI's physical framebuffer space. Source crop is normalized.
// A zero-sized scissor means the whole framebuffer.
struct MapImageDraw final {
    uint32_t image = 0U;
    std::array<float, 4> source{ 0.0F, 0.0F, 1.0F, 1.0F };
    std::array<float, 4> destination{};
    std::array<uint32_t, 4> scissor{};
};

[[nodiscard]] inline bool validMapImageDraw(MapImageDraw const& draw) noexcept
{
    if (draw.image >= MAXIMUM_MAP_IMAGES
        || !std::ranges::all_of(draw.source, [](float value) { return std::isfinite(value); })
        || !std::ranges::all_of(draw.destination, [](float value) { return std::isfinite(value); })) {
        return false;
    }
    if (draw.source[0] < 0.0F || draw.source[1] < 0.0F
        || draw.source[2] <= 0.0F || draw.source[3] <= 0.0F
        || draw.source[0] + draw.source[2] > 1.0F || draw.source[1] + draw.source[3] > 1.0F
        || draw.destination[2] <= 0.0F || draw.destination[3] <= 0.0F) {
        return false;
    }
    if ((draw.scissor[2] == 0U) != (draw.scissor[3] == 0U)) {
        return false;
    }
    return static_cast<uint64_t>(draw.scissor[0]) + draw.scissor[2] <= std::numeric_limits<int32_t>::max()
        && static_cast<uint64_t>(draw.scissor[1]) + draw.scissor[3] <= std::numeric_limits<int32_t>::max();
}

} // namespace client
