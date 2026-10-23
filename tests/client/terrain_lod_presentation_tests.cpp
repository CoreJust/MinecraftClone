#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

std::shared_ptr<shared::TerrainWorld const>
presentationWorld(uint32_t const resolution, bool const slope = false, bool const climate = false)
{
    auto atlas = std::make_shared<generation::GlobalAtlas>();
    atlas->configuration.resolution = resolution;
    atlas->vertical_scale = 10.0;
    uint32_t const count = resolution * resolution;
    atlas->elevation.assign(count, 1.5F);
    if (slope) {
        for (uint32_t y = 0U; y < resolution; ++y) {
            for (uint32_t x = 0U; x < resolution; ++x) {
                atlas->elevation[y * resolution + x] += static_cast<float>(x % 32U) * 0.5F;
            }
        }
    }
    atlas->temperature.assign(count, 12.0F);
    if (climate) {
        for (uint32_t y = 0U; y < resolution; ++y) {
            for (uint32_t x = 0U; x < resolution; ++x) {
                atlas->temperature[y * resolution + x] = static_cast<float>(x % 32U) - 4.0F;
            }
        }
    }
    atlas->precipitation.assign(count, 0.3F);
    atlas->water_level.assign(count, 0.0F);
    atlas->ocean_mask.assign(count, 0U);
    atlas->lake_mask.assign(count, 0U);
    atlas->river_mask.assign(count, 0U);
    return std::make_shared<shared::TerrainWorld const>(atlas);
}

[[nodiscard]] std::unique_ptr<shared::SurfaceTile> sampleSurfaceOnHeap(
    shared::TerrainWorld const& terrain,
    shared::ChunkCoordinate const coordinate
)
{
    auto surface = std::make_unique<shared::SurfaceTile>();
    *surface = terrain.sampleSurface(coordinate);
    return surface;
}

[[nodiscard]] bool publishMaterializedChunk(
    shared::SparseWorld& world,
    shared::TerrainWorld const& terrain,
    shared::SurfaceTile const& surface,
    shared::ChunkCoordinate const coordinate
)
{
    shared::Chunk const chunk = terrain.materializeChunk(surface, coordinate);
    return world.publishMaterializedChunk(chunk, terrain.identity().revision, terrain.identity().seed);
}

TEST(TerrainLodPresentationTest, CompletePeriodicWorldKeepsCoverageWhileRefiningAcrossTheWrap)
{
    static constexpr uint64_t WORLD_AREA = 65'536ULL * 65'536ULL;
    auto const world = presentationWorld(256U);
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U);
    EXPECT_EQ(scene->identity, world->identity());
    EXPECT_EQ(scene->shading_atlas, world->atlas());
    ASSERT_FALSE(scene->faces.empty());
    ASSERT_EQ(scene->groups.size(), scene->leaves.size());
    uint64_t covered_area = 0U;
    uint32_t l0_count = 0U;
    uint32_t l2_count = 0U;
    for (client::TerrainLodRegion const leaf : scene->leaves) {
        uint64_t const extent = client::terrainLodRegionExtent(leaf.level, scene->root_extent);
        covered_area += extent * extent;
        l0_count += leaf.level == shared::TerrainLodLevel::L0 ? 1U : 0U;
        l2_count += leaf.level == shared::TerrainLodLevel::L2 ? 1U : 0U;
    }
    EXPECT_EQ(covered_area, WORLD_AREA);
    EXPECT_GT(l0_count, 0U);
    EXPECT_GT(l2_count, 0U);
    EXPECT_EQ(scene->actual_column_count, 0U);
    for (client::TerrainLodDrawGroup const& group : scene->groups) {
        EXPECT_LE(static_cast<uint64_t>(group.range.first_instance) + group.range.instance_count, scene->faces.size());
    }
}

