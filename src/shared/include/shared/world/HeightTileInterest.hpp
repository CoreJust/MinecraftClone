#pragma once

#include <shared/net/Message.hpp>

#include <cstdint>
#include <vector>

namespace shared {

struct HeightTileHeading final {
    int8_t x = 0;
    int8_t y = 127;

    constexpr bool operator==(HeightTileHeading const&) const noexcept = default;
};

struct HeightTileInterest final {
    std::vector<HeightTileKey> keys;
    int8_t heading_x = 0;
    int8_t heading_y = 127;
};

enum class HeightTileGenerationBand : uint8_t {
    Immediate,
    Near,
    Directional,
    Background,
};

[[nodiscard]]
HeightTileInterest makeHeightTileInterest(
    HeightTileKey center,
    int8_t heading_x,
    int8_t heading_y
);

[[nodiscard]] HeightTileHeading canonicalHeightTileHeading(int8_t x, int8_t y) noexcept;

[[nodiscard]] double heightTileInterestPriority(
    HeightTileKey center,
    int8_t heading_x,
    int8_t heading_y,
    HeightTileKey key
) noexcept;

[[nodiscard]] HeightTileGenerationBand heightTileGenerationBand(
    HeightTileKey center,
    int8_t heading_x,
    int8_t heading_y,
    HeightTileKey key
) noexcept;

} // namespace shared
