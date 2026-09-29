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

enum class HeightTileSurfaceDetail : uint8_t {
    Coarse,
    Coarse2,
    Coarse4,
    Fine,
    Distant,
};

struct HeightTileSurfaceProjection final {
    double vertical_fov_degrees = 70.0;
    uint32_t viewport_width_pixels = 1'440U;
    uint32_t viewport_height_pixels = 900U;

    constexpr bool operator==(HeightTileSurfaceProjection const&) const noexcept = default;
};

struct HeightTileSurfaceBounds final {
    double min_x_blocks = 0.0;
    double max_x_blocks = 0.0;
    double min_y_blocks = 0.0;
    double max_y_blocks = 0.0;
    double min_z_blocks = 0.0;
    double max_z_blocks = 0.0;

    constexpr bool operator==(HeightTileSurfaceBounds const&) const noexcept = default;
};

class HeightTileSurfaceLodPolicy final {
public:
    explicit HeightTileSurfaceLodPolicy(HeightTileSurfaceProjection projection = {}) noexcept;

    [[nodiscard]]
    HeightTileSurfaceDetail detailFor(
        HeightTileSurfaceBounds viewer_bounds,
        HeightTileSurfaceBounds surface_bounds,
        std::optional<HeightTileSurfaceDetail> current_detail = std::nullopt
    ) const noexcept;

private:
    double m_first_reduction_distance = 0.0;
};

struct HeightTileSurfaceMesh final {
    static constexpr uint32_t MAXIMUM_QUAD_COUNT = HeightTile::SAMPLE_COUNT * 4U;

    HeightTileCoordinate coordinate{};
    std::vector<HeightTileSurfaceQuad> quads;
    HeightTileSurfaceDetail detail = HeightTileSurfaceDetail::Fine;
};

struct HeightTileSurfaceSeamBridge final {
    std::vector<HeightTileSurfaceQuad> first;
    std::vector<HeightTileSurfaceQuad> second;
};

[[nodiscard]] HeightTileSurfaceSeamBridge heightTileSurfaceSeamBridge(
    HeightTileSurfaceMesh const& first,
    HeightTileSurfaceMesh const& second,
    HeightTileSurfaceDirection first_edge
);

struct HeightTileSurfaceNeighbors final {
    std::optional<HeightTile::Heights> negative_x;
    std::optional<HeightTile::Heights> positive_x;
    std::optional<HeightTile::Heights> negative_y;
    std::optional<HeightTile::Heights> positive_y;
    HeightTileSurfaceDetail negative_x_detail = HeightTileSurfaceDetail::Fine;
    HeightTileSurfaceDetail positive_x_detail = HeightTileSurfaceDetail::Fine;
    HeightTileSurfaceDetail negative_y_detail = HeightTileSurfaceDetail::Fine;
    HeightTileSurfaceDetail positive_y_detail = HeightTileSurfaceDetail::Fine;
};

class HeightTileSurfaceMesher final {
public:
    [[nodiscard]]
    static HeightTileSurfaceDetail detailFor(
        HeightTileSurfaceBounds viewer_bounds,
        HeightTileSurfaceBounds surface_bounds,
        HeightTileSurfaceProjection projection = {}
    ) noexcept;

    [[nodiscard]]
    static HeightTileSurfaceBounds boundsForTile(
        HeightTileCoordinate player_tile,
        HeightTileCoordinate tile,
        double min_z_blocks,
        double max_z_blocks
    ) noexcept;

    [[nodiscard]]
    static HeightTileSurfaceBounds boundsForTile(
        HeightTileCoordinate player_tile,
        HeightTile const& tile,
        HeightTileSurfaceNeighbors const& neighbors
    ) noexcept;

    [[nodiscard]]
    HeightTileSurfaceMesh build(
        HeightTile const& tile,
        HeightTileSurfaceNeighbors const& neighbors = {},
        HeightTileSurfaceDetail detail = HeightTileSurfaceDetail::Fine
    ) const;
};

} // namespace shared