TEST(TerrainLodPresentationTest, Actual512ChunkMasksExactlyFourApproximateCells)
{
    static constexpr uint64_t WORLD_AREA = 65'536ULL * 65'536ULL;
    auto const world = presentationWorld(512U);
    client::TerrainLodActualColumn actual{.x = 0U, .y = 0U, .meshes = {}, .faces = {}};
    for (auto& edge : actual.edge_heights) {
        edge.fill(400U);
    }
    shared::ChunkMesh mesh{
        .coordinate = {.x = 0, .y = 0, .z = 24},
        .content_identity = {},
        .faces =
            {{.local_origin = {.x = 0U, .y = 0U, .z = 15U},
              .direction = shared::FaceDirection::PositiveZ,
              .material = shared::Block::Stone}},
    };
    actual.meshes.push_back(mesh);
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, std::span{&actual, 1U});
    EXPECT_EQ(scene->actual_column_count, 1U);
    uint64_t area = 0U;
    uint32_t actual_count = 0U;
    for (client::TerrainLodRegion const leaf : scene->leaves) {
        uint64_t const extent = client::terrainLodRegionExtent(leaf.level, scene->root_extent);
        area += extent * extent;
        actual_count += leaf.level == shared::TerrainLodLevel::L3 ? 1U : 0U;
        if (leaf.level == shared::TerrainLodLevel::L2) {
            EXPECT_FALSE(leaf.x < 2U && leaf.y < 2U);
        }
    }
    EXPECT_EQ(area, WORLD_AREA);
    EXPECT_EQ(actual_count, 1U);
    std::set<std::array<int32_t, 4>> joined_block_edges;
    for (client::StoneFaceInstance const& face : scene->faces) {
        if (face.direction != client::StoneFaceInstance::TERRAIN_TRIANGLE) {
            continue;
        }
        auto const dx = [](uint32_t const packed) { return static_cast<int32_t>(static_cast<int16_t>(packed)); };
        auto const dy = [](uint32_t const packed) { return static_cast<int32_t>(static_cast<int16_t>(packed >> 16U)); };
        auto const wrap_coordinate = [](int32_t const coordinate) {
            return static_cast<int32_t>(static_cast<uint32_t>(coordinate) & 0xffffU);
        };
        uint32_t const heights = static_cast<uint32_t>(face.z);
        std::array<std::array<int32_t, 3>, 3> const vertices{
            std::array{
                wrap_coordinate(face.x),
                wrap_coordinate(face.y),
                static_cast<int32_t>(heights & 0xffffU)},
            std::array{
                wrap_coordinate(face.x + dx(face.u_extent)),
                wrap_coordinate(face.y + dy(face.u_extent)),
                static_cast<int32_t>(heights >> 16U)},
            std::array{
                wrap_coordinate(face.x + dx(face.v_extent)),
                wrap_coordinate(face.y + dy(face.v_extent)),
                static_cast<int32_t>(face.reserved_1 & 0xffffU)},
        };
        for (uint32_t i = 0U; i < 3U; ++i) {
            auto a = vertices[i];
            auto b = vertices[(i + 1U) % 3U];
            if (a[2] != 400 * 32 || b[2] != 400 * 32) {
                continue;
            }
            if (a > b) {
                std::swap(a, b);
            }
            bool const vertical =
                a[0] == b[0] && (a[0] == 0 || a[0] == 16) && a[1] >= 0 && b[1] <= 16 && b[1] - a[1] == 1;
            bool const horizontal =
                a[1] == b[1] && (a[1] == 0 || a[1] == 16) && a[0] >= 0 && b[0] <= 16 && b[0] - a[0] == 1;
            if (vertical || horizontal) {
                joined_block_edges.insert({a[0], a[1], b[0], b[1]});
            }
        }
    }
    EXPECT_EQ(joined_block_edges.size(), 64U);
    EXPECT_TRUE(std::ranges::any_of(scene->faces, [](client::StoneFaceInstance const& face) {
        return face.x == 0 && face.y == 0 && face.z == 399 && face.direction == 5U;
    }));
    EXPECT_FALSE(std::ranges::any_of(scene->faces, [](client::StoneFaceInstance const& face) {
        return face.direction < 4U && face.z == 0 && face.v_extent == 400U;
    }));
    EXPECT_FALSE(std::ranges::any_of(scene->faces, [](client::StoneFaceInstance const& face) {
        return face.direction == 4U && face.z == 0 && face.u_extent == 16U && face.v_extent == 16U;
    }));
}

TEST(TerrainLodPresentationTest, MissingMaterializedSurfaceRetainsApproximationAndSourceEditsRejectOldMesh)
{
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    EXPECT_FALSE(client::captureTerrainLodColumn(world, 0U, 0U).has_value());
    shared::Chunk chunk = shared::Chunk::makeStoneFixture();
    ASSERT_TRUE(world.publishMaterializedChunk(chunk, 1U, shared::WorldConfiguration::SEED));
    client::TerrainLodScene scene;
    scene.chunk_sources.push_back({.coordinate = chunk.coordinate(), .identity = chunk.contentIdentity()});
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    ASSERT_TRUE(world.setBlock({.x = 0, .y = 0, .z = 0}, shared::Block::Air));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(scene, world));
}

} // namespace

TEST(TerrainLodPresentationTest, SlopingAtlasUsesContinuousTrianglesRatherThanHorizontalColumns)
{
    auto const world = presentationWorld(256U, true);
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U);
    bool slope = false;
    for (client::StoneFaceInstance const& face : scene->faces) {
        if (face.direction == 6U) {
            uint32_t const heights = static_cast<uint32_t>(face.z);
            slope =
                slope || (heights & 0xffffU) != (heights >> 16U) || (heights & 0xffffU) != (face.reserved_1 & 0xffffU);
        }
    }
    EXPECT_TRUE(slope);
}

