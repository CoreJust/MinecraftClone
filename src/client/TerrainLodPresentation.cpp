#include <client/TerrainLodPresentation.hpp>

#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace client {

namespace {

static constexpr uint32_t PERIOD = generation::WORLD_SIZE_BLOCKS;

struct Cell final {
    TerrainLodRegion region;
    uint32_t extent;
    int32_t height;
    shared::Block material;
    std::array<int32_t, 4> corner_heights{};
    std::array<int32_t, 4> corner_temperatures{};
};

shared::BlockCoordinate neighborFaceCoordinate(uint32_t const edge, uint32_t const u, uint32_t const v)
{
    shared::BlockCoordinate local{};
    if (edge < 2U) {
        local = {
            .x = static_cast<uint8_t>(edge == 0U ? 15U : 0U),
            .y = static_cast<uint8_t>(u),
            .z = static_cast<uint8_t>(v)};
    } else if (edge < 4U) {
        local = {
            .x = static_cast<uint8_t>(u),
            .y = static_cast<uint8_t>(edge == 2U ? 15U : 0U),
            .z = static_cast<uint8_t>(v)};
    } else {
        local = {
            .x = static_cast<uint8_t>(u),
            .y = static_cast<uint8_t>(v),
            .z = static_cast<uint8_t>(edge == 4U ? 15U : 0U)};
    }
    return local;
}

uint64_t cellKey(TerrainLodRegion const region)
{
    return (static_cast<uint64_t>(region.level) << 48U) | (static_cast<uint64_t>(region.x) << 24U) | region.y;
}

uint32_t wrap(int64_t const value)
{
    return static_cast<uint32_t>((value % PERIOD + PERIOD) % PERIOD);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double distanceToCell(uint32_t const origin, uint32_t const extent, uint32_t const center)
{
    double const midpoint = static_cast<double>(origin) + static_cast<double>(extent) * 0.5;
    double distance = std::abs(midpoint - center);
    distance = std::min(distance, static_cast<double>(PERIOD) - distance);
    return std::max(0.0, distance - static_cast<double>(extent) * 0.5);
}

void collectCells(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& artifact,
    TerrainLodRegion const region,
    uint32_t const root_extent,
    uint32_t const center_x,
    uint32_t const center_y,
    std::vector<Cell>& cells,
    std::stop_token const& cancellation)
{
    if (cancellation.stop_requested()) {
        throw std::runtime_error("terrain LOD scene cancelled");
    }
    uint32_t const extent = terrainLodRegionExtent(region.level, root_extent);
    uint32_t const origin_x = region.x * extent;
    uint32_t const origin_y = region.y * extent;
    double const distance =
        std::max(distanceToCell(origin_x, extent, center_x), distanceToCell(origin_y, extent, center_y));
    double const threshold = region.level == shared::TerrainLodLevel::L0
        ? 1'024.0
        : static_cast<double>(shared::ACTUAL_TERRAIN_RADIUS_BLOCKS + 32U);
    if (region.level < shared::TerrainLodLevel::L2 && distance < threshold) {
        auto const next_level = static_cast<shared::TerrainLodLevel>(static_cast<uint32_t>(region.level) + 1U);
        auto const child_artifact =
            shared::buildTerrainLod(world, next_level, origin_x, origin_y, extent, cancellation);
        for (TerrainLodRegion const child : terrainLodChildren(region, root_extent)) {
            collectCells(world, *child_artifact, child, root_extent, center_x, center_y, cells, cancellation);
        }
        return;
    }
    shared::TerrainLodSample const sample =
        shared::sampleTerrainLod(artifact, origin_x + extent * 0.5, origin_y + extent * 0.5);
    Cell cell{
        .region = region,
        .extent = extent,
        .height = static_cast<int32_t>(std::clamp(std::floor(sample.height), 0.0, 1'023.0)) + 1,
        .material = sample.top_material,
    };
    for (uint32_t corner = 0U; corner < 4U; ++corner) {
        uint32_t const corner_x = corner % 2U;
        uint32_t const corner_y = corner / 2U;
        auto const node = shared::sampleTerrainLod(
            artifact,
            static_cast<double>(origin_x + corner_x * extent),
            static_cast<double>(origin_y + corner_y * extent)
        );
        cell.corner_heights[corner] =
            static_cast<int32_t>(std::lround(std::clamp(node.height + 1.0, 0.0, 1'024.0) * 32.0));
        cell.corner_temperatures[corner] = static_cast<int32_t>(std::lround(node.temperature * 32.0));
    }
    cells.push_back(cell);
}

std::vector<StoneFaceInstance> meshAuthorityProfiles(
    TerrainLodColumnInput const& column,
    std::stop_token const& cancellation
)
{
    std::vector<StoneFaceInstance> faces;
    faces.reserve(512U);
    auto const emit =
        [&](
            uint32_t x,
            uint32_t y,
            uint32_t z,
            uint32_t direction,
            shared::Block material,
            uint32_t height = 1U,
            uint32_t width = 1U
        ) {
            faces.push_back({
                .x = static_cast<int32_t>(column.x * 16U + x),
                .y = static_cast<int32_t>(column.y * 16U + y),
                .z = static_cast<int32_t>(z),
                .direction = direction,
                .u_extent = width,
                .v_extent = height,
                .material_id = static_cast<uint32_t>(material),
            });
        };
    bool const merged_bottom = std::ranges::all_of(column.authority_profiles, [](auto const& profile) {
        return profile.dense_section_mask == 0U;
    });
    if (merged_bottom) {
        emit(0U, 0U, 0U, 4U, shared::Block::Bedrock, 16U, 16U);
    }
    static constexpr std::array<int32_t, 4> OFFSETS{-1, 1, -18, 18};
    for (uint32_t y = 0U; y < 16U; ++y) {
        for (uint32_t x = 0U; x < 16U; ++x) {
            if (cancellation.stop_requested()) {
                throw std::runtime_error("authoritative profile meshing cancelled");
            }
            uint32_t const index = (y + 1U) * 18U + x + 1U;
            auto const& own = column.authority_profiles[index];
            uint64_t scan_mask = own.dense_section_mask;
            for (int32_t const offset : OFFSETS) {
                scan_mask |= column.authority_profiles[static_cast<uint32_t>(static_cast<int32_t>(index) + offset)]
                            .dense_section_mask;
            }
            scan_mask |= (scan_mask << 1U) | (scan_mask >> 1U);
            uint64_t remaining = scan_mask;
            while (remaining != 0U) {
                uint32_t const section = static_cast<uint32_t>(std::countr_zero(remaining));
                remaining &= remaining - 1U;
                for (uint32_t z = section * 16U; z < (section + 1U) * 16U; ++z) {
                    auto const material = own.materialAt(static_cast<uint16_t>(z));
                    if (!shared::isSolidBlock(material)) {
                        continue;
                    }
                    if (z == 0U || !shared::isSolidBlock(own.materialAt(static_cast<uint16_t>(z - 1U)))) {
                        emit(x, y, static_cast<uint16_t>(z), 4U, material);
                    }
                    if (z == 1'023U || !shared::isSolidBlock(own.materialAt(static_cast<uint16_t>(z + 1U)))) {
                        emit(x, y, static_cast<uint16_t>(z), 5U, material);
                    }
                    for (uint32_t direction = 0U; direction < 4U; ++direction) {
                        auto const& neighbor = column.authority_profiles[static_cast<uint32_t>(
                            static_cast<int32_t>(index) + OFFSETS[direction])];
                        if (!shared::isSolidBlock(neighbor.materialAt(static_cast<uint16_t>(z)))) {
                            emit(x, y, static_cast<uint16_t>(z), direction, material);
                        }
                    }
                }
            }
            if ((scan_mask & (uint64_t{1} << (own.base_highest_solid_z / 16U))) == 0U) {
                emit(x, y, own.base_highest_solid_z, 5U, own.materialAt(own.base_highest_solid_z));
            }
            if (!merged_bottom && (scan_mask & 1U) == 0U) {
                emit(x, y, 0U, 4U, own.materialAt(0U));
            }
            for (uint32_t direction = 0U; direction < 4U; ++direction) {
                auto const& neighbor =
                    column.authority_profiles[static_cast<uint32_t>(static_cast<int32_t>(index) + OFFSETS[direction])];
                uint32_t const first = neighbor.base_highest_solid_z + 1U;
                uint32_t const end = own.base_highest_solid_z + 1U;
                if (first >= end) {
                    continue;
                }
                std::array<uint32_t, 5> cuts{
                    first,
                    end,
                    std::clamp<uint32_t>(own.ground_first_z, first, end),
                    std::clamp<uint32_t>(own.ground_end_z, first, end),
                    std::clamp<uint32_t>(own.base_highest_solid_z, first, end),
                };
                std::sort(cuts.begin(), cuts.end());
                for (uint32_t cut = 1U; cut < cuts.size(); ++cut) {
                    if (cuts[cut] != cuts[cut - 1U]) {
                        uint32_t cursor = cuts[cut - 1U];
                        while (cursor < cuts[cut]) {
                            uint32_t const end_run =
                            scan_mask == 0U ? cuts[cut] : std::min(cuts[cut], (cursor / 16U + 1U) * 16U);
                            if ((scan_mask & (uint64_t{1} << (cursor / 16U))) == 0U) {
                            emit(
                            x,
                            y,
                            cursor,
                            direction,
                            own.materialAt(static_cast<uint16_t>(cursor)),
                            end_run - cursor);
                            }
                            cursor = end_run;
                        }
                    }
                }
            }
        }
    }
    return faces;
}

} // namespace

std::optional<TerrainLodColumnInput>
captureTerrainLodColumn(shared::SparseWorld const& world, uint32_t const x, uint32_t const y)
{
    if (x >= PERIOD / 16U || y >= PERIOD / 16U) {
        return std::nullopt;
    }
    TerrainLodColumnInput result{
        .x = x,
        .y = y,
        .chunks = {},
        .sources = {},
        .authority_profiles = {},
        .authority_sources = {},
        .edge_heights = {},
    };
    if (world.authoritativeColumn(static_cast<int64_t>(x) * 16, static_cast<int64_t>(y) * 16)) {
        result.authority_profiles.resize(static_cast<uint64_t>(18U) * 18U);
        result.authority_sources.reserve(5U);
        for (auto const offset :
             {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
            shared::ChunkCoordinate const coordinate{
                .x = static_cast<int32_t>(wrap(
                    static_cast<int64_t>(x) * 16 + static_cast<int64_t>(offset[0]) * 16
                ) / 16U),
                .y = static_cast<int32_t>(wrap(
                    static_cast<int64_t>(y) * 16 + static_cast<int64_t>(offset[1]) * 16
                ) / 16U),
                .z = 0,
            };
            auto const identity = world.authoritativeTileIdentity(coordinate);
            if (!identity) {
                return std::nullopt;
            }
            result.authority_sources.push_back(
                {coordinate, identity->identity, identity->revision, identity->content_hash});
        }
        for (uint32_t row = 0U; row < 18U; ++row) {
            for (uint32_t col = 0U; col < 18U; ++col) {
                if ((row == 0U || row == 17U) && (col == 0U || col == 17U)) {
                    continue;
                }
                uint32_t const wx = wrap(static_cast<int64_t>(x * 16U + col) - 1);
                uint32_t const wy = wrap(static_cast<int64_t>(y * 16U + row) - 1);
                auto profile = world.authoritativeColumn(wx, wy);
                if (!profile) {
                    return std::nullopt;
                }
                result.authority_profiles[row * 18U + col] = std::move(*profile);
            }
        }
        for (uint32_t along = 0U; along < 16U; ++along) {
            result.edge_heights[0][along] = result.authority_profiles[(along + 1U) * 18U + 1U].highest_solid_z + 1U;
            result.edge_heights[1][along] = result.authority_profiles[(along + 1U) * 18U + 16U].highest_solid_z + 1U;
            result.edge_heights[2][along] = result.authority_profiles[18U + along + 1U].highest_solid_z + 1U;
            result.edge_heights[3][along] = result.authority_profiles[16U * 18U + along + 1U].highest_solid_z + 1U;
        }
        return result;
    }
    std::array<bool, 64> surface_chunks{};
    for (uint32_t local_y = 0U; local_y < 16U; ++local_y) {
        for (uint32_t local_x = 0U; local_x < 16U; ++local_x) {
            auto const surface = world.verifiedSurfaceColumn(x * 16U + local_x, y * 16U + local_y);
            if (!surface) {
                return std::nullopt;
            }
            surface_chunks[surface->highest_solid_z / 16U] = true;
            uint16_t const height = static_cast<uint16_t>(surface->highest_solid_z + 1U);
            if (local_x == 0U) {
                result.edge_heights[0U][local_y] = height;
            }
            if (local_x == 15U) {
                result.edge_heights[1U][local_y] = height;
            }
            if (local_y == 0U) {
                result.edge_heights[2U][local_x] = height;
            }
            if (local_y == 15U) {
                result.edge_heights[3U][local_x] = height;
            }
        }
    }
    auto const first_surface = std::find(surface_chunks.begin(), surface_chunks.end(), true);
    auto const last_surface = std::find(surface_chunks.rbegin(), surface_chunks.rend(), true);
    uint32_t const first_z = static_cast<uint32_t>(first_surface - surface_chunks.begin());
    uint32_t const end_z = 64U - static_cast<uint32_t>(last_surface - surface_chunks.rbegin());
    for (uint32_t z = first_z; z < end_z; ++z) {
        surface_chunks[z] = true;
    }
    for (uint32_t z = 0U; z < 64U; ++z) {
        shared::ChunkCoordinate const coordinate{
            .x = static_cast<int32_t>(x),
            .y = static_cast<int32_t>(y),
            .z = static_cast<int32_t>(z),
        };
        shared::Chunk const* const chunk = world.residentChunk(coordinate);
        if (chunk == nullptr) {
            if (surface_chunks[z]) {
                return std::nullopt;
            }
            continue;
        }
        shared::ChunkNeighborFaces neighbors;
        std::vector<TerrainLodChunkSource> sources;
        bool complete = true;
        for (uint32_t edge = 0U; edge < 6U; ++edge) {
            auto const normal = shared::faceNormal(static_cast<shared::FaceDirection>(edge));
            shared::ChunkCoordinate const adjacent{
                .x = static_cast<int32_t>((static_cast<int64_t>(x) + normal[0] + PERIOD / 16U) % (PERIOD / 16U)),
                .y = static_cast<int32_t>((static_cast<int64_t>(y) + normal[1] + PERIOD / 16U) % (PERIOD / 16U)),
                .z = coordinate.z + normal[2],
            };
            std::array<shared::Block, 256> blocks;
            blocks.fill(shared::Block::Air);
            if (adjacent.z >= 0 && adjacent.z < 64) {
                shared::Chunk const* const neighbor = world.residentChunk(adjacent);
                if (neighbor == nullptr) {
                    if (surface_chunks[z]) {
                        complete = false;
                        break;
                    }
                    // Unknown neighbors of lower resident chunks expose conservative
                    // closing faces.
                    neighbors.faces[edge] = blocks;
                    continue;
                }
                sources.push_back({.coordinate = adjacent, .identity = neighbor->contentIdentity()});
                for (uint32_t v = 0U; v < 16U; ++v) {
                    for (uint32_t u = 0U; u < 16U; ++u) {
                        shared::BlockCoordinate const local = neighborFaceCoordinate(edge, u, v);
                        auto const block = neighbor->blockAt(local);
                        if (!block.has_value()) {
                            return std::nullopt;
                        }
                        blocks[v * 16U + u] = block.value();
                    }
                }
            }
            neighbors.faces[edge] = blocks;
        }
        if (!complete) {
            return std::nullopt;
        }
        result.sources.push_back({.coordinate = coordinate, .identity = chunk->contentIdentity()});
        result.sources.insert(result.sources.end(), sources.begin(), sources.end());
        result.chunks.push_back({.chunk = *chunk, .neighbors = neighbors});
    }
    return result;
}

bool terrainLodColumnSourcesCurrent(TerrainLodColumnInput const& column, shared::SparseWorld const& world)
{
    for (auto const& source : column.sources) {
        auto const* chunk = world.residentChunk(source.coordinate);
        if (chunk == nullptr || chunk->contentIdentity() != source.identity) {
            return false;
        }
    }
    for (auto const& source : column.authority_sources) {
        if (!world.matchesAuthoritativeTileIdentity(
                source.coordinate, source.identity, source.revision, source.content_hash)) {
            return false;
        }
    }
    return true;
}

bool terrainLodSourcesCurrent(TerrainLodScene const& scene, shared::SparseWorld const& world)
{
    for (TerrainLodChunkSource const& source : scene.chunk_sources) {
        shared::Chunk const* const chunk = world.residentChunk(source.coordinate);
        if (chunk == nullptr || chunk->contentIdentity() != source.identity) {
            return false;
        }
    }
    for (auto const& source : scene.authority_sources) {
        if (!world.matchesAuthoritativeTileIdentity(
                source.coordinate, source.identity, source.revision, source.content_hash)) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneFromColumns(
    shared::TerrainWorld const& world,
    uint32_t const center_x,
    uint32_t const center_y,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const columns,
    std::stop_token const& cancellation)
{
    std::vector<TerrainLodActualColumn> actual;
    actual.reserve(columns.size());
    for (auto const& input_column : columns) {
        TerrainLodColumnInput const& column = *input_column;
        TerrainLodActualColumn output{
            .x = column.x,
            .y = column.y,
            .meshes = {},
            .faces = {},
            .edge_heights = column.edge_heights,
        };
        if (!column.authority_profiles.empty()) {
            output.faces = meshAuthorityProfiles(column, cancellation);
        }
        output.meshes.reserve(column.chunks.size());
        for (TerrainLodChunkInput const& input : column.chunks) {
            if (cancellation.stop_requested()) {
                throw std::runtime_error("terrain LOD chunk meshing cancelled");
            }
            shared::ChunkMesher mesher;
            output.meshes.push_back(mesher.update(input.chunk, input.neighbors));
        }
        actual.push_back(std::move(output));
    }
    auto scene =
        std::const_pointer_cast<TerrainLodScene>(buildTerrainLodScene(world, center_x, center_y, actual, cancellation));
    std::unordered_set<uint64_t> authority_keys;
    for (auto const& input_column : columns) {
        TerrainLodColumnInput const& column = *input_column;
        scene->chunk_sources.insert(scene->chunk_sources.end(), column.sources.begin(), column.sources.end());
        for (auto const& source : column.authority_sources) {
            uint64_t const key =
                (static_cast<uint64_t>(source.coordinate.x) << 32U) | static_cast<uint32_t>(source.coordinate.y);
            if (authority_keys.insert(key).second) {
                scene->authority_sources.push_back(source);
            }
        }
    }
    return scene;
}

std::shared_ptr<TerrainLodScene const> buildTerrainLodScene(
    shared::TerrainWorld const& world,
    uint32_t const center_x,
    uint32_t const center_y,
    std::span<TerrainLodActualColumn const> const actual_columns,
    std::stop_token const& cancellation)
{
    auto const root = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0, 0U, 0U, PERIOD, cancellation);
    auto scene = std::make_shared<TerrainLodScene>();
    scene->identity = root->terrain_identity;
    scene->shading_atlas = world.atlas();
    scene->root_extent = root->spacing;
    scene->block_sea_level = world.atlas()->block_sea_level;
    std::vector<Cell> cells;
    cells.reserve(root->nodes.size() + 8'192U);
    uint32_t const side = PERIOD / root->spacing;
    for (uint32_t y = 0U; y < side; ++y) {
        for (uint32_t x = 0U; x < side; ++x) {
            collectCells(
                world,
                *root,
                {.level = shared::TerrainLodLevel::L0, .x = x, .y = y},
                root->spacing,
                center_x,
                center_y,
                cells,
                cancellation);
        }
    }
    std::unordered_map<uint64_t, uint32_t> index;
    index.reserve(cells.size());
    for (uint32_t i = 0U; i < cells.size(); ++i) {
        index.emplace(cellKey(cells[i].region), i);
    }
    auto const cell_at = [&index, &cells, &scene](uint32_t const x, uint32_t const y) -> Cell const& {
        for (uint32_t level = 3U; level-- > 0U;) {
            auto const lod = static_cast<shared::TerrainLodLevel>(level);
            uint32_t const extent = terrainLodRegionExtent(lod, scene->root_extent);
            auto const found = index.find(cellKey({.level = lod, .x = x / extent, .y = y / extent}));
            if (found != index.end()) {
                return cells[found->second];
            }
        }
        throw std::runtime_error("terrain LOD selection has a coverage hole");
    };
    std::unordered_map<uint64_t, std::array<int32_t, 2>> shared_heights;
    std::function<std::array<int32_t, 2>(uint32_t, uint32_t)> corner_height;
    corner_height = [&cell_at, &index, &cells, &shared_heights, &corner_height](
        uint32_t px,
        uint32_t py
    ) -> std::array<int32_t, 2> {
        px = wrap(px);
        py = wrap(py);
        uint64_t const key = (static_cast<uint64_t>(px) << 32U) | py;
        auto const cached = shared_heights.find(key);
        if (cached != shared_heights.end()) {
            return cached->second;
        }
        uint32_t owner_index = index.at(cellKey(cell_at(px, py).region));
        for (uint32_t dy = 0U; dy < 2U; ++dy) {
            for (uint32_t dx = 0U; dx < 2U; ++dx) {
                uint32_t const candidate_index = index.at(cellKey(
                    cell_at(wrap(static_cast<int64_t>(px) - dx), wrap(static_cast<int64_t>(py) - dy)).region
                ));
                if (cells[candidate_index].extent > cells[owner_index].extent) {
                    owner_index = candidate_index;
                }
            }
        }
        Cell const& owner = cells[owner_index];
        uint32_t const ox = owner.region.x * owner.extent;
        uint32_t const oy = owner.region.y * owner.extent;
        uint32_t const dx = wrap(static_cast<int64_t>(px) - ox);
        uint32_t const dy = wrap(static_cast<int64_t>(py) - oy);
        std::array<int32_t, 2> height{};
        if ((dx == 0U || dx == owner.extent) && (dy == 0U || dy == owner.extent)) {
            uint32_t const corner = (dy == 0U ? 0U : 2U) + (dx == 0U ? 0U : 1U);
            height = {owner.corner_heights[corner], owner.corner_temperatures[corner]};
        } else {
            bool const vertical = dx == 0U || dx == owner.extent;
            uint32_t const ax = vertical ? px : ox;
            uint32_t const ay = vertical ? oy : py;
            uint32_t const bx = vertical ? px : ox + owner.extent;
            uint32_t const by = vertical ? oy + owner.extent : py;
            auto const a = corner_height(ax, ay);
            auto const b = corner_height(bx, by);
            double const fraction = static_cast<double>(vertical ? dy : dx) / static_cast<double>(owner.extent);
            for (uint32_t component = 0U; component < 2U; ++component) {
                height[component] =
                    static_cast<int32_t>(std::lround(a[component] + (b[component] - a[component]) * fraction));
            }
        }
        shared_heights.emplace(key, height);
        return height;
    };
    std::unordered_map<uint64_t, TerrainLodActualColumn const*> actual;
    actual.reserve(actual_columns.size());
    for (TerrainLodActualColumn const& column : actual_columns) {
        bool covered = column.x < PERIOD / 16U && column.y < PERIOD / 16U;
        for (uint32_t y = 0U; y < 16U && covered; y += 8U) {
            for (uint32_t x = 0U; x < 16U; x += 8U) {
                covered = covered &&
                    cell_at(column.x * 16U + x, column.y * 16U + y).region.level == shared::TerrainLodLevel::L2;
            }
        }
        if (covered) {
            actual.emplace((static_cast<uint64_t>(column.x) << 32U) | column.y, &column);
        }
    }
    static constexpr uint32_t TRANSITION_WIDTH = 32U;
    auto const column_key = [](uint32_t const x, uint32_t const y) { return (static_cast<uint64_t>(x) << 32U) | y; };
    std::unordered_set<uint64_t> transition_columns;
    struct BoundaryPoint final {
        uint32_t x;
        uint32_t y;
        int64_t height_sum = 0;
        uint32_t count = 0U;
    };
    std::unordered_map<uint64_t, BoundaryPoint> boundary_points;
    for (auto const& [key, column] : actual) {
        static_cast<void>(key);
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                uint32_t const cx = wrap(static_cast<int64_t>(column->x) * 16 + static_cast<int64_t>(dx) * 16) / 16U;
                uint32_t const cy = wrap(static_cast<int64_t>(column->y) * 16 + static_cast<int64_t>(dy) * 16) / 16U;
                uint64_t const neighbor_key = column_key(cx, cy);
                if (!actual.contains(neighbor_key)) {
                    transition_columns.insert(neighbor_key);
                }
            }
        }
        for (uint32_t edge = 0U; edge < 4U; ++edge) {
            uint32_t const nx = wrap(static_cast<int64_t>(column->x * 16U) + (edge == 0U ? -1 : edge == 1U ? 16 : 0));
            uint32_t const ny = wrap(static_cast<int64_t>(column->y * 16U) + (edge == 2U ? -1 : edge == 3U ? 16 : 0));
            if (actual.contains(column_key(nx / 16U, ny / 16U))) {
                continue;
            }
            for (uint32_t along = 0U; along < 16U; ++along) {
                for (uint32_t endpoint = 0U; endpoint < 2U; ++endpoint) {
                    uint32_t const px =
                        wrap(column->x * 16U + (edge < 2U ? (edge == 1U ? 16U : 0U) : along + endpoint));
                    uint32_t const py =
                        wrap(column->y * 16U + (edge < 2U ? along + endpoint : (edge == 3U ? 16U : 0U)));
                    auto& point = boundary_points[column_key(px, py)];
                    point.x = px;
                    point.y = py;
                    point.height_sum += static_cast<int64_t>(column->edge_heights[edge][along]) * 32;
                    ++point.count;
                }
            }
        }
    }
    auto const coarse_properties = [&cell_at, &corner_height](uint32_t const px, uint32_t const py) {
        Cell const& owner = cell_at(wrap(px), wrap(py));
        uint32_t const ox = owner.region.x * owner.extent;
        uint32_t const oy = owner.region.y * owner.extent;
        double const half = owner.extent * 0.5;
        double const dx = static_cast<double>(wrap(static_cast<int64_t>(px) - ox)) - half;
        double const dy = static_cast<double>(wrap(static_cast<int64_t>(py) - oy)) - half;
        double const t = std::max(std::abs(dx), std::abs(dy)) / half;
        std::array<int32_t, 2> center{};
        for (uint32_t corner = 0U; corner < 4U; ++corner) {
            center[0] += owner.corner_heights[corner];
            center[1] += owner.corner_temperatures[corner];
        }
        center[0] /= 4;
        center[1] /= 4;
        if (t == 0.0) {
            return center;
        }
        bool const vertical = std::abs(dx) > std::abs(dy);
        uint32_t const ax = vertical ? ox + (dx > 0.0 ? owner.extent : 0U) : ox;
        uint32_t const ay = vertical ? oy : oy + (dy > 0.0 ? owner.extent : 0U);
        uint32_t const bx = vertical ? ax : ox + owner.extent;
        uint32_t const by = vertical ? oy + owner.extent : ay;
        auto const a = corner_height(ax, ay);
        auto const b = corner_height(bx, by);
        double const fraction = 0.5 + (vertical ? dy : dx) / (2.0 * half * t);
        for (uint32_t component = 0U; component < 2U; ++component) {
            double const edge = a[component] + (b[component] - a[component]) * fraction;
            center[component] = static_cast<int32_t>(std::lround(center[component] + (edge - center[component]) * t));
        }
        return center;
    };
    struct BoundaryCorrection final {
        uint32_t x;
        uint32_t y;
        int32_t height;
        int32_t correction;
    };
    std::unordered_map<uint64_t, std::vector<BoundaryCorrection>> boundary_buckets;
    for (auto const& [key, point] : boundary_points) {
        static_cast<void>(key);
        int32_t const height = static_cast<int32_t>(point.height_sum / point.count);
        boundary_buckets[column_key(point.x / 16U, point.y / 16U)].push_back({
            point.x,
            point.y,
            height,
            height - coarse_properties(point.x, point.y)[0],
        });
    }
    std::unordered_map<uint64_t, std::array<int32_t, 2>> transition_values;
    auto const transition_properties = [&](uint32_t px, uint32_t py) {
        px = wrap(px);
        py = wrap(py);
        uint64_t const key = column_key(px, py);
        auto const cached = transition_values.find(key);
        if (cached != transition_values.end()) {
            return cached->second;
        }
        auto value = coarse_properties(px, py);
        auto const boundary = boundary_points.find(key);
        if (boundary != boundary_points.end()) {
            value[0] = static_cast<int32_t>(boundary->second.height_sum / boundary->second.count);
        } else {
            double nearest = TRANSITION_WIDTH;
            double weighted = 0.0;
            double weight_sum = 0.0;
            for (int32_t by = -2; by <= 2; ++by) {
                for (int32_t bx = -2; bx <= 2; ++bx) {
                    uint32_t const cx = wrap(
                        static_cast<int64_t>(px / 16U) * 16 + static_cast<int64_t>(bx) * 16
                    ) / 16U;
                    uint32_t const cy = wrap(
                        static_cast<int64_t>(py / 16U) * 16 + static_cast<int64_t>(by) * 16
                    ) / 16U;
                    auto const bucket = boundary_buckets.find(column_key(cx, cy));
                    if (bucket == boundary_buckets.end()) {
                        continue;
                    }
                    for (auto const& point : bucket->second) {
                        double dx = std::abs(static_cast<double>(px) - point.x);
                        double dy = std::abs(static_cast<double>(py) - point.y);
                        dx = std::min(dx, PERIOD - dx);
                        dy = std::min(dy, PERIOD - dy);
                        double const distance = std::hypot(dx, dy);
                        if (distance >= TRANSITION_WIDTH) {
                            continue;
                        }
                        nearest = std::min(nearest, distance);
                        double const falloff = 1.0 - distance / TRANSITION_WIDTH;
                        double const weight = falloff * falloff / (1.0 + distance * distance);
                        weighted += point.correction * weight;
                        weight_sum += weight;
                    }
                }
            }
            if (weight_sum > 0.0) {
                value[0] +=
                    static_cast<int32_t>(std::lround(weighted / weight_sum * (1.0 - nearest / TRANSITION_WIDTH)));
            }
        }
        value[0] = std::clamp(value[0], 32, 32'768);
        transition_values.emplace(key, value);
        return value;
    };
    // A coarse cell can meet the band at its far edge while its origin lies outside the band.
    std::unordered_set<uint64_t> transition_cells;
    transition_cells.reserve(transition_columns.size());
    for (uint64_t const key : transition_columns) {
        uint32_t const x = static_cast<uint32_t>(key >> 32U) * 16U;
        uint32_t const y = static_cast<uint32_t>(key) * 16U;
        uint32_t const step = std::min(16U, terrainLodRegionExtent(shared::TerrainLodLevel::L2, scene->root_extent));
        for (uint32_t dy = 0U; dy < 16U; dy += step) {
            for (uint32_t dx = 0U; dx < 16U; dx += step) {
                transition_cells.insert(cellKey(cell_at(x + dx, y + dy).region));
            }
        }
    }
    scene->faces.reserve(cells.size() * 3U);
    scene->groups.reserve(cells.size());
    scene->leaves.reserve(cells.size());
    for (Cell const& cell : cells) {
        uint32_t const x = cell.region.x * cell.extent;
        uint32_t const y = cell.region.y * cell.extent;
        bool const actual_ready = cell.region.level == shared::TerrainLodLevel::L2 &&
            actual.contains((static_cast<uint64_t>(x / 16U) << 32U) | (y / 16U));
        if (actual_ready) {
            continue;
        }
        uint32_t const first = static_cast<uint32_t>(scene->faces.size());
        if (transition_cells.contains(cellKey(cell.region))) {
            int32_t minimum = 65'535;
            int32_t maximum = 0;
            for (uint32_t ty = y; ty < y + cell.extent; ty += 2U) {
                for (uint32_t tx = x; tx < x + cell.extent; tx += 2U) {
                    auto const cp = transition_properties(tx + 1U, ty + 1U);
                    std::array<int32_t, 3> const center{
                        static_cast<int32_t>(tx + 1U),
                        static_cast<int32_t>(ty + 1U),
                        cp[0],
                    };
                    std::array<std::array<int32_t, 3>, 8> starts{};
                    std::array<std::array<int32_t, 3>, 8> ends{};
                    std::array<int32_t, 8> ta{};
                    std::array<int32_t, 8> tb{};
                    for (uint32_t segment = 0U; segment < 8U; ++segment) {
                        uint32_t const edge = segment / 2U;
                        uint32_t const along = segment % 2U;
                        uint32_t const ax = edge == 0U ? tx + along
                            : edge == 1U               ? tx + 2U
                            : edge == 2U               ? tx + 2U - along
                            : tx;
                        uint32_t const ay = edge == 0U ? ty
                            : edge == 1U               ? ty + along
                            : edge == 2U               ? ty + 2U
                            : ty + 2U - along;
                        uint32_t const bx = ax + (edge == 0U ? 1U : 0U) - (edge == 2U ? 1U : 0U);
                        uint32_t const by = ay + (edge == 1U ? 1U : 0U) - (edge == 3U ? 1U : 0U);
                        auto ap = transition_properties(ax, ay);
                        auto bp = transition_properties(bx, by);
                        uint32_t const nx = wrap(static_cast<int64_t>(std::min(ax, bx)) + (edge == 3U ? -1 : 0));
                        uint32_t const ny = wrap(static_cast<int64_t>(std::min(ay, by)) + (edge == 0U ? -1 : 0));
                        auto const neighbor = actual.find(column_key(nx / 16U, ny / 16U));
                        if (neighbor != actual.end()) {
                            static constexpr std::array<uint32_t, 4> ACTUAL_EDGE{3U, 0U, 2U, 1U};
                            uint32_t const offset = (edge % 2U == 0U ? nx : ny) % 16U;
                            ap[0] = neighbor->second->edge_heights[ACTUAL_EDGE[edge]][offset] * 32;
                            bp[0] = ap[0];
                        }
                        starts[segment] = {static_cast<int32_t>(ax), static_cast<int32_t>(ay), ap[0]};
                        ends[segment] = {static_cast<int32_t>(bx), static_cast<int32_t>(by), bp[0]};
                        ta[segment] = ap[1];
                        tb[segment] = bp[1];
                        minimum = std::min({minimum, cp[0], ap[0], bp[0]});
                        maximum = std::max({maximum, cp[0], ap[0], bp[0]});
                    }
                    for (uint32_t segment = 0U; segment < 8U; ++segment) {
                        scene->faces.push_back(
                            StoneFaceInstance::fromTerrainSurfaceTriangle(
                            {center, starts[segment], ends[segment]}, {cp[1], ta[segment], tb[segment]}));
                        uint32_t const next = (segment + 1U) % 8U;
                        if (ends[segment][2] != starts[next][2]) {
                            scene->faces.push_back(
                            StoneFaceInstance::fromTerrainSurfaceTriangle(
                            {center, ends[segment], starts[next]}, {cp[1], tb[segment], ta[next]}));
                            scene->faces.push_back(
                            StoneFaceInstance::fromTerrainSurfaceTriangle(
                            {center, starts[next], ends[segment]}, {cp[1], ta[next], tb[segment]}));
                        }
                    }
                }
            }
            scene->groups.push_back({
                .range =
                    {.first_instance = first, .instance_count = static_cast<uint32_t>(scene->faces.size()) - first},
                .origin_x = x,
                .origin_y = y,
                .extent = cell.extent,
                .minimum_z = minimum / 32,
                .maximum_z = (maximum + 31) / 32,
            });
            scene->leaves.push_back(cell.region);
            continue;
        }
        std::array<int32_t, 3> const center{
            static_cast<int32_t>(x + cell.extent / 2U),
            static_cast<int32_t>(y + cell.extent / 2U),
            (cell.corner_heights[0] + cell.corner_heights[1] + cell.corner_heights[2] + cell.corner_heights[3]) / 4,
        };
        int32_t minimum_height = center[2];
        int32_t maximum_height = center[2];
        for (uint32_t edge = 0U; edge < 4U; ++edge) {
            uint32_t const along_origin = edge < 2U ? y : x;
            uint32_t offset = 0U;
            while (offset < cell.extent) {
                uint32_t const across = edge < 2U
                    ? wrap(static_cast<int64_t>(x) + (edge == 0U ? -1 : static_cast<int64_t>(cell.extent)))
                    : wrap(static_cast<int64_t>(y) + (edge == 2U ? -1 : static_cast<int64_t>(cell.extent)));
                Cell const& neighbor =
                    edge < 2U ? cell_at(across, along_origin + offset) : cell_at(along_origin + offset, across);
                uint32_t const neighbor_origin =
                    edge < 2U ? neighbor.region.y * neighbor.extent : neighbor.region.x * neighbor.extent;
                uint32_t length =
                    std::min(cell.extent - offset, neighbor_origin + neighbor.extent - (along_origin + offset));
                uint32_t const nx = edge < 2U ? across : along_origin + offset;
                uint32_t const ny = edge < 2U ? along_origin + offset : across;
                auto const actual_neighbor = actual.find((static_cast<uint64_t>(nx / 16U) << 32U) | (ny / 16U));
                if (actual_neighbor != actual.end() || transition_cells.contains(cellKey(neighbor.region))) {
                    length = 1U;
                }
                uint32_t const ex = edge < 2U ? x + (edge == 1U ? cell.extent : 0U) : along_origin + offset;
                uint32_t const ey = edge < 2U ? along_origin + offset : y + (edge == 3U ? cell.extent : 0U);
                std::array<int32_t, 3> a{
                    static_cast<int32_t>(ex),
                    static_cast<int32_t>(ey),
                    corner_height(ex, ey)[0],
                };
                std::array<int32_t, 3> b{
                    static_cast<int32_t>(ex + (edge < 2U ? 0U : length)),
                    static_cast<int32_t>(ey + (edge < 2U ? length : 0U)),
                    corner_height(ex + (edge < 2U ? 0U : length), ey + (edge < 2U ? length : 0U))[0],
                };
                minimum_height = std::min({minimum_height, a[2], b[2]});
                maximum_height = std::max({maximum_height, a[2], b[2]});
                int32_t temperature_a = corner_height(ex, ey)[1];
                int32_t temperature_b =
                    corner_height(ex + (edge < 2U ? 0U : length), ey + (edge < 2U ? length : 0U))[1];
                if (edge == 0U || edge == 3U) {
                    std::swap(a, b);
                    std::swap(temperature_a, temperature_b);
                }
                int32_t const center_temperature = (cell.corner_temperatures[0] + cell.corner_temperatures[1] +
                            cell.corner_temperatures[2] + cell.corner_temperatures[3]) /
                    4;
                scene->faces.push_back(
                    StoneFaceInstance::fromTerrainSurfaceTriangle(
                        {center, a, b}, {center_temperature, temperature_a, temperature_b}));
                offset += length;
            }
        }
        scene->groups.push_back({
            .range = {.first_instance = first, .instance_count = static_cast<uint32_t>(scene->faces.size()) - first},
            .origin_x = x,
            .origin_y = y,
            .extent = cell.extent,
            .minimum_z = minimum_height / 32,
            .maximum_z = (maximum_height + 31) / 32,
        });
        scene->leaves.push_back(cell.region);
    }
    for (auto const& [key, column_pointer] : actual) {
        static_cast<void>(key);
        TerrainLodActualColumn const& column = *column_pointer;
        uint32_t const first = static_cast<uint32_t>(scene->faces.size());
        scene->faces.insert(scene->faces.end(), column.faces.begin(), column.faces.end());
        for (shared::ChunkMesh const& mesh : column.meshes) {
            for (shared::MeshFace const& face : mesh.faces) {
                scene->faces.push_back(StoneFaceInstance::fromChunkFace(mesh.coordinate, face));
            }
        }
        scene->groups.push_back({
            .range = {.first_instance = first, .instance_count = static_cast<uint32_t>(scene->faces.size()) - first},
            .origin_x = column.x * 16U,
            .origin_y = column.y * 16U,
            .extent = 16U,
            .minimum_z = 0,
            .maximum_z = 1'024,
        });
        scene->leaves.push_back({.level = shared::TerrainLodLevel::L3, .x = column.x, .y = column.y});
    }
    scene->actual_column_count = static_cast<uint32_t>(actual.size());
    return scene;
}

} // namespace client
