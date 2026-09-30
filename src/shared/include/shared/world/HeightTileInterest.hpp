#pragma once

#include <shared/net/Message.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace shared {

[[nodiscard]]
constexpr uint64_t heightTileCoordinateHash(int32_t const x, int32_t const y) noexcept
{
    uint64_t hash = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32U)
        ^ static_cast<uint32_t>(y);
    // Mix both axes into low bits for power-of-two unordered-container buckets.
    hash ^= hash >> 30U;
    hash *= 0xbf58'476d'1ce4'e5b9ULL;
    hash ^= hash >> 27U;
    hash *= 0x94d0'49bb'1331'11ebULL;
    return hash ^ (hash >> 31U);
}

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

struct HeightTileInterestOrders final {
    uint32_t radius;
    std::array<HeightTileInterest, 16> orders;
};

enum class HeightTileGenerationBand : uint8_t {
    Immediate,
    Near,
    Directional,
    Background,
};

[[nodiscard]]
std::shared_ptr<HeightTileInterestOrders const> prepareHeightTileInterestOrders(
    uint32_t radius = HEIGHT_TILE_INTEREST_RADIUS
);

[[nodiscard]]
HeightTileInterest makeHeightTileInterest(
    HeightTileKey center,
    int8_t heading_x,
    int8_t heading_y,
    uint32_t radius = HEIGHT_TILE_INTEREST_RADIUS
);

[[nodiscard]]
HeightTileInterest makeHeightTileInterest(
    HeightTileKey center,
    int8_t heading_x,
    int8_t heading_y,
    HeightTileInterestOrders const& orders
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

[[nodiscard]]
std::vector<HeightTileKey> selectHeightTileRemovalCandidates(
    HeightTileKey center,
    int8_t heading_x,
    int8_t heading_y,
    std::span<HeightTileKey const> pending,
    std::span<HeightTileKey const> inflight,
    uint32_t maximum_count
);

} // namespace shared