TEST(TerrainLodPresentationTest, EveryInterpolatedEdgeIsSharedExactlyAcrossLevelsAndPeriodicSeam)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto const world = presentationWorld(resolution, true, true);
        auto const scene = client::buildTerrainLodScene(*world, 0U, 0U);
        std::map<std::array<uint64_t, 2>, uint32_t> edges;
        auto const vertex_key = [](int32_t const x, int32_t const y, uint32_t const z, int32_t const temperature) {
            return (static_cast<uint64_t>(static_cast<uint16_t>(temperature)) << 48U) |
                (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0xffffU) << 32U) |
                (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xffffU) << 16U) | z;
        };
        for (client::StoneFaceInstance const& face : scene->faces) {
            ASSERT_EQ(face.direction, client::StoneFaceInstance::TERRAIN_TRIANGLE);
            uint32_t const heights = static_cast<uint32_t>(face.z);
            uint32_t const encoded_temperature = (face.reserved_1 << 1U) >> 17U;
            int32_t const temperature = encoded_temperature < 0x4000U
                ? static_cast<int32_t>(encoded_temperature)
                : static_cast<int32_t>(encoded_temperature) - 0x8000;
            auto const dx = [](uint32_t const packed) {
                return static_cast<int32_t>(static_cast<int16_t>(packed & 0xffffU));
            };
            auto const dy = [](uint32_t const packed) {
                return static_cast<int32_t>(static_cast<int16_t>(packed >> 16U));
            };
            std::array<uint64_t, 3> const vertices{
                vertex_key(
                    face.x,
                    face.y,
                    heights & 0xffffU,
                    temperature
                ),
                vertex_key(
                    face.x + dx(face.u_extent),
                    face.y + dy(face.u_extent),
                    heights >> 16U,
                    static_cast<int16_t>(face.material_id)),
                vertex_key(
                    face.x + dx(face.v_extent),
                    face.y + dy(face.v_extent),
                    face.reserved_1 & 0xffffU,
                    static_cast<int16_t>(face.material_id >> 16U)),
            };
            for (uint32_t i = 0U; i < 3U; ++i) {
                std::array<uint64_t, 2> edge{vertices[i], vertices[(i + 1U) % 3U]};
                std::sort(edge.begin(), edge.end());
                ++edges[edge];
            }
        }
        uint32_t unmatched = 0U;
        for (auto const& [edge, count] : edges) {
            static_cast<void>(edge);
            unmatched += count != 2U ? 1U : 0U;
        }
        EXPECT_EQ(unmatched, 0U) << "atlas resolution " << resolution;
    }
}

TEST(TerrainLodPresentationTest, LowerResidentChunkWithMissingNeighborsIsCapturedWithClosingFaces)
{
    auto const terrain = presentationWorld(256U);
    auto const surface = sampleSurfaceOnHeap(*terrain, {.x = 1, .y = 1, .z = 0});
    shared::SparseWorld world{{.max_resident_chunks = 256U}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    ASSERT_TRUE(world.publishSurfaceMetadata(*surface));
    uint32_t minimum_z = 63U;
    uint32_t maximum_z = 0U;
    for (auto const& column : surface->columns) {
        minimum_z = std::min(minimum_z, static_cast<uint32_t>(column.highest_solid_z / 16U));
        maximum_z = std::max(maximum_z, static_cast<uint32_t>(column.highest_solid_z / 16U));
    }
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 3; ++x) {
            auto const neighbor_surface = sampleSurfaceOnHeap(*terrain, {.x = x, .y = y, .z = 0});
            for (uint32_t z = minimum_z - 1U; z <= maximum_z + 1U; ++z) {
                ASSERT_TRUE(publishMaterializedChunk(
                    world,
                    *terrain,
                    *neighbor_surface,
                    {.x = x, .y = y, .z = static_cast<int32_t>(z)}
                ));
            }
        }
    }
    ASSERT_TRUE(publishMaterializedChunk(world, *terrain, *surface, {.x = 1, .y = 1, .z = 0}));
    auto const capture = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(capture.has_value());
    auto const found = std::ranges::find_if(
        capture->chunks, [](client::TerrainLodChunkInput const& chunk) { return chunk.chunk.coordinate().z == 0; });
    ASSERT_NE(found, capture->chunks.end());
    auto const mesher = std::make_unique<shared::ChunkMesher>();
    auto const mesh = mesher->update(found->chunk, found->neighbors);
    EXPECT_FALSE(mesh.faces.empty());
}

TEST(TerrainLodPresentationTest, MissingChunkBetweenVerifiedHighAndLowSurfaceRetainsApproximation)
{
    auto const terrain = presentationWorld(256U);
    auto surface = terrain->sampleSurface({.x = 1, .y = 1, .z = 0});
    uint32_t const low_z = surface.columns[0].highest_solid_z / 16U;
    uint32_t const high_z = low_z + 4U;
    surface.columns[0].highest_solid_z = static_cast<uint16_t>(high_z * 16U + 15U);
    surface.columns[0].ground_first_z = static_cast<uint16_t>(surface.columns[0].highest_solid_z - 4U);
    surface.columns[0].ground_end_z = surface.columns[0].highest_solid_z;
    shared::SparseWorld world{{.max_resident_chunks = 256U}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    ASSERT_TRUE(world.publishSurfaceMetadata(surface));
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 3; ++x) {
            for (uint32_t z = low_z - 1U; z <= high_z + 1U; ++z) {
                if (z == low_z + 2U) {
                    continue;
                }
                shared::ChunkCoordinate const coordinate{.x = x, .y = y, .z = static_cast<int32_t>(z)};
                auto const chunk =
                    x == 1 && y == 1 ? terrain->materializeChunk(surface, coordinate) : shared::Chunk{coordinate};
                ASSERT_TRUE(
                    world.publishMaterializedChunk(chunk, terrain->identity().revision, terrain->identity().seed));
            }
        }
    }
    ASSERT_TRUE(world.verifiedSurfaceColumn(16, 16).has_value());
    ASSERT_TRUE(world.verifiedSurfaceColumn(17, 16).has_value());
    EXPECT_FALSE(client::captureTerrainLodColumn(world, 1U, 1U).has_value());
}

TEST(TerrainLodPresentationTest, SmoothSurfaceCarriesInterpolatedClimateInsteadOfFlatCellMaterial)
{
    auto const world = presentationWorld(256U, true);
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U);
    for (auto const& face : scene->faces) {
        ASSERT_EQ(face.direction, 6U);
        ASSERT_NE(face.reserved_1 & 0x8000'0000U, 0U);
        EXPECT_EQ(static_cast<int16_t>(face.material_id & 0xffffU), 12 * 32);
        EXPECT_EQ(static_cast<int16_t>(face.material_id >> 16U), 12 * 32);
    }
}

