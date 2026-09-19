#pragma once

#include <shared/world/WorldGeneration.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace shared {

enum class HeightTileSurfaceDirection : uint8_t {
    NegativeX,
    PositiveX,
    NegativeY,
    PositiveY,
    PositiveZ = 5,
};

struct HeightTileSurfaceQuad final {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    HeightTileSurfaceDirection direction = HeightTileSurfaceDirection::PositiveZ;
    uint16_t u_extent = 1U;
    uint16_t v_extent = 1U;

    constexpr bool operator==(HeightTileSurfaceQuad const&) const noexcept = default;
};

struct HeightTileSurfaceMesh final {
    static constexpr uint32_t MAXIMUM_QUAD_COUNT = HeightTile::SAMPLE_COUNT * 4U;

    HeightTileCoordinate coordinate{};
    std::vector<HeightTileSurfaceQuad> quads;
};

struct HeightTileSurfaceNeighbors final {
    std::optional<HeightTile::Heights> negative_x;
    std::optional<HeightTile::Heights> positive_x;
    std::optional<HeightTile::Heights> negative_y;
    std::optional<HeightTile::Heights> positive_y;
};

class HeightTileSurfaceMesher final {
public:
    [[nodiscard]]
    HeightTileSurfaceMesh build(
        HeightTile const& tile,
        HeightTileSurfaceNeighbors const& neighbors = {}
    ) const;
};

} // namespace shared
