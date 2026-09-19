#pragma once

#include <client/PreviewResidency.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <array>
#include <optional>

namespace client {

struct PreviewMeshSource final {
    HeightTileHandle tile;
    std::array<HeightTileHandle, 4> neighbors;
};

[[nodiscard]] inline shared::HeightTileSurfaceMesh buildPreviewMesh(PreviewMeshSource const& source)
{
    auto const heights = [](HeightTileHandle const& tile) -> std::optional<shared::HeightTile::Heights> {
        return tile ? std::optional<shared::HeightTile::Heights>{tile->heights()} : std::nullopt;
    };
    shared::HeightTileSurfaceNeighbors const neighbors{
        .negative_x = heights(source.neighbors[0]),
        .positive_x = heights(source.neighbors[1]),
        .negative_y = heights(source.neighbors[2]),
        .positive_y = heights(source.neighbors[3]),
    };
    shared::HeightTileSurfaceMesher mesher;
    return mesher.build({
        .coordinate = {.x = source.tile->key().x, .y = source.tile->key().y},
        .heights = source.tile->heights(),
    }, neighbors);
}

} // namespace client