TEST(TerrainLodPresentationTest, FlatActualBoundaryEndsTheSmoothTopAtAuthoritativeHeight)
{
    auto const world = presentationWorld(256U);
    client::TerrainLodActualColumn actual{.x = 2048U, .y = 2048U, .meshes = {}, .faces = {}};
    for (auto& edge : actual.edge_heights) {
        edge.fill(600U);
    }
    auto const scene = client::buildTerrainLodScene(*world, 32'768U, 32'768U, std::span{&actual, 1U});
    uint32_t joined = 0U;
    for (auto const& face : scene->faces) {
        if (face.direction != 6U || (face.reserved_1 & 0x8000'0000U) == 0U) {
            continue;
        }
        uint32_t const h = static_cast<uint32_t>(face.z);
        if ((h & 0xffffU) == 600U * 32U && (h >> 16U) == 600U * 32U) {
            ++joined;
        }
        if ((h >> 16U) == 600U * 32U && (face.reserved_1 & 0xffffU) == 600U * 32U) {
            ++joined;
        }
    }
    EXPECT_GE(joined, 64U);
}

namespace {

uint32_t unmatchedActualTopEdges(
    client::TerrainLodScene const& scene, std::span<client::TerrainLodActualColumn const> const columns)
{
    std::map<std::array<uint32_t, 4>, uint32_t> required;
    auto const key = [](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        x0 &= 0xffffU;
        y0 &= 0xffffU;
        x1 &= 0xffffU;
        y1 &= 0xffffU;
        if (std::array{x0, y0} > std::array{x1, y1}) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        return std::array{x0, y0, x1, y1};
    };
    for (auto const& column : columns) {
        for (uint32_t edge = 0U; edge < 4U; ++edge) {
            uint32_t const nx = (column.x + (edge == 0U ? 4095U : edge == 1U ? 1U : 0U)) % 4096U;
            uint32_t const ny = (column.y + (edge == 2U ? 4095U : edge == 3U ? 1U : 0U)) % 4096U;
            if (std::ranges::any_of(columns, [nx, ny](auto const& other) { return other.x == nx && other.y == ny; })) {
                continue;
            }
            for (uint32_t along = 0U; along < 16U; ++along) {
                uint32_t const x = column.x * 16U + (edge < 2U ? (edge == 1U ? 16U : 0U) : along);
                uint32_t const y = column.y * 16U + (edge < 2U ? along : (edge == 3U ? 16U : 0U));
                required[key(x, y, x + (edge < 2U ? 0U : 1U), y + (edge < 2U ? 1U : 0U))] =
                    column.edge_heights[edge][along] * 32U;
            }
        }
    }
    for (auto const& face : scene.faces) {
        if (face.direction != 6U || (face.reserved_1 & 0x8000'0000U) == 0U) {
            continue;
        }
        uint32_t const heights = static_cast<uint32_t>(face.z);
        std::array<std::array<uint32_t, 3>, 3> const v{
            std::array{static_cast<uint32_t>(face.x), static_cast<uint32_t>(face.y), heights & 0xffffU},
            std::array{
                static_cast<uint32_t>(face.x + static_cast<int16_t>(face.u_extent)),
                static_cast<uint32_t>(face.y + static_cast<int16_t>(face.u_extent >> 16U)),
                heights >> 16U},
            std::array{
                static_cast<uint32_t>(face.x + static_cast<int16_t>(face.v_extent)),
                static_cast<uint32_t>(face.y + static_cast<int16_t>(face.v_extent >> 16U)),
                face.reserved_1 & 0xffffU},
        };
        for (uint32_t i = 0U; i < 3U; ++i) {
            auto const& a = v[i];
            auto const& b = v[(i + 1U) % 3U];
            auto const found = required.find(key(a[0], a[1], b[0], b[1]));
            if (found != required.end() && a[2] == found->second && b[2] == found->second) {
                required.erase(found);
            }
        }
    }
    return static_cast<uint32_t>(required.size());
}

client::TerrainLodActualColumn canonicalActualColumn(shared::TerrainWorld const& world, uint32_t x, uint32_t y)
{
    auto const surface = sampleSurfaceOnHeap(
        world,
        {.x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y), .z = 0}
    );
    client::TerrainLodActualColumn result{.x = x, .y = y, .meshes = {}, .faces = {}};
    uint32_t low = 63U, high = 0U;
    for (uint32_t along = 0U; along < 16U; ++along) {
        uint32_t const row_start = along * 16U;
        result.edge_heights[0][along] = surface->columns[row_start].highest_solid_z + 1U;
        result.edge_heights[1][along] = surface->columns[row_start + 15U].highest_solid_z + 1U;
        result.edge_heights[2][along] = surface->columns[along].highest_solid_z + 1U;
        result.edge_heights[3][along] = surface->columns[240U + along].highest_solid_z + 1U;
    }
    for (auto const& column : surface->columns) {
        low = std::min(low, static_cast<uint32_t>(column.highest_solid_z / 16U));
        high = std::max(high, static_cast<uint32_t>(column.highest_solid_z / 16U));
    }
    for (uint32_t z = low; z <= high; ++z) {
        auto const chunk = std::make_unique<shared::Chunk>(world.materializeChunk(
            *surface,
            {.x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y), .z = static_cast<int32_t>(z)}
        ));
        auto neighbors = std::make_unique<shared::ChunkNeighborFaces>();
        for (auto& face : neighbors->faces) {
            face = std::array<shared::Block, 256>{};
        }
        shared::ChunkMesher mesher;
        result.meshes.push_back(mesher.update(*chunk, *neighbors));
    }
    return result;
}

} // namespace

TEST(TerrainLodPresentationTest, CanonicalR6TopEdgesJoinTransitionAtSpawnAndMovedCenter)
{
    auto const world = std::make_shared<shared::TerrainWorld const>(generation::prepareAtlas({}));
    std::array const columns{canonicalActualColumn(*world, 2056U, 2048U)};
    for (uint32_t const center_y : {32'768U, 32'832U}) {
        auto const scene = client::buildTerrainLodScene(*world, 32'896U, center_y, columns);
        EXPECT_EQ(unmatchedActualTopEdges(*scene, columns), 0U);
    }
}

TEST(TerrainLodPresentationTest, SteppedAdjacentConcaveAndWrappedActualEdgesShareTheirTopLimits)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto const world = presentationWorld(resolution);
        std::array<client::TerrainLodActualColumn, 3> columns{
            client::TerrainLodActualColumn{.x = 4095U, .y = 4095U, .meshes = {}, .faces = {}},
            client::TerrainLodActualColumn{.x = 0U, .y = 4095U, .meshes = {}, .faces = {}},
            client::TerrainLodActualColumn{.x = 4095U, .y = 0U, .meshes = {}, .faces = {}},
        };
        for (auto& column : columns) {
            for (auto& edge : column.edge_heights) {
                for (uint32_t i = 0U; i < 16U; ++i) {
                    edge[i] = static_cast<uint16_t>(460U + i / 4U);
                }
            }
        }
        auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, columns);
        EXPECT_EQ(unmatchedActualTopEdges(*scene, columns), 0U);
    }
}

TEST(TerrainLodPresentationTest, TransitionOuterEdgeAndInteriorHaveNoUnmatchedTopSegments)
{
    auto const world = presentationWorld(256U, true);
    for (std::array<uint32_t, 4> const point : {std::array{2'048U, 2'048U, 32'768U, 32'768U},
             std::array{4'052U, 0U, 0U, 0U}, std::array{3'780U, 365U, 60'490U, 6'508U}}) {
        client::TerrainLodActualColumn actual{.x = point[0], .y = point[1], .meshes = {}, .faces = {}};
        for (auto& edge : actual.edge_heights) {
            edge.fill(600U);
        }
        auto const scene = client::buildTerrainLodScene(*world, point[2], point[3], std::span{&actual, 1U});
        std::map<std::array<uint64_t, 2>, uint32_t> edges;
        auto const vertex = [](int32_t x, int32_t y, uint32_t h) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0xffffU) << 32U) |
                (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xffffU) << 16U) | h;
        };
        for (auto const& face : scene->faces) {
            if (face.direction != 6U) {
                continue;
            }
            uint32_t const heights = static_cast<uint32_t>(face.z);
            std::array const v{
                vertex(face.x, face.y, heights & 0xffffU),
                vertex(
                    face.x + static_cast<int16_t>(face.u_extent),
                    face.y + static_cast<int16_t>(face.u_extent >> 16U),
                    heights >> 16U),
                vertex(
                    face.x + static_cast<int16_t>(face.v_extent),
                    face.y + static_cast<int16_t>(face.v_extent >> 16U),
                    face.reserved_1 & 0xffffU),
            };
            for (uint32_t i = 0U; i < 3U; ++i) {
                std::array key{v[i], v[(i + 1U) % 3U]};
                std::sort(key.begin(), key.end());
                ++edges[key];
            }
        }
        uint32_t open = 0U, invalid = 0U;
        for (auto const& [key, count] : edges) {
            static_cast<void>(key);
            open += count == 1U ? 1U : 0U;
            invalid += count > 2U ? 1U : 0U;
        }
        EXPECT_EQ(open, 64U);
        EXPECT_EQ(invalid, 0U);
        EXPECT_EQ(unmatchedActualTopEdges(*scene, std::span{&actual, 1U}), 0U);
    }
}

namespace {

using TopPoint = std::array<double, 3>;

std::array<TopPoint, 3> trianglePoints(client::StoneFaceInstance const& face)
{
    uint32_t const heights = static_cast<uint32_t>(face.z);
    return {{
        {double(face.x), double(face.y), double(heights & 0xffffU) / 32.0},
        {face.x + double(static_cast<int16_t>(face.u_extent)),
            face.y + double(static_cast<int16_t>(face.u_extent >> 16U)), double(heights >> 16U) / 32.0},
        {face.x + double(static_cast<int16_t>(face.v_extent)),
            face.y + double(static_cast<int16_t>(face.v_extent >> 16U)), double(face.reserved_1 & 0xffffU) / 32.0},
    }};
}

std::optional<double> topHeightAt(client::TerrainLodScene const& scene, double x, double y)
{
    for (auto const& group : scene.groups) {
        if (x < group.origin_x || x >= group.origin_x + group.extent
            || y < group.origin_y || y >= group.origin_y + group.extent) {
            continue;
        }
        for (uint32_t i = group.range.first_instance; i < group.range.first_instance + group.range.instance_count; ++i) {
            auto const& face = scene.faces[i];
            if (face.direction == 5U && x >= static_cast<double>(face.x)
                && x < static_cast<double>(face.x) + static_cast<double>(face.u_extent)
                && y >= static_cast<double>(face.y)
                && y < static_cast<double>(face.y) + static_cast<double>(face.v_extent)) {
                return face.z + 1.0;
            }
            if (face.direction != 6U) {
                continue;
            }
            auto const v = trianglePoints(face);
            double const area = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1])
                - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]);
            if (area <= 0.0) {
                continue;
            }
            double const a = ((v[1][0] - x) * (v[2][1] - y) - (v[2][0] - x) * (v[1][1] - y)) / area;
            double const b = ((v[2][0] - x) * (v[0][1] - y) - (v[0][0] - x) * (v[2][1] - y)) / area;
            double const c = 1.0 - a - b;
            if (a >= -1e-8 && b >= -1e-8 && c >= -1e-8) {
                return a * v[0][2] + b * v[1][2] + c * v[2][2];
            }
        }
    }
    return std::nullopt;
}

} // namespace

TEST(TerrainLodPresentationTest, ActualEdgesAtRefinementLimitJoinPartiallyOverlappingCoarseCells)
{
    for (uint32_t const resolution : {256U, 512U}) {
        auto const world = presentationWorld(resolution, true);
        std::array<uint32_t, 2> const columns =
            resolution == 256U ? std::array{4'052U, 43U} : std::array{4'054U, 41U};
        for (uint32_t const x : columns) {
            client::TerrainLodActualColumn actual{.x = x, .y = 0U, .meshes = {}, .faces = {}};
            for (auto& edge : actual.edge_heights) {
                edge.fill(600U);
            }
            auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, std::span{&actual, 1U});
            ASSERT_EQ(scene->actual_column_count, 1U);
            EXPECT_EQ(unmatchedActualTopEdges(*scene, std::span{&actual, 1U}), 0U) << "actual x=" << x;
        }
    }
}

TEST(TerrainLodPresentationTest, RefinementLimitHasUpwardCoverageAndMatchingSurfaceLimits)
{
    auto const world = presentationWorld(256U, true);
    client::TerrainLodActualColumn actual{.x = 4'052U, .y = 0U, .meshes = {}, .faces = {}};
    for (auto& edge : actual.edge_heights) {
        edge.fill(600U);
    }
    actual.faces.push_back({.x = 64'832, .y = 0, .z = 599, .direction = 5U,
        .u_extent = 16U, .v_extent = 16U, .material_id = static_cast<uint32_t>(shared::Block::Grass)});
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, std::span{&actual, 1U});
    for (int32_t y = 0; y < 40; ++y) {
        for (int32_t x = 64'760; x < 64'880; ++x) {
            EXPECT_TRUE(topHeightAt(*scene, x + 0.371, y + 0.619).has_value()) << x << ',' << y;
        }
    }
    for (uint32_t y = 0U; y < 16U; ++y) {
        auto const outside = topHeightAt(*scene, 64'832.0 - 1e-5, y + 0.371);
        auto const inside = topHeightAt(*scene, 64'832.0 + 1e-5, y + 0.371);
        ASSERT_TRUE(outside.has_value());
        ASSERT_TRUE(inside.has_value());
        EXPECT_NEAR(*outside, *inside, 0.001) << "shared actual/coarse limit y=" << y;
    }
    for (auto const& face : scene->faces) {
        if (face.direction != 6U) {
            continue;
        }
        auto const v = trianglePoints(face);
        EXPECT_GE((v[1][0] - v[0][0]) * (v[2][1] - v[0][1])
            - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]), 0.0);
    }
}

TEST(TerrainLodPresentationTest, CanonicalOwnerRegionRetainedColumnJoinsNegativeRefinementLimit)
{
    auto const world = std::make_shared<shared::TerrainWorld const>(generation::prepareAtlas({}));
    auto const actual = canonicalActualColumn(*world, 3'780U, 365U);
    auto const scene = client::buildTerrainLodScene(*world, 60'490U, 6'508U, std::span{&actual, 1U});
    EXPECT_EQ(unmatchedActualTopEdges(*scene, std::span{&actual, 1U}), 0U);
    for (uint32_t x = 0U; x < 16U; ++x) {
        auto const outside = topHeightAt(*scene, 60'480.0 + x + 0.371, 5'824.0 - 1e-5);
        auto const inside = topHeightAt(*scene, 60'480.0 + x + 0.371, 5'824.0 + 1e-5);
        ASSERT_TRUE(outside.has_value());
        ASSERT_TRUE(inside.has_value());
        EXPECT_NEAR(*outside, *inside, 0.001) << "owner-region coarse/fine join x=" << x;
    }
}

TEST(TerrainLodPresentationTest, RefinementLimitSteppedProfileHasCompleteVerticalCaps)
{
    auto const world = presentationWorld(256U, true);
    client::TerrainLodActualColumn actual{.x = 4'052U, .y = 0U, .meshes = {}, .faces = {}};
    for (auto& edge : actual.edge_heights) {
        for (uint32_t i = 0U; i < 16U; ++i) {
            edge[i] = static_cast<uint16_t>(600U + i / 4U);
        }
    }
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, std::span{&actual, 1U});
    for (uint32_t y : {4U, 8U, 12U}) {
        std::vector<std::array<double, 2>> intervals;
        for (auto const& face : scene->faces) {
            if (face.direction != 6U) {
                continue;
            }
            auto const v = trianglePoints(face);
            for (uint32_t i = 0U; i < 3U; ++i) {
                auto const& a = v[i];
                auto const& b = v[(i + 1U) % 3U];
                if (a[0] == 64'832.0 && b[0] == a[0] && a[1] == y && b[1] == y) {
                    intervals.push_back({std::min(a[2], b[2]), std::max(a[2], b[2])});
                }
            }
        }
        std::sort(intervals.begin(), intervals.end());
        double covered = 599.0 + static_cast<double>(y) / 4.0;
        for (auto const& interval : intervals) {
            if (interval[0] <= covered) {
                covered = std::max(covered, interval[1]);
            }
        }
        EXPECT_GE(covered, 600.0 + static_cast<double>(y) / 4.0) << "vertical profile step y=" << y;
    }
}

TEST(TerrainLodPresentationTest, FullProfileAuthorityCapturesWithoutDenseChunks)
{
    auto const terrain = presentationWorld(256U);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (auto const offset :
         {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        auto const surface = terrain->sampleSurface({.x = 1 + offset[0], .y = 1 + offset[1], .z = 0});
        ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    }
    EXPECT_EQ(world.residentChunkCount(), 0U);
    auto const captured = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(captured.has_value());
    EXPECT_TRUE(captured->chunks.empty());
}

namespace {

using UnitAuthorityFace = std::array<int32_t, 5>;

std::set<UnitAuthorityFace> denseReferenceFaces(client::TerrainLodColumnInput const& captured)
{
    std::set<UnitAuthorityFace> result;
    auto const material = [&captured](uint32_t x, uint32_t y, int32_t z) {
        return z < 0 || z >= 1'024 ? shared::Block::Air
                            : captured.authority_profiles[y * 18U + x].materialAt(static_cast<uint16_t>(z));
    };
    for (uint32_t section = 0U; section < 64U; ++section) {
        shared::Chunk::Blocks blocks;
        for (uint32_t z = 0U; z < 16U; ++z) {
            for (uint32_t y = 0U; y < 16U; ++y) {
                for (uint32_t x = 0U; x < 16U; ++x) {
                    blocks[z * 256U + y * 16U + x] = material(x + 1U, y + 1U, static_cast<int32_t>(section * 16U + z));
                }
            }
        }
        shared::Chunk chunk{
            {static_cast<int32_t>(captured.x), static_cast<int32_t>(captured.y), static_cast<int32_t>(section)},
            blocks};
        shared::ChunkNeighborFaces neighbors;
        for (uint32_t direction = 0U; direction < 6U; ++direction) {
            std::array<shared::Block, 256> face;
            for (uint32_t v = 0U; v < 16U; ++v) {
                for (uint32_t u = 0U; u < 16U; ++u) {
                    if (direction < 2U) {
                        face[v * 16U + u] =
                            material(direction == 0U ? 0U : 17U, u + 1U, static_cast<int32_t>(section * 16U + v));
                    } else if (direction < 4U) {
                        face[v * 16U + u] =
                            material(u + 1U, direction == 2U ? 0U : 17U, static_cast<int32_t>(section * 16U + v));
                    } else {
                        face[v * 16U + u] =
                            material(u + 1U, v + 1U, static_cast<int32_t>(section * 16U) + (direction == 4U ? -1 : 16));
                    }
                }
            }
            neighbors.faces[direction] = face;
        }
        shared::ChunkMesher mesher;
        for (auto const& face : mesher.update(chunk, neighbors).faces) {
            result.insert(
                {static_cast<int32_t>(captured.x * 16U + face.local_origin.x),
                 static_cast<int32_t>(captured.y * 16U + face.local_origin.y),
                 static_cast<int32_t>(section * 16U + face.local_origin.z),
                 static_cast<int32_t>(face.direction),
                 static_cast<int32_t>(face.material)});
        }
    }
    return result;
}

std::set<UnitAuthorityFace> profileSceneFaces(client::TerrainLodScene const& scene)
{
    std::set<UnitAuthorityFace> result;
    for (auto const& face : scene.faces) {
        if (face.direction >= 6U) {
            continue;
        }
        for (uint32_t v = 0U; v < face.v_extent; ++v) {
            for (uint32_t u = 0U; u < face.u_extent; ++u) {
                result.insert(
                    {face.x + static_cast<int32_t>(face.direction < 2U ? 0U : u),
                     face.y + static_cast<int32_t>(face.direction < 2U ? u : face.direction < 4U ? 0U : v),
                     face.z + static_cast<int32_t>(face.direction < 4U ? v : 0U),
                     static_cast<int32_t>(face.direction),
                     static_cast<int32_t>(face.material_id)});
            }
        }
    }
    return result;
}

TEST(TerrainLodPresentationTest, AuthoritySourcesStayCurrentAcrossCanonicalDenseCacheChurnAndDetectEdits)
{
    auto const terrain = presentationWorld(256U);
    auto const identity = terrain->identity();
    auto const surface = terrain->sampleSurface({.x = 4, .y = 5, .z = 0});
    shared::SparseWorld world({.seed = identity.seed, .max_resident_chunks = 2U, .revision = identity.revision},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const original_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(original_identity.has_value());
    auto const& original = original_identity.value();
    client::TerrainLodScene scene;
    scene.authority_sources.push_back({surface.coordinate, original.identity, original.revision, original.content_hash});
    ASSERT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    ASSERT_NE(world.ensureChunk({4, 5, 24}), nullptr);
    auto const admitted_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(admitted_identity.has_value());
    auto const& admitted = admitted_identity.value();
    EXPECT_EQ(admitted.content_hash, original.content_hash);
    EXPECT_EQ(admitted.revision, original.revision);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    client::TerrainLodScene resident_scene;
    resident_scene.authority_sources.push_back({surface.coordinate, admitted.identity, admitted.revision, admitted.content_hash});
    ASSERT_TRUE(world.evict({4, 5, 24}));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(resident_scene, world));
    auto const retained_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(retained_identity.has_value());
    EXPECT_EQ(retained_identity->content_hash, original.content_hash);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    ASSERT_NE(world.ensureChunk({4, 5, 24}), nullptr);
    ASSERT_TRUE(world.setBlock({64, 80, 385}, shared::Block::Snow));
    auto const edited_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(edited_identity.has_value());
    EXPECT_NE(edited_identity->content_hash, original.content_hash);
    EXPECT_FALSE(client::terrainLodSourcesCurrent(scene, world));
    auto const& edited = edited_identity.value();
    scene.authority_sources[0] = {surface.coordinate, edited.identity, edited.revision, edited.content_hash};
    ASSERT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    ASSERT_NE(world.ensureChunk({4, 5, 25}), nullptr);
    auto const final_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(final_identity.has_value());
    EXPECT_EQ(final_identity->content_hash, edited.content_hash);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    ASSERT_TRUE(world.evict({4, 5, 25}));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
}

} // namespace

TEST(TerrainLodPresentationTest, ProfileFacesMatchDenseMesherAndDenseEditCavityExactly)
{
    auto const terrain = presentationWorld(256U, true);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (auto const offset :
         {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        ASSERT_TRUE(world.publishAuthoritativeSurface(
            terrain->sampleSurface({.x = 1 + offset[0], .y = 1 + offset[1], .z = 0})));
    }
    for (uint32_t edited = 0U; edited < 4U; ++edited) {
        if (edited == 1U) {
            ASSERT_TRUE(world.setBlock({17, 17, 20}, shared::Block::Air));
        } else if (edited == 2U) {
            ASSERT_TRUE(world.setBlock({17, 17, 900}, shared::Block::Snow));
        } else if (edited == 3U) {
            ASSERT_TRUE(world.setBlock({17, 17, 0}, shared::Block::Air));
        }
        auto capture = client::captureTerrainLodColumn(world, 1U, 1U);
        ASSERT_TRUE(capture.has_value());
        EXPECT_TRUE(capture->chunks.empty());
        EXPECT_EQ(capture->authority_sources.size(), 5U);
        EXPECT_TRUE(client::terrainLodColumnSourcesCurrent(*capture, world));
        auto const input = std::make_shared<client::TerrainLodColumnInput const>(std::move(*capture));
        std::array const columns{input};
        auto const scene = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, columns);
        EXPECT_EQ(profileSceneFaces(*scene), denseReferenceFaces(*input));
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, world));
        ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({.x = 50, .y = 50, .z = 0})));
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, world));
        ASSERT_TRUE(world.setBlock({18, 17, static_cast<int64_t>(21U + edited)}, shared::Block::Air));
        EXPECT_FALSE(client::terrainLodSourcesCurrent(*scene, world));
        EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*input, world));
    }
}

TEST(TerrainLodPresentationTest, OrdinaryProfileBottomUsesOneExactTexturedRectangleIncludingPeriodicEdge)
{
    auto const terrain = presentationWorld(256U);
    for (uint32_t const tile_x : {1U, 4'095U}) {
        shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
        ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
        for (auto const offset :
             {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
            int32_t const x = (static_cast<int32_t>(tile_x) + offset[0] + 4'096) % 4'096;
            ASSERT_TRUE(world.publishAuthoritativeSurface(
                terrain->sampleSurface({.x = x, .y = 1 + offset[1], .z = 0})));
        }
        auto capture = client::captureTerrainLodColumn(world, tile_x, 1U);
        ASSERT_TRUE(capture.has_value());
        auto const input = std::make_shared<client::TerrainLodColumnInput const>(std::move(*capture));
        std::array const columns{input};
        auto const scene = client::buildTerrainLodSceneFromColumns(*terrain, tile_x * 16U, 16U, columns);
        EXPECT_EQ(profileSceneFaces(*scene), denseReferenceFaces(*input));
        std::vector<client::StoneFaceInstance> bottoms;
        for (auto const& face : scene->faces) {
            if (face.direction == 4U && face.z == 0) {
                bottoms.push_back(face);
            }
        }
        ASSERT_EQ(bottoms.size(), 1U);
        auto const& bottom = bottoms.front();
        EXPECT_EQ(bottom.x, static_cast<int32_t>(tile_x * 16U));
        EXPECT_EQ(bottom.y, 16);
        EXPECT_EQ(bottom.u_extent, 16U);
        EXPECT_EQ(bottom.v_extent, 16U);
        EXPECT_EQ(bottom.material_id, static_cast<uint32_t>(shared::Block::Bedrock));
    }
}
