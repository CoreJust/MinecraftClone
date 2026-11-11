#include <client/RiverBedMesh.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/TerrainLodDrawIndex.hpp>
#include <client/render/TerrainLodUploadPlan.hpp>
#include <client/render/VulkanFrustum.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <generation/Generation.hpp>
#include <generation/V10Provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace client::detail {
void prepareTerrainLodMeshNormals(client::TerrainLodScene& scene, std::stop_token const& cancellation,
    std::span<client::StoneFaceInstance> staged_faces = {});

double terrainLodChildHeightError(
    std::array<int32_t, 4> const& corners, shared::TerrainLodArtifact const& child);


std::optional<RiverBedMesh> tryBuildRiverStarMesh(
    std::array<double, 2> minimum,
    std::array<double, 2> maximum,
    std::span<std::array<double, 2> const> anchors,
    std::span<std::array<uint32_t, 2> const> edges,
    std::function<double(double, double)> const& height);

bool nodeRiverConstraintCrossings(
    std::span<std::array<double, 2> const> points,
    std::span<std::array<uint32_t, 2> const> edges,
    std::vector<std::vector<uint32_t>>& paths);

bool isRiverConstraintIntersection(
    std::span<std::array<double, 2> const> points,
    std::span<std::array<uint32_t, 2> const> edges,
    std::array<double, 2> point);

void resampleRiverRenderHeights(
    std::span<std::array<double, 3> const> original,
    std::span<std::array<double, 3>> positions,
    std::function<double(double, double)> const& height,
    TerrainLodRiverGeometryAudit& audit);

} // namespace client::detail

namespace {

std::vector<client::StoneFaceInstance> logicalFaces(client::TerrainLodScene const& scene)
{
    std::vector<client::StoneFaceInstance> faces(client::terrainLodSceneFaceCount(scene));
    for (client::TerrainLodDrawGroup const& group : scene.groups) {
        auto const group_faces = client::terrainLodGroupFaces(group);
        std::copy(group_faces.begin(), group_faces.end(), faces.begin() + group.range.first_instance);
    }
    return faces;
}

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

shared::TerrainWorld const& v21PresentationWorld()
{
    static auto const world = [] {
        generation::GenerationConfiguration configuration;
        configuration.seed = 42U;
        configuration.resolution = 256U;
        configuration.model_tag = generation::TerrainModelTag::V21;
        return std::make_unique<shared::TerrainWorld>(generation::prepareAtlas(configuration));
    }();
    return *world;
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
    ASSERT_GT(client::terrainLodSceneFaceCount(*scene), 0U);
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
    ASSERT_NE(scene->draw_index, nullptr);
    for (client::TerrainLodDrawGroup const& group : scene->groups) {
        EXPECT_LE(static_cast<uint64_t>(group.range.first_instance) + group.range.instance_count,
            client::terrainLodSceneFaceCount(*scene));
    }

    client::TerrainLodDrawIndex rebuilt_index;
    rebuilt_index.reset(scene->groups);
    for (glm::dvec3 const camera : {glm::dvec3{-32'768.0, -32'768.0, 1'024.0},
             glm::dvec3{0.0, 0.0, 1'024.0}, glm::dvec3{65'536.0, 65'536.0, 1'024.0}}) {
        client::VulkanFrustum const frustum{glm::mat4{1.0F}};
        std::vector<uint32_t> prepared_visible;
        std::vector<uint32_t> rebuilt_visible;
        static_cast<void>(scene->draw_index->collect(frustum, camera, prepared_visible));
        static_cast<void>(rebuilt_index.collect(frustum, camera, rebuilt_visible));
        EXPECT_EQ(prepared_visible, rebuilt_visible);
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
    for (client::StoneFaceInstance const& face : logicalFaces(*scene)) {
        if (face.faceDirection() != client::StoneFaceInstance::TERRAIN_TRIANGLE) {
            continue;
        }
        auto const dx = [](uint32_t const packed) { return static_cast<int32_t>(static_cast<int16_t>(packed)); };
        auto const dy = [](uint32_t const packed) { return static_cast<int32_t>(static_cast<int16_t>(packed >> 16U)); };
        auto const wrap_coordinate = [](int32_t const coordinate) {
            return static_cast<int32_t>(static_cast<uint32_t>(coordinate) & 0xffffU);
        };
        auto const heights = face.terrainTriangleHeights();
        std::array<std::array<int32_t, 3>, 3> const vertices{
            std::array{
                wrap_coordinate(face.x),
                wrap_coordinate(face.y),
                static_cast<int32_t>(heights[0])},
            std::array{
                wrap_coordinate(face.x + dx(face.u_extent)),
                wrap_coordinate(face.y + dy(face.u_extent)),
                static_cast<int32_t>(heights[1])},
            std::array{
                wrap_coordinate(face.x + dx(face.v_extent)),
                wrap_coordinate(face.y + dy(face.v_extent)),
                static_cast<int32_t>(face.terrainTriangleHeights()[2])},
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
    EXPECT_TRUE(std::ranges::any_of(logicalFaces(*scene), [](client::StoneFaceInstance const& face) {
        return face.x == 0 && face.y == 0 && face.z == 399 && face.faceDirection() == 5U;
    }));
    EXPECT_FALSE(std::ranges::any_of(logicalFaces(*scene), [](client::StoneFaceInstance const& face) {
        return face.faceDirection() < 4U && face.z == 0 && face.v_extent == 400U;
    }));
    EXPECT_FALSE(std::ranges::any_of(logicalFaces(*scene), [](client::StoneFaceInstance const& face) {
        return face.faceDirection() == 4U && face.z == 0 && face.u_extent == 16U && face.v_extent == 16U;
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
    for (client::StoneFaceInstance const& face : logicalFaces(*scene)) {
        if (face.faceDirection() == 6U) {
            auto const heights = face.terrainTriangleHeights();
            slope =
                slope || (heights[0]) != (heights[1]) || (heights[0]) != (face.terrainTriangleHeights()[2]);
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
        for (client::StoneFaceInstance const& face : logicalFaces(*scene)) {
            ASSERT_EQ(face.faceDirection(), client::StoneFaceInstance::TERRAIN_TRIANGLE);
            auto const heights = face.terrainTriangleHeights();
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
                    heights[0],
                    temperature
                ),
                vertex_key(
                    face.x + dx(face.u_extent),
                    face.y + dy(face.u_extent),
                    heights[1],
                    static_cast<int16_t>(face.material_id)),
                vertex_key(
                    face.x + dx(face.v_extent),
                    face.y + dy(face.v_extent),
                    face.terrainTriangleHeights()[2],
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
    for (auto const& face : logicalFaces(*scene)) {
        ASSERT_EQ(face.faceDirection(), 6U);
        ASSERT_NE(face.reserved_1 & 0x8000'0000U, 0U);
        EXPECT_EQ(static_cast<int16_t>(face.material_id & 0xffffU), 12 * 32);
        EXPECT_EQ(static_cast<int16_t>(face.material_id >> 16U), 12 * 32);
    }
}

TEST(TerrainLodPresentationTest, V21LocalDryHillCarriesItsOwnLodMaterialsInsteadOfCoarseOceanCategories)
{
    auto const& world = v21PresentationWorld();
    auto const scene = client::buildTerrainLodScene(world, 35'360U, 33'230U);
    // These global-grid corners are submerged, but the enclosed native L2 hill is dry grass.
    EXPECT_LT(world.sampleSurfaceHeight(35'328U, 32'768U), 384U);
    EXPECT_LT(world.sampleSurfaceHeight(35'584U, 33'024U), 384U);
    bool found_hill = false;
    for (auto const& face : logicalFaces(*scene)) {
        if (face.faceDirection() != client::StoneFaceInstance::TERRAIN_TRIANGLE
            || face.x != 35'416 || face.y != 32'984) {
            continue;
        }
        found_hill = true;
        ASSERT_EQ(face.reserved_1 & 0xc000'0000U, 0x4000'0000U);
        for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
            EXPECT_EQ((face.material_id >> (vertex * 8U)) & 0xffU,
                static_cast<uint32_t>(shared::Block::Grass));
        }
    }
    ASSERT_TRUE(found_hill);
}

TEST(TerrainLodPresentationTest, MaterialTrianglesPreserveGeometryAndEveryPackedCategory)
{
    std::array<std::array<int32_t, 3>, 3> const vertices{{{64, 32, 65'535}, {80, 32, 12'288}, {64, 48, 32}}};
    auto const face = client::StoneFaceInstance::fromTerrainMaterialTriangle(vertices,
        {shared::Block::Sand, shared::Block::Snow, shared::Block::Ice});
    auto const explicit_face = client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Sand);
    EXPECT_EQ(face.x, explicit_face.x);
    EXPECT_EQ(face.y, explicit_face.y);
    EXPECT_EQ(face.z, explicit_face.z);
    EXPECT_EQ(face.u_extent, explicit_face.u_extent);
    EXPECT_EQ(face.v_extent, explicit_face.v_extent);
    EXPECT_EQ(face.terrainTriangleHeights()[2], explicit_face.reserved_1);
    EXPECT_EQ(face.reserved_1 & 0xc000'0000U, 0x4000'0000U);
    EXPECT_EQ(face.material_id & 0xffU, 7U);
    EXPECT_EQ((face.material_id >> 8U) & 0xffU, 5U);
    EXPECT_EQ((face.material_id >> 16U) & 0xffU, 6U);
}

TEST(TerrainLodPresentationTest, MaterialTrianglesRejectInvalidCategoriesAtEveryVertex)
{
    std::array<std::array<int32_t, 3>, 3> const vertices{{{0, 0, 32}, {16, 0, 32}, {0, 16, 32}}};
    for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
        for (shared::Block const invalid : {shared::Block::Air, static_cast<shared::Block>(255U)}) {
            std::array materials{shared::Block::Grass, shared::Block::Stone, shared::Block::Sand};
            materials[vertex] = invalid;
            EXPECT_THROW(static_cast<void>(client::StoneFaceInstance::fromTerrainMaterialTriangle(vertices, materials)),
                std::invalid_argument) << "vertex " << vertex;
        }
    }
}

TEST(TerrainLodPresentationTest, V21SharedVerticesKeepOneCategoryAcrossRefinementAndPeriodicSeam)
{
    auto const& world = v21PresentationWorld();
    for (auto const center : {std::array{0U, 0U}, std::array{35'360U, 33'230U}}) {
        auto const scene = client::buildTerrainLodScene(world, center[0], center[1]);
        std::map<std::array<int32_t, 3>, uint32_t> categories;
        uint32_t shared_vertices = 0U;
        for (auto const& face : logicalFaces(*scene)) {
            ASSERT_EQ(face.reserved_1 & 0xc000'0000U, 0x4000'0000U);
            auto const heights = face.terrainTriangleHeights();
            std::array<std::array<int32_t, 3>, 3> const vertices{{
                {face.x, face.y, static_cast<int32_t>(heights[0])},
                {face.x + static_cast<int16_t>(face.u_extent), face.y + static_cast<int16_t>(face.u_extent >> 16U),
                    static_cast<int32_t>(heights[1])},
                {face.x + static_cast<int16_t>(face.v_extent), face.y + static_cast<int16_t>(face.v_extent >> 16U),
                    static_cast<int32_t>(face.terrainTriangleHeights()[2])},
            }};
            for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
                auto key = vertices[vertex];
                key[0] &= 0xffff;
                key[1] &= 0xffff;
                uint32_t const category = (face.material_id >> (vertex * 8U)) & 0xffU;
                ASSERT_TRUE(shared::isSolidBlock(static_cast<shared::Block>(category)));
                auto const [found, inserted] = categories.emplace(key, category);
                if (!inserted) {
                    ASSERT_EQ(found->second, category) << "shared XY " << key[0] << ',' << key[1];
                    ++shared_vertices;
                }
            }
        }
        EXPECT_GT(shared_vertices, 0U);
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
    for (auto const& face : logicalFaces(*scene)) {
        if (face.faceDirection() != 6U || (face.reserved_1 & 0x8000'0000U) == 0U) {
            continue;
        }
        auto const h = face.terrainTriangleHeights();
        if ((h[0]) == 600U * 32U && (h[1]) == 600U * 32U) {
            ++joined;
        }
        if ((h[1]) == 600U * 32U && (face.terrainTriangleHeights()[2]) == 600U * 32U) {
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
    for (auto const& face : logicalFaces(scene)) {
        if (face.faceDirection() != 6U || (face.reserved_1 & 0x8000'0000U) == 0U) {
            continue;
        }
        auto const heights = face.terrainTriangleHeights();
        std::array<std::array<uint32_t, 3>, 3> const v{
            std::array{static_cast<uint32_t>(face.x), static_cast<uint32_t>(face.y), heights[0]},
            std::array{
                static_cast<uint32_t>(face.x + static_cast<int16_t>(face.u_extent)),
                static_cast<uint32_t>(face.y + static_cast<int16_t>(face.u_extent >> 16U)),
                heights[1]},
            std::array{
                static_cast<uint32_t>(face.x + static_cast<int16_t>(face.v_extent)),
                static_cast<uint32_t>(face.y + static_cast<int16_t>(face.v_extent >> 16U)),
                face.terrainTriangleHeights()[2]},
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
        for (auto const& face : logicalFaces(*scene)) {
            if (face.faceDirection() != 6U) {
                continue;
            }
            auto const heights = face.terrainTriangleHeights();
            std::array const v{
                vertex(face.x, face.y, heights[0]),
                vertex(
                    face.x + static_cast<int16_t>(face.u_extent),
                    face.y + static_cast<int16_t>(face.u_extent >> 16U),
                    heights[1]),
                vertex(
                    face.x + static_cast<int16_t>(face.v_extent),
                    face.y + static_cast<int16_t>(face.v_extent >> 16U),
                    face.terrainTriangleHeights()[2]),
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
    auto const heights = face.terrainTriangleHeights();
    double const xy_scale = (face.direction & client::StoneFaceInstance::TERRAIN_TRIANGLE_XY_Q5) != 0U ? 32.0 : 1.0;
    return {{
        {double(face.x) / xy_scale, double(face.y) / xy_scale, double(heights[0]) / 32.0},
        {(face.x + double(static_cast<int16_t>(face.u_extent))) / xy_scale,
            (face.y + double(static_cast<int16_t>(face.u_extent >> 16U))) / xy_scale, double(heights[1]) / 32.0},
        {(face.x + double(static_cast<int16_t>(face.v_extent))) / xy_scale,
            (face.y + double(static_cast<int16_t>(face.v_extent >> 16U))) / xy_scale, double(heights[2]) / 32.0},
    }};
}

struct TopTriangleAt final {
    double height_m;
    std::array<shared::Block, 3> materials;
};

std::vector<TopTriangleAt> topTrianglesAt(client::TerrainLodScene const& scene, double const x, double const y)
{
    std::vector<TopTriangleAt> result;
    double highest = -std::numeric_limits<double>::infinity();
    for (auto const& group : scene.groups) {
        if (x < group.origin_x || x >= group.origin_x + group.extent
            || y < group.origin_y || y >= group.origin_y + group.extent) {
            continue;
        }
        for (client::StoneFaceInstance const& face : client::terrainLodGroupFaces(group)) {
            if (face.faceDirection() != client::StoneFaceInstance::TERRAIN_TRIANGLE) {
                continue;
            }
            auto const points = trianglePoints(face);
            double const area = (points[1][0] - points[0][0]) * (points[2][1] - points[0][1])
                - (points[2][0] - points[0][0]) * (points[1][1] - points[0][1]);
            if (std::abs(area) <= 1e-12) {
                continue;
            }
            std::array<double, 3> const weights{
                ((points[1][0] - x) * (points[2][1] - y) - (points[2][0] - x) * (points[1][1] - y)) / area,
                ((points[2][0] - x) * (points[0][1] - y) - (points[0][0] - x) * (points[2][1] - y)) / area,
                0.0,
            };
            std::array<double, 3> const barycentric{weights[0], weights[1], 1.0 - weights[0] - weights[1]};
            if (std::ranges::any_of(barycentric, [](double const weight) { return weight < -1e-9; })) {
                continue;
            }
            double const height = barycentric[0] * points[0][2]
                + barycentric[1] * points[1][2] + barycentric[2] * points[2][2];
            if (height > highest + 1e-9) {
                highest = height;
                result.clear();
            }
            if (std::abs(height - highest) > 1e-9) {
                continue;
            }
            result.push_back({height, {
                static_cast<shared::Block>(face.material_id & 0xffU),
                static_cast<shared::Block>((face.material_id >> 8U) & 0xffU),
                static_cast<shared::Block>((face.material_id >> 16U) & 0xffU),
            }});
        }
    }
    return result;
}

std::optional<double> topHeightAt(client::TerrainLodScene const& scene, double x, double y)
{
    for (auto const& group : scene.groups) {
        if (x < group.origin_x || x >= group.origin_x + group.extent
            || y < group.origin_y || y >= group.origin_y + group.extent) {
            continue;
        }
        for (client::StoneFaceInstance const& face : client::terrainLodGroupFaces(group)) {
            if (face.faceDirection() == 5U && x >= static_cast<double>(face.x)
                && x < static_cast<double>(face.x) + static_cast<double>(face.u_extent)
                && y >= static_cast<double>(face.y)
                && y < static_cast<double>(face.y) + static_cast<double>(face.v_extent)) {
                return face.z + 1.0;
            }
            if (face.faceDirection() != 6U) {
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

TEST(TerrainLodPresentationTest, V10FineRiverFanMaterialsFollowCanonicalWetCenterlineAndKeepDryControls)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world{atlas};
    auto const scene = client::buildTerrainLodScene(world, 34'784U, 30'212U);

    // Course 208, knot 30 follows the joined route selected for V10 fine terrain.
    constexpr double CENTER_X = 34'791.565'652'2;
    constexpr double CENTER_Y = 30'327.950'805'2;
    constexpr double WET_HIT_X = 34'791.445'329'25;
    constexpr double WET_HIT_Y = 30'328.223'364'86;
    auto const wet_center = generation::sampleV10WaterGeometry(*atlas, CENTER_X, CENTER_Y, 3U);
    ASSERT_TRUE(wet_center.river);
    ASSERT_GT(wet_center.elevation_m, 0.0);
    auto const wet_triangles = topTrianglesAt(*scene, WET_HIT_X, WET_HIT_Y);
    ASSERT_FALSE(wet_triangles.empty());
    EXPECT_TRUE(std::ranges::all_of(wet_triangles, [](TopTriangleAt const& triangle) {
        return std::ranges::all_of(triangle.materials, [](shared::Block const material) {
            return material == shared::Block::Dirt;
        });
    }));

    constexpr double DRY_BANK_X = 34'800.353'867;
    constexpr double DRY_BANK_Y = 30'331.705'165;
    auto const dry_bank = generation::sampleV10WaterGeometry(*atlas, DRY_BANK_X, DRY_BANK_Y, 3U);
    ASSERT_FALSE(dry_bank.river);
    auto const dry_triangles = topTrianglesAt(*scene, DRY_BANK_X, DRY_BANK_Y);
    ASSERT_FALSE(dry_triangles.empty());
    EXPECT_FALSE(std::ranges::any_of(dry_triangles, [](TopTriangleAt const& triangle) {
        return std::ranges::all_of(triangle.materials, [](shared::Block const material) {
            return material == shared::Block::Dirt;
        });
    }));

    auto const periodic = generation::sampleV10WaterGeometry(
        *atlas, CENTER_X + 65'536.0, CENTER_Y - 65'536.0, 3U);
    EXPECT_EQ(periodic.river, wet_center.river);
    EXPECT_NEAR(periodic.elevation_m, wet_center.elevation_m, 1e-9);
    EXPECT_NEAR(periodic.water_level_m, wet_center.water_level_m, 1e-9);

    auto const standing_lake = generation::sampleV10WaterGeometry(*atlas, 43'520.0, 9'216.0, 3U);
    EXPECT_TRUE(standing_lake.lake);
    EXPECT_GT(standing_lake.elevation_m, 0.0);
}

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
    for (auto const& face : logicalFaces(*scene)) {
        if (face.faceDirection() != 6U) {
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
        for (auto const& face : logicalFaces(*scene)) {
            if (face.faceDirection() != 6U) {
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

TEST(TerrainLodPresentationTest, GrassBankFilteringPreservesAuthoritativeDirtAndExcludesEditedSurfaces)
{
    auto const terrain = presentationWorld(256U);
    auto const identity = terrain->identity();
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    for (auto const offset :
         {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        shared::AuthoritativeTerrainTile tile{
            .coordinate = {1 + offset[0], 1 + offset[1], 0},
            .identity = identity,
        };
        tile.columns.fill({18U, 16U, 18U, shared::Block::Grass, shared::Block::Dirt});
        if (offset == std::array{0, 0}) {
            tile.columns[17U] = {24U, 19U, 24U, shared::Block::Grass, shared::Block::Dirt};
        }
        ASSERT_TRUE(world.publishAuthoritativeSurface(tile));
    }
    auto const bank_faces = [&] {
        auto captured = client::captureTerrainLodColumn(world, 1U, 1U);
        if (!captured) {
            ADD_FAILURE() << "Missing canonical column capture";
            return std::vector<client::StoneFaceInstance>{};
        }
        std::array const columns{std::make_shared<client::TerrainLodColumnInput const>(std::move(*captured))};
        auto const scene = client::buildTerrainLodColumnPatch(identity, columns);
        std::vector<client::StoneFaceInstance> result;
        for (auto const& face : logicalFaces(*scene)) {
            if (face.x == 17 && face.y == 17 && face.faceDirection() == 0U
                && face.material_id == static_cast<uint32_t>(shared::Block::Dirt)) {
                result.push_back(face);
            }
        }
        return result;
    };
    auto const before = bank_faces();
    ASSERT_EQ(before.size(), 2U);
    EXPECT_EQ(before[0].z, 19);
    EXPECT_EQ(before[0].v_extent, 3U);
    EXPECT_EQ(before[0].reserved_1, 0U);
    EXPECT_EQ(before[1].z, 22);
    EXPECT_EQ(before[1].v_extent, 2U);
    EXPECT_EQ(before[1].reserved_1, client::StoneFaceInstance::VOXEL_GRASS_BANK);
    EXPECT_EQ(world.queryBlock({17, 17, 23}).block, shared::Block::Dirt);

    ASSERT_TRUE(world.applyEditDelta({17, 17, 24}, shared::Block::Air, 0U, 1U, 1U));
    auto const after = bank_faces();
    ASSERT_FALSE(after.empty());
    EXPECT_TRUE(std::ranges::all_of(after, [](auto const& face) { return face.reserved_1 == 0U; }));
}

namespace {

using UnitAuthorityFace = std::array<int32_t, 5>;

std::set<UnitAuthorityFace> denseReferenceFaces(client::TerrainLodColumnInput const& captured)
{
    std::set<UnitAuthorityFace> result;
    auto const material = [&captured](uint32_t x, uint32_t y, int32_t z) {
        return z < 0 || z >= static_cast<int32_t>(shared::WorldExtent::DEPTH) ? shared::Block::Air
                            : captured.authority_profiles[y * 18U + x].materialAt(static_cast<uint16_t>(z));
    };
    for (uint32_t section = 0U; section < shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH; ++section) {
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
    for (auto const& face : logicalFaces(scene)) {
        if (face.faceDirection() >= 6U) {
            continue;
        }
        for (uint32_t v = 0U; v < face.v_extent; ++v) {
            for (uint32_t u = 0U; u < face.u_extent; ++u) {
                result.insert(
                    {face.x + static_cast<int32_t>(face.faceDirection() < 2U ? 0U : u),
                     face.y + static_cast<int32_t>(face.faceDirection() < 2U ? u : face.faceDirection() < 4U ? 0U : v),
                     face.z + static_cast<int32_t>(face.faceDirection() < 4U ? v : 0U),
                     static_cast<int32_t>(face.faceDirection()),
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
    scene.identity = identity;
    scene.authority_sources.push_back({
        surface.coordinate, original.identity, original.revision, original.content_hash, original.dense_content_hash,
    });
    ASSERT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    ASSERT_NE(world.ensureChunk({4, 5, 24}), nullptr);
    auto const admitted_identity = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(admitted_identity.has_value());
    auto const& admitted = admitted_identity.value();
    EXPECT_EQ(admitted.content_hash, original.content_hash);
    EXPECT_EQ(admitted.revision, original.revision);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
    client::TerrainLodScene resident_scene;
    resident_scene.identity = identity;
    resident_scene.authority_sources.push_back({
        surface.coordinate, admitted.identity, admitted.revision, admitted.content_hash, admitted.dense_content_hash,
    });
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
    scene.authority_sources[0] = {
        surface.coordinate, edited.identity, edited.revision, edited.content_hash, edited.dense_content_hash,
    };
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
        EXPECT_EQ(capture->chunks.size(), world.editedChunks().size());
        EXPECT_EQ(capture->authority_sources.size(), 5U);
        EXPECT_TRUE(client::terrainLodColumnSourcesCurrent(*capture, world));
        auto const input = std::make_shared<client::TerrainLodColumnInput const>(std::move(*capture));
        std::array const columns{input};
        auto const scene = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, columns);
        EXPECT_EQ(profileSceneFaces(*scene), denseReferenceFaces(*input));
        EXPECT_TRUE(scene->edit_sources.empty());
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, world));
        ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({.x = 50, .y = 50, .z = 0})));
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, world));
        ASSERT_TRUE(world.setBlock({18, 17, static_cast<int64_t>(21U + edited)}, shared::Block::Air));
        EXPECT_FALSE(client::terrainLodSourcesCurrent(*scene, world));
        EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*input, world));
    }
}

TEST(TerrainLodPresentationTest, ReplicatedSparseOverlayEditsRenderExactHoleAndPlacedBlockFaces)
{
    auto const terrain = presentationWorld(256U, true);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (auto const offset :
         {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        ASSERT_TRUE(world.publishAuthoritativeSurface(
            terrain->sampleSurface({.x = 1 + offset[0], .y = 1 + offset[1], .z = 0})));
    }

    shared::WorldCoordinate const hole{17, 17, 20};
    shared::WorldCoordinate const placed{17, 17, 900};
    auto const hole_base = world.queryBlock(hole);
    auto const placed_base = world.queryBlock(placed);
    ASSERT_TRUE(hole_base.block.has_value());
    ASSERT_TRUE(shared::isSolidBlock(*hole_base.block));
    ASSERT_EQ(placed_base.block, shared::Block::Air);
    ASSERT_TRUE(world.applyEditDelta(hole, shared::Block::Air, 0U, 1U, 1U));
    ASSERT_TRUE(world.applyEditDelta(placed, shared::Block::Snow, 0U, 1U, 1U));
    EXPECT_EQ(world.residentChunkCount(), 0U);

    auto const captured = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(captured.has_value());
    ASSERT_EQ(captured->chunks.size(), 2U);
    ASSERT_TRUE(client::terrainLodColumnSourcesCurrent(*captured, world));
    auto const input = std::make_shared<client::TerrainLodColumnInput const>(*captured);
    std::array const columns{input};
    auto const scene = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, columns);
    EXPECT_EQ(profileSceneFaces(*scene), denseReferenceFaces(*input));

    auto const assertCapturedChunk = [&](shared::WorldCoordinate const coordinate, shared::Block const material) {
        shared::ChunkCoordinate const chunk_coordinate = shared::WorldBounds::chunkCoordinate(coordinate);
        auto const effective = world.effectiveChunkSnapshot(chunk_coordinate);
        ASSERT_TRUE(effective.has_value());
        auto const input = std::ranges::find_if(captured->chunks, [&](client::TerrainLodChunkInput const& chunk) {
            return chunk.chunk.coordinate() == chunk_coordinate;
        });
        ASSERT_NE(input, captured->chunks.end());
        EXPECT_EQ(input->chunk.contentIdentity(), effective->contentIdentity());
        EXPECT_EQ(input->chunk.blockAt(shared::WorldBounds::blockCoordinate(coordinate)), material);
        for (auto const& face : input->neighbors.faces) {
            EXPECT_TRUE(face.has_value());
        }

        auto const source = std::ranges::find_if(captured->sources, [&](client::TerrainLodChunkSource const& value) {
            return value.coordinate == chunk_coordinate;
        });
        ASSERT_NE(source, captured->sources.end());
        EXPECT_EQ(source->identity, effective->contentIdentity());
        EXPECT_EQ(source->edit_revision, world.editChunkRevision(chunk_coordinate));

        shared::ChunkMesher mesher;
        shared::ChunkMesh const mesh = mesher.update(input->chunk, input->neighbors);
        shared::BlockCoordinate const local = shared::WorldBounds::blockCoordinate(coordinate);
        if (material == shared::Block::Air) {
            auto const below = world.queryBlock({coordinate.x, coordinate.y, coordinate.z - 1});
            ASSERT_TRUE(below.block.has_value());
            ASSERT_TRUE(shared::isSolidBlock(*below.block));
            EXPECT_TRUE(std::ranges::any_of(mesh.faces, [&](shared::MeshFace const& face) {
                return face.local_origin.x == local.x && face.local_origin.y == local.y
                    && face.local_origin.z == local.z - 1U
                    && face.direction == shared::FaceDirection::PositiveZ && face.material == *below.block;
            }));
        } else {
            EXPECT_TRUE(std::ranges::any_of(mesh.faces, [&](shared::MeshFace const& face) {
                return face.local_origin == local && face.direction == shared::FaceDirection::PositiveZ
                    && face.material == material;
            }));
        }
    };
    assertCapturedChunk(hole, shared::Block::Air);
    assertCapturedChunk(placed, shared::Block::Snow);

    ASSERT_TRUE(world.applyEditDelta(hole, *hole_base.block, 1U, 2U, 2U));
    EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*captured, world));
}

TEST(TerrainLodPresentationTest, ExactReplacementMatchesFullSceneAfterAuthoritativeEdit)
{
    auto const terrain = presentationWorld(256U);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (auto const offset :
         {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        ASSERT_TRUE(world.publishAuthoritativeSurface(
            terrain->sampleSurface({.x = 1 + offset[0], .y = 1 + offset[1], .z = 0})));
    }
    auto before = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(before.has_value());
    shared::WorldCoordinate const placed{17, 17, 900};
    ASSERT_TRUE(world.applyEditDelta(placed, shared::Block::Snow, 0U, 1U, 1U));
    EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*before, world));
    auto captured = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(captured.has_value());
    std::array const columns{std::make_shared<client::TerrainLodColumnInput const>(*captured)};
    auto const full = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, columns);
    auto const patch = client::buildTerrainLodColumnPatch(terrain->identity(), columns);
    ASSERT_EQ(patch->groups.size(), 1U);
    EXPECT_EQ(patch->groups.front().extent, 16U);
    EXPECT_EQ(profileSceneFaces(*patch), profileSceneFaces(*full));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*patch, world));
    ASSERT_TRUE(world.applyEditDelta(placed, shared::Block::Air, 1U, 2U, 2U));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*patch, world));
    captured = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(captured.has_value());
    std::array const removed{std::make_shared<client::TerrainLodColumnInput const>(*captured)};
    auto const removal = client::buildTerrainLodColumnPatch(terrain->identity(), removed);
    EXPECT_EQ(profileSceneFaces(*removal), denseReferenceFaces(*captured));
}

TEST(TerrainLodPresentationTest, WrappedNeighborOverlayEditInvalidatesCapturedChunkFaces)
{
    auto const terrain = presentationWorld(256U);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (auto const offset :
         {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        int32_t const x = (offset[0] + 4'096) % 4'096;
        ASSERT_TRUE(world.publishAuthoritativeSurface(
            terrain->sampleSurface({.x = x, .y = 1 + offset[1], .z = 0})));
    }

    shared::WorldCoordinate const center_block{0, 17, 900};
    shared::WorldCoordinate const wrapped_neighbor{65'535, 17, 900};
    ASSERT_EQ(world.queryBlock(center_block).block, shared::Block::Air);
    ASSERT_EQ(world.queryBlock(wrapped_neighbor).block, shared::Block::Air);
    ASSERT_TRUE(world.applyEditDelta(center_block, shared::Block::Stone, 0U, 1U, 1U));

    auto const before_neighbor_edit = client::captureTerrainLodColumn(world, 0U, 1U);
    ASSERT_TRUE(before_neighbor_edit.has_value());
    ASSERT_EQ(before_neighbor_edit->chunks.size(), 1U);
    ASSERT_TRUE(client::terrainLodColumnSourcesCurrent(*before_neighbor_edit, world));
    ASSERT_TRUE(world.applyEditDelta(wrapped_neighbor, shared::Block::Stone, 0U, 1U, 1U));
    EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*before_neighbor_edit, world));

    auto const after_neighbor_edit = client::captureTerrainLodColumn(world, 0U, 1U);
    ASSERT_TRUE(after_neighbor_edit.has_value());
    auto const center_chunk = std::ranges::find_if(after_neighbor_edit->chunks, [](client::TerrainLodChunkInput const& chunk) {
        return chunk.chunk.coordinate() == shared::ChunkCoordinate{.x = 0, .y = 1, .z = 56};
    });
    ASSERT_NE(center_chunk, after_neighbor_edit->chunks.end());
    ASSERT_TRUE(center_chunk->neighbors.faces[0].has_value());
    EXPECT_EQ((*center_chunk->neighbors.faces[0])[4U * 16U + 1U], shared::Block::Stone);
}

TEST(TerrainLodPresentationTest, SparseOverlayWithUnknownBaseCannotCaptureAsReady)
{
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.applyEditDelta({17, 17, 900}, shared::Block::Snow, 0U, 1U, 1U));
    EXPECT_FALSE(client::captureTerrainLodColumn(world, 1U, 1U).has_value());
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
        for (auto const& face : logicalFaces(*scene)) {
            if (face.faceDirection() == 4U && face.z == 0) {
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

TEST(TerrainLodPresentationTest, AuthoritySourceFastPathRetainsDenseAndAvailabilityFences)
{
    auto const terrain = presentationWorld(256U);
    auto const identity = terrain->identity();
    auto const surface = terrain->sampleSurface({.x = 4, .y = 5, .z = 0});
    shared::SparseWorld world({.seed = identity.seed, .max_resident_chunks = 2U, .revision = identity.revision},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.setTerrainIdentity(identity));
    ASSERT_TRUE(world.publishAuthoritativeSurface(surface));
    auto const captured = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(captured.has_value());
    uint64_t const revision = world.authoritativeSurfaceRevision();
    client::TerrainLodScene scene;
    scene.identity = identity;
    scene.authority_sources.push_back({
        surface.coordinate, captured->identity, captured->revision, captured->content_hash,
        captured->dense_content_hash,
    });
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));

    auto* const dense_chunk = world.ensureChunk({4, 5, 24});
    ASSERT_NE(dense_chunk, nullptr);
    auto const previous = dense_chunk->blockAt({0U, 0U, 0U});
    ASSERT_TRUE(previous.has_value());
    shared::Block const replacement = *previous == shared::Block::Air ? shared::Block::Snow : shared::Block::Air;
    ASSERT_TRUE(dense_chunk->setBlock({0U, 0U, 0U}, replacement));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), revision);
    EXPECT_FALSE(client::terrainLodSourcesCurrent(scene, world));

    auto const changed = world.authoritativeTileIdentity(surface.coordinate);
    ASSERT_TRUE(changed.has_value());
    scene.authority_sources[0] = {
        surface.coordinate, changed->identity, changed->revision, changed->content_hash, changed->dense_content_hash,
    };
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));

    std::array<shared::EditedChunkRevision, 1U> missing_requirements{{{{4, 5, 24}, 1U}}};
    ASSERT_TRUE(world.setEditedChunkRequirements(surface.coordinate, missing_requirements));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), revision);
    EXPECT_FALSE(client::terrainLodSourcesCurrent(scene, world));

    std::array<shared::EditedChunkRevision, 0U> no_requirements{};
    ASSERT_TRUE(world.setEditedChunkRequirements(surface.coordinate, no_requirements));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, world));
}

TEST(TerrainLodPresentationTest, SourceChecksRejectMixedAuthorityIdentities)
{
    auto const terrain = presentationWorld(256U);
    auto const replacement_terrain = presentationWorld(512U);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (int32_t x = 0; x <= 2; ++x) {
        for (int32_t y = 0; y <= 2; ++y) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({.x = x, .y = y, .z = 0})));
        }
    }
    auto captured = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(captured.has_value());
    ASSERT_EQ(captured->authority_sources.size(), 5U);
    ASSERT_TRUE(client::terrainLodColumnSourcesCurrent(*captured, world));
    auto const input = std::make_shared<client::TerrainLodColumnInput const>(*captured);
    std::array const columns{input};
    auto scene = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, columns);
    ASSERT_EQ(scene->authority_sources.size(), 5U);
    ASSERT_TRUE(client::terrainLodSourcesCurrent(*scene, world));

    captured->authority_sources.back().identity = replacement_terrain->identity();
    EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*captured, world));
    auto const mixed_input = std::make_shared<client::TerrainLodColumnInput const>(std::move(*captured));
    std::array const mixed_columns{mixed_input};
    EXPECT_EQ(client::staleTerrainLodColumns(mixed_columns, world, terrain->identity()),
        (std::vector<client::TerrainLodColumnCoordinate>{{1U, 1U}}));
    client::TerrainLodScene mixed_scene = *scene;
    mixed_scene.authority_sources.back().identity = replacement_terrain->identity();
    EXPECT_FALSE(client::terrainLodSourcesCurrent(mixed_scene, world));
}

TEST(TerrainLodPresentationTest, FallbackSceneDeduplicatesEditDependenciesAndRetainsConflicts)
{
    static constexpr uint32_t COLUMN_COUNT = shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH;
    static constexpr uint32_t VERTICAL_CHUNK_COUNT = shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH;

    auto const terrain = presentationWorld(256U);
    auto const make_fallback_column = [](uint32_t const x, uint32_t const y) {
        static constexpr std::array OFFSETS{
            std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1},
        };
        auto column = std::make_shared<client::TerrainLodColumnInput>();
        column->x = x;
        column->y = y;
        column->edit_sources.reserve(OFFSETS.size() * VERTICAL_CHUNK_COUNT);
        for (auto const offset : OFFSETS) {
            int32_t const source_x = static_cast<int32_t>(
                (static_cast<int64_t>(x) + offset[0] + COLUMN_COUNT) % COLUMN_COUNT);
            int32_t const source_y = static_cast<int32_t>(
                (static_cast<int64_t>(y) + offset[1] + COLUMN_COUNT) % COLUMN_COUNT);
            for (uint32_t z = 0U; z < VERTICAL_CHUNK_COUNT; ++z) {
                column->edit_sources.push_back({{source_x, source_y, static_cast<int32_t>(z)}, 0U});
            }
        }
        return std::shared_ptr<client::TerrainLodColumnInput const>{std::move(column)};
    };
    auto const first = make_fallback_column(0U, 0U);
    auto const second = make_fallback_column(1U, 0U);
    std::array const inputs{first, second};
    auto const scene = client::buildTerrainLodSceneFromColumns(*terrain, 0U, 0U, inputs, {}, nullptr, {}, true);

    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    EXPECT_EQ(first->edit_sources.size(), 5U * VERTICAL_CHUNK_COUNT);
    EXPECT_EQ(second->edit_sources.size(), 5U * VERTICAL_CHUNK_COUNT);
    EXPECT_EQ(scene->edit_sources.size(), 8U * VERTICAL_CHUNK_COUNT);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, world));

    auto conflicted_second = std::make_shared<client::TerrainLodColumnInput>(*second);
    auto const conflicting_source = std::ranges::find(
        conflicted_second->edit_sources, client::TerrainLodEditSource{{0, 0, 0}, 0U});
    ASSERT_NE(conflicting_source, conflicted_second->edit_sources.end());
    conflicting_source->revision = 1U;
    std::array const conflicting_inputs{
        first, std::shared_ptr<client::TerrainLodColumnInput const>{std::move(conflicted_second)},
    };
    auto const conflicting_scene =
        client::buildTerrainLodSceneFromColumns(*terrain, 0U, 0U, conflicting_inputs, {}, nullptr, {}, true);
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*conflicting_scene, world));
}

TEST(TerrainLodPresentationTest, CapturedBatchValidationRefreshesSharedTileStateAndAvailability)
{
    auto const terrain = presentationWorld(256U);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (int32_t x = 0; x <= 3; ++x) {
        for (int32_t y = 0; y <= 2; ++y) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({.x = x, .y = y, .z = 0})));
        }
    }

    client::TerrainLodAuthorityBatch batch;
    std::array<std::shared_ptr<client::TerrainLodCapturedColumn const>, 2U> columns{
        client::captureTerrainLodColumnForBatch(world, 1U, 1U, batch),
        client::captureTerrainLodColumnForBatch(world, 2U, 1U, batch),
    };
    ASSERT_NE(columns[0], nullptr);
    ASSERT_NE(columns[1], nullptr);
    EXPECT_TRUE(client::staleCapturedTerrainLodColumns(columns, world, batch).empty());
    auto const raw = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(raw);
    auto const& batched = columns.front()->input();
    ASSERT_FALSE(raw->authority_profiles[19U].identity.configuration_identity.empty());
    ASSERT_EQ(batched.authority_profiles.size(), raw->authority_profiles.size());
    EXPECT_EQ(batched.authority_sources, raw->authority_sources);
    EXPECT_EQ(batched.edge_heights, raw->edge_heights);
    EXPECT_EQ(batched.edge_materials, raw->edge_materials);
    EXPECT_TRUE(std::ranges::all_of(batched.authority_profiles, [](auto const& profile) {
        return profile.identity.configuration_identity.empty();
    }));

    auto* const dense_chunk = world.ensureChunk({2, 1, 63});
    ASSERT_NE(dense_chunk, nullptr);
    auto const previous = dense_chunk->blockAt({0U, 0U, 0U});
    ASSERT_TRUE(previous.has_value());
    shared::Block const replacement = *previous == shared::Block::Air ? shared::Block::Snow : shared::Block::Air;
    ASSERT_TRUE(dense_chunk->setBlock({0U, 0U, 0U}, replacement));
    std::vector<client::TerrainLodColumnCoordinate> const stale_after_dense_edit{{1U, 1U}, {2U, 1U}};
    EXPECT_EQ(client::staleCapturedTerrainLodColumns(columns, world, batch), stale_after_dense_edit);

    columns = {
        client::captureTerrainLodColumnForBatch(world, 1U, 1U, batch),
        client::captureTerrainLodColumnForBatch(world, 2U, 1U, batch),
    };
    ASSERT_NE(columns[0], nullptr);
    ASSERT_NE(columns[1], nullptr);
    EXPECT_TRUE(client::staleCapturedTerrainLodColumns(columns, world, batch).empty());

    ASSERT_TRUE(world.setBlock({32, 16, 1000}, shared::Block::Snow));
    auto const stale_after_sparse_edit = client::staleCapturedTerrainLodColumns(columns, world, batch);
    EXPECT_NE(std::ranges::find(stale_after_sparse_edit, client::TerrainLodColumnCoordinate{2U, 1U}),
        stale_after_sparse_edit.end());
    uint64_t const authority_revision = world.authoritativeSurfaceRevision();
    columns = {
        client::captureTerrainLodColumnForBatch(world, 1U, 1U, batch),
        client::captureTerrainLodColumnForBatch(world, 2U, 1U, batch),
    };
    ASSERT_NE(columns[0], nullptr);
    ASSERT_NE(columns[1], nullptr);
    EXPECT_TRUE(client::staleCapturedTerrainLodColumns(columns, world, batch).empty());

    std::array<shared::EditedChunkRevision, 1U> missing_requirement{{{{2, 1, 63}, 1U}}};
    ASSERT_TRUE(world.setEditedChunkRequirements({2, 1, 0}, missing_requirement));
    EXPECT_EQ(world.authoritativeSurfaceRevision(), authority_revision);
    EXPECT_EQ(client::staleCapturedTerrainLodColumns(columns, world, batch), stale_after_dense_edit);
    std::array<shared::EditedChunkRevision, 0U> no_requirements{};
    ASSERT_TRUE(world.setEditedChunkRequirements({2, 1, 0}, no_requirements));
    EXPECT_TRUE(client::staleCapturedTerrainLodColumns(columns, world, batch).empty());

    auto const replacement_terrain = presentationWorld(512U);
    ASSERT_EQ(replacement_terrain->identity().seed, terrain->identity().seed);
    ASSERT_EQ(replacement_terrain->identity().revision, terrain->identity().revision);
    ASSERT_NE(replacement_terrain->identity(), terrain->identity());
    ASSERT_TRUE(world.setTerrainIdentity(replacement_terrain->identity()));
    for (int32_t x = 0; x <= 3; ++x) {
        for (int32_t y = 0; y <= 2; ++y) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(
                replacement_terrain->sampleSurface({.x = x, .y = y, .z = 0})));
        }
    }
    EXPECT_EQ(client::staleCapturedTerrainLodColumns(columns, world, batch),
        (std::vector<client::TerrainLodColumnCoordinate>{{1U, 1U}, {2U, 1U}}));
    auto const old_batch_columns = columns;
    EXPECT_EQ(client::captureTerrainLodColumnForBatch(world, 1U, 1U, batch), nullptr);
    batch.reset();
    columns = {
        client::captureTerrainLodColumnForBatch(world, 1U, 1U, batch),
        client::captureTerrainLodColumnForBatch(world, 2U, 1U, batch),
    };
    ASSERT_NE(columns[0], nullptr);
    ASSERT_NE(columns[1], nullptr);
    EXPECT_TRUE(client::staleCapturedTerrainLodColumns(columns, world, batch).empty());
    EXPECT_EQ(client::staleCapturedTerrainLodColumns(old_batch_columns, world, batch),
        (std::vector<client::TerrainLodColumnCoordinate>{{1U, 1U}, {2U, 1U}}));
}

TEST(TerrainLodPresentationTest, BatchedChunkOnlyValidationDoesNotRequireTerrainIdentity)
{
    auto const terrain = presentationWorld(256U);
    static constexpr shared::ChunkCoordinate COORDINATE{4, 5, 0};
    shared::SparseWorld world({.seed = 42U, .max_resident_chunks = 2U, .revision = 1U},
        shared::SparseWorldGenerationMode::MaterializedOnly);
    ASSERT_TRUE(world.publishMaterializedChunk(shared::Chunk{COORDINATE}, 1U, 42U));
    shared::Chunk const* const resident = world.residentChunk(COORDINATE);
    ASSERT_NE(resident, nullptr);

    client::TerrainLodColumnInput column;
    column.x = 4U;
    column.y = 5U;
    column.sources.push_back({COORDINATE, resident->contentIdentity(), world.editChunkRevision(COORDINATE)});
    std::array const columns{std::make_shared<client::TerrainLodColumnInput const>(std::move(column))};
    EXPECT_TRUE(client::staleTerrainLodColumns(columns, world, terrain->identity()).empty());

    shared::Chunk* const mutable_chunk = world.ensureChunk(COORDINATE);
    ASSERT_NE(mutable_chunk, nullptr);
    ASSERT_TRUE(mutable_chunk->setBlock({0U, 0U, 0U}, shared::Block::Snow));
    EXPECT_EQ(client::staleTerrainLodColumns(columns, world, terrain->identity()),
        (std::vector<client::TerrainLodColumnCoordinate>{{4U, 5U}}));
}

TEST(TerrainLodPresentationTest, UnrenderedCapturedColumnsDoNotInvalidateNearGroundScene)
{
    auto const terrain = presentationWorld(256U);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> columns;
    for (int32_t const center : {1, 100}) {
        for (auto const offset :
             {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface(
                {.x = center + offset[0], .y = 1 + offset[1], .z = 0})));
        }
        auto capture = client::captureTerrainLodColumn(world, static_cast<uint32_t>(center), 1U);
        ASSERT_TRUE(capture);
        columns.push_back(std::make_shared<client::TerrainLodColumnInput const>(std::move(*capture)));
    }
    auto const scene = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, columns);
    ASSERT_EQ(scene->actual_column_count, 1U);
    ASSERT_TRUE(client::terrainLodSourcesCurrent(*scene, world));
    ASSERT_TRUE(world.applyEditDelta({1'601, 17, 900}, shared::Block::Snow, 0U, 1U, 1U));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, world));
    ASSERT_TRUE(world.applyEditDelta({17, 17, 900}, shared::Block::Snow, 0U, 1U, 1U));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*scene, world));
}

TEST(TerrainLodPresentationTest, UpperWorldProfilesRenderExactTopWithoutALegacyCeilingFace)
{
    auto const terrain = presentationWorld(256U);
    auto input = std::make_shared<client::TerrainLodColumnInput>();
    input->x = 2'048U;
    input->y = 2'048U;
    auto blocks = std::make_shared<std::array<shared::Block, shared::WorldExtent::DEPTH>>();
    blocks->fill(shared::Block::Stone);
    (*blocks)[0] = shared::Block::Bedrock;
    (*blocks)[1'025] = shared::Block::Air;
    (*blocks)[2'047] = shared::Block::Sand;
    shared::AuthoritativeTerrainColumn profile;
    profile.highest_solid_z = 2'047U;
    profile.base_highest_solid_z = 399U;
    profile.ground_first_z = 395U;
    profile.ground_end_z = 399U;
    profile.top_material = shared::Block::Grass;
    profile.ground_material = shared::Block::Dirt;
    profile.dense_section_mask.set();
    profile.dense_blocks = blocks;
    input->authority_profiles.assign(18U * 18U, profile);
    for (auto& edge : input->edge_heights) {
        edge.fill(2'048U);
    }
    std::array<std::shared_ptr<client::TerrainLodColumnInput const>, 1> const inputs{input};
    auto const patch = client::buildTerrainLodColumnPatch(terrain->identity(), inputs);
    ASSERT_EQ(patch->groups.size(), 1U);
    EXPECT_EQ(patch->groups[0].maximum_z, 2'048);
    uint32_t world_top_faces = 0U;
    for (auto const& face : logicalFaces(*patch)) {
        if (face.faceDirection() == 5U && face.z == 2'047) {
            ++world_top_faces;
            EXPECT_EQ(face.material_id, static_cast<uint32_t>(shared::Block::Sand));
        }
        EXPECT_FALSE(face.faceDirection() == 5U && face.z == 1'023);
    }
    EXPECT_EQ(world_top_faces, 256U);
}

TEST(TerrainLodPresentationTest, ClimateTransitionsMeetTopWorldEditsWithoutClippingOrPackedOverflow)
{
    auto const terrain = presentationWorld(256U);
    for (uint32_t const origin : {32'768U, 0U}) {
        client::TerrainLodActualColumn actual{.x = origin / 16U, .y = origin / 16U};
        for (auto& edge : actual.edge_heights) {
            edge.fill(2'048U);
        }
        auto const scene = client::buildTerrainLodScene(*terrain, origin, origin, std::span{&actual, 1U});
        std::set<std::array<int32_t, 4>> border_segments;
        auto const on_border = [origin](std::array<int32_t, 2> const& point) {
            int32_t const low = static_cast<int32_t>(origin);
            int32_t const high = low + 16;
            return ((point[0] == low || point[0] == high) && point[1] >= low && point[1] <= high)
                || ((point[1] == low || point[1] == high) && point[0] >= low && point[0] <= high);
        };
        for (auto const& face : logicalFaces(*scene)) {
            if (face.faceDirection() != client::StoneFaceInstance::TERRAIN_TRIANGLE) {
                continue;
            }
            auto const heights = face.terrainTriangleHeights();
            auto const points = trianglePoints(face);
            std::array<std::array<int32_t, 2>, 3> xy{};
            for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
                EXPECT_LE(heights[vertex], 65'536U);
                xy[vertex] = {static_cast<int32_t>(points[vertex][0]) & 0xffff,
                    static_cast<int32_t>(points[vertex][1]) & 0xffff};
                if (on_border(xy[vertex])) {
                    EXPECT_EQ(heights[vertex], 65'536U);
                }
            }
            for (uint32_t edge = 0U; edge < 3U; ++edge) {
                auto a = xy[edge];
                auto b = xy[(edge + 1U) % 3U];
                if (on_border(a) && on_border(b)
                    && std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) == 1) {
                    if (b < a) {
                        std::swap(a, b);
                    }
                    border_segments.insert({a[0], a[1], b[0], b[1]});
                }
            }
        }
        EXPECT_EQ(border_segments.size(), 64U) << "all four sixteen-block sides meet the exact top";
        EXPECT_TRUE(std::ranges::any_of(scene->groups, [](auto const& group) { return group.maximum_z == 2'048; }));
    }
}

TEST(TerrainLodPresentationTest, V21HighGeneratedCornerMatchesItsLodSamplerAboveTheLegacyCeiling)
{
    auto const& world = v21PresentationWorld();
    uint32_t const period = generation::WORLD_SIZE_BLOCKS;
    auto const root = shared::buildTerrainLod(world, shared::TerrainLodLevel::L0, 0U, 0U, period);
    auto const highest = std::max_element(root->nodes.begin(), root->nodes.end(), [](auto const& a, auto const& b) {
        return a.sample.height < b.sample.height;
    });
    ASSERT_NE(highest, root->nodes.end());
    ASSERT_GT(highest->sample.height, 1'023.0);
    uint32_t const x = highest->key.x % period;
    uint32_t const y = highest->key.y % period;
    uint32_t const expected = static_cast<uint32_t>((highest->sample.height + 1.0) * 32.0);
    // Refinement is on the opposite side of the periodic world, preserving this vertex's L0 authority.
    auto const scene = client::buildTerrainLodScene(world, (x + period / 2U) % period, (y + period / 2U) % period);
    uint32_t matching_vertices = 0U;
    for (auto const& face : logicalFaces(*scene)) {
        if (face.faceDirection() != client::StoneFaceInstance::TERRAIN_TRIANGLE) {
            continue;
        }
        auto const points = trianglePoints(face);
        auto const heights = face.terrainTriangleHeights();
        for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
            if ((static_cast<uint32_t>(points[vertex][0]) & 0xffffU) == x
                && (static_cast<uint32_t>(points[vertex][1]) & 0xffffU) == y) {
                EXPECT_EQ(heights[vertex], expected);
                ++matching_vertices;
            }
        }
    }
    EXPECT_GT(matching_vertices, 0U);
}

TEST(TerrainLodPresentationTest, ProgressiveColumnPromotionMatchesFullSceneIncludingTransitionRim)
{
    auto const terrain = presentationWorld(256U, true);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (int32_t y = 0; y <= 2; ++y) {
        for (int32_t x = 0; x <= 3; ++x) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({.x = x, .y = y, .z = 0})));
        }
    }
    auto const first = client::captureTerrainLodColumn(world, 1U, 1U);
    auto const second = client::captureTerrainLodColumn(world, 2U, 1U);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    std::array const initial{std::make_shared<client::TerrainLodColumnInput const>(*first)};
    auto const previous = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, initial);
    std::array const expanded{initial.front(), std::make_shared<client::TerrainLodColumnInput const>(*second)};
    auto const progressive = client::buildTerrainLodSceneFromColumns(
        *terrain, 16U, 16U, expanded, {}, previous.get(), initial, true);
    auto const continued = client::buildTerrainLodSceneFromColumns(
        *terrain, 16U, 16U, expanded, {}, progressive.get(), expanded, true);
    auto const full = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, expanded);
    EXPECT_EQ(progressive->root_artifact, previous->root_artifact);
    EXPECT_EQ(continued->root_artifact, previous->root_artifact);
    EXPECT_NE(full->root_artifact, previous->root_artifact);
    ASSERT_EQ(progressive->actual_column_count, 2U);
    ASSERT_EQ(progressive->groups.size(), full->groups.size());
    ASSERT_EQ(continued->logical_face_count, progressive->logical_face_count);
    ASSERT_EQ(continued->groups.size(), progressive->groups.size());
    for (auto const& expected : full->groups) {
        auto const found = std::ranges::find_if(progressive->groups, [&](auto const& group) {
            return group.origin_x == expected.origin_x && group.origin_y == expected.origin_y
                && group.extent == expected.extent;
        });
        ASSERT_NE(found, progressive->groups.end());
        auto const expected_faces = client::terrainLodGroupFaces(expected);
        auto const actual_faces = client::terrainLodGroupFaces(*found);
        EXPECT_TRUE(std::ranges::equal(actual_faces, expected_faces)) << expected.origin_x << ',' << expected.origin_y;
        auto const continued_group = std::ranges::find_if(continued->groups, [&](auto const& group) {
            return group.origin_x == expected.origin_x && group.origin_y == expected.origin_y
                && group.extent == expected.extent;
        });
        ASSERT_NE(continued_group, continued->groups.end());
        EXPECT_EQ(continued_group->face_storage, found->face_storage);
        EXPECT_TRUE(std::ranges::equal(client::terrainLodGroupFaces(*continued_group), actual_faces));
    }
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*progressive, world));
    ASSERT_TRUE(world.setBlock({33, 17, 900}, shared::Block::Snow));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*progressive, world));
}

TEST(TerrainLodPresentationTest, ProgressivePromotionAcross128BlockRootMatchesFreshScene)
{
    auto const terrain = presentationWorld(512U, true);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (int32_t y = 0; y <= 2; ++y) {
        for (int32_t x = 6; x <= 9; ++x) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({.x = x, .y = y, .z = 0})));
        }
    }
    auto const left = client::captureTerrainLodColumn(world, 7U, 1U);
    auto const right = client::captureTerrainLodColumn(world, 8U, 1U);
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    std::array const initial{std::make_shared<client::TerrainLodColumnInput const>(*left)};
    std::array const expanded{initial.front(), std::make_shared<client::TerrainLodColumnInput const>(*right)};
    auto const previous = client::buildTerrainLodSceneFromColumns(*terrain, 128U, 16U, initial);
    auto const progressive = client::buildTerrainLodSceneFromColumns(
        *terrain, 128U, 16U, expanded, {}, previous.get(), initial, true);
    auto const fresh = client::buildTerrainLodSceneFromColumns(*terrain, 128U, 16U, expanded);
    ASSERT_EQ(progressive->root_extent, 128U);
    EXPECT_EQ(progressive->actual_column_count, 2U);
    EXPECT_EQ(progressive->leaves, fresh->leaves);
    EXPECT_EQ(logicalFaces(*progressive), logicalFaces(*fresh));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*progressive, world));
}

TEST(TerrainLodPresentationTest, ProgressiveSceneRebuildsRootWhenSourceAtlasChanges)
{
    auto const terrain = presentationWorld(256U, true);
    auto const other = presentationWorld(256U);
    auto const previous = client::buildTerrainLodScene(*terrain, 0U, 0U);
    auto incompatible = *previous;
    incompatible.shading_atlas = other->atlas();
    auto const rebuilt = client::buildTerrainLodScene(*terrain, 0U, 0U, {}, {}, &incompatible);
    auto const fresh = client::buildTerrainLodScene(*terrain, 0U, 0U);
    EXPECT_NE(rebuilt->root_artifact, previous->root_artifact);
    EXPECT_EQ(rebuilt->leaves, fresh->leaves);
    EXPECT_EQ(logicalFaces(*rebuilt), logicalFaces(*fresh));
    auto const replaced = client::buildTerrainLodScene(*other, 0U, 0U, {}, {}, previous.get());
    auto const other_fresh = client::buildTerrainLodScene(*other, 0U, 0U);
    EXPECT_NE(replaced->root_artifact, previous->root_artifact);
    EXPECT_EQ(replaced->leaves, other_fresh->leaves);
    EXPECT_EQ(logicalFaces(*replaced), logicalFaces(*other_fresh));
}

TEST(TerrainLodPresentationTest, NewDestinationBeyondOldRefinementPublishesEditedColumnWithLocalParentReplacement)
{
    auto const terrain = presentationWorld(256U, true);
    shared::SparseWorld world{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(world.setTerrainIdentity(terrain->identity()));
    for (auto const center : {client::TerrainLodColumnCoordinate{1U, 1U}, client::TerrainLodColumnCoordinate{101U, 101U}}) {
        for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
                std::array{0, -1}, std::array{0, 1}}) {
            ASSERT_TRUE(world.publishAuthoritativeSurface(terrain->sampleSurface({
                .x = static_cast<int32_t>(center.x) + offset[0],
                .y = static_cast<int32_t>(center.y) + offset[1], .z = 0})));
        }
    }
    auto first = client::captureTerrainLodColumn(world, 1U, 1U);
    ASSERT_TRUE(first);
    std::array const initial{std::make_shared<client::TerrainLodColumnInput const>(*first)};
    auto const previous = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, initial);
    ASSERT_TRUE(std::ranges::none_of(previous->leaves, [](auto const leaf) {
        return leaf.level == shared::TerrainLodLevel::L2 && leaf.x == 101U && leaf.y == 101U;
    }));
    ASSERT_TRUE(world.applyEditDelta({1617, 1617, 900}, shared::Block::Snow, 0U, 1U, 1U));
    auto destination = client::captureTerrainLodColumn(world, 101U, 101U);
    ASSERT_TRUE(destination);
    std::array const expanded{initial.front(), std::make_shared<client::TerrainLodColumnInput const>(*destination)};
    auto const local = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, expanded, {}, previous.get(), initial, true);
    auto const canonical = client::buildTerrainLodSceneFromColumns(*terrain, 16U, 16U, expanded, {}, nullptr, {}, true);
    ASSERT_EQ(local->actual_column_count, 2U);
    ASSERT_EQ(local->groups.size(), canonical->groups.size());
    size_t reused = 0U;
    for (auto const& group : local->groups) {
        auto const expected = std::ranges::find_if(canonical->groups, [&](auto const& entry) {
            return entry.origin_x == group.origin_x && entry.origin_y == group.origin_y && entry.extent == group.extent;
        });
        ASSERT_NE(expected, canonical->groups.end());
        EXPECT_TRUE(std::ranges::equal(client::terrainLodGroupFaces(group), client::terrainLodGroupFaces(*expected)))
            << "group=" << group.origin_x << ',' << group.origin_y << " extent=" << group.extent;
        reused += std::ranges::any_of(previous->groups, [&](auto const& entry) {
            return entry.face_storage == group.face_storage && entry.origin_x == group.origin_x
                && entry.origin_y == group.origin_y && entry.extent == group.extent;
        }) ? 1U : 0U;
    }
    EXPECT_GT(reused, 0U);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*local, world));
    ASSERT_TRUE(world.applyEditDelta({1617, 1617, 900}, shared::Block::Air, 1U, 2U, 2U));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*local, world));
}

TEST(TerrainLodPresentationTest, ExplicitVisualDiskFiltersSquareCornerAndKeepsCircleEdge)
{
    auto const world = presentationWorld(256U, true);
    std::array const columns{client::TerrainLodActualColumn{.x = 40U, .y = 40U},
        client::TerrainLodActualColumn{.x = 40U, .y = 0U},
        client::TerrainLodActualColumn{.x = 24U, .y = 32U}};
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, columns, {}, nullptr, {}, true,
        client::TerrainLodColumnCoordinate{0U, 0U});
    EXPECT_EQ(scene->actual_column_count, 2U);
    EXPECT_TRUE(std::ranges::none_of(scene->leaves, [](auto const leaf) {
        return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == 40U && leaf.y == 40U;
    }));
    for (auto const coordinate : {client::TerrainLodColumnCoordinate{40U, 0U},
             client::TerrainLodColumnCoordinate{24U, 32U}}) {
        EXPECT_TRUE(std::ranges::any_of(scene->leaves, [&](auto const leaf) {
            return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == coordinate.x && leaf.y == coordinate.y;
        }));
    }
}

TEST(TerrainLodPresentationTest, MovingVisualDiskKeepsNearFootprintsBeyondStaleCoarseCenter)
{
    auto const world = presentationWorld(256U, true);
    std::array const columns{client::TerrainLodActualColumn{.x = 100U, .y = 100U}};
    auto const scene = client::buildTerrainLodScene(*world, 0U, 0U, columns, {}, nullptr, {}, true,
        client::TerrainLodColumnCoordinate{100U, 100U});
    ASSERT_TRUE(scene->visual_center);
    EXPECT_EQ(scene->visual_center->x, 100U);
    EXPECT_EQ(scene->center_x, 0U);
    EXPECT_EQ(scene->actual_column_count, 1U);
    EXPECT_TRUE(std::ranges::any_of(scene->leaves, [](auto const leaf) {
        return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == 100U && leaf.y == 100U;
    }));
}


TEST(TerrainLodPresentationTest, V10ColdStartupScenePreservesCanonicalBedAndIdentity)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world{atlas};
    auto const start = std::chrono::steady_clock::now();
    // This is the normal full-scene entry point used before exact columns are
    // promoted. It must succeed without a diagnostic census or fallback mesh.
    auto const scene = client::buildTerrainLodScene(world, 32'768U, 32'768U);
    RecordProperty("normal_scene_build_seconds", std::to_string(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()));
    ASSERT_TRUE(scene);
    EXPECT_EQ(scene->identity, world.identity());
    EXPECT_EQ(scene->shading_atlas, atlas);
    EXPECT_EQ(scene->center_x, 32'768U);
    EXPECT_EQ(scene->center_y, 32'768U);
    ASSERT_TRUE(scene->river_constraints);
    EXPECT_EQ(scene->groups.size(), scene->leaves.size());
    uint64_t covered_area = 0U;
    for (auto const leaf : scene->leaves) {
        uint64_t const extent = client::terrainLodRegionExtent(leaf.level, scene->root_extent);
        covered_area += extent * extent;
    }
    EXPECT_EQ(covered_area, 65'536ULL * 65'536ULL);
    auto const& audit = scene->river_geometry_audit;
    EXPECT_GT(audit.inspected_fan_count, 0U);
    EXPECT_EQ(audit.diagnostic_failed_fan_count, 0U);
    EXPECT_EQ(audit.snapped_top_triangle_reversal_count, 0U);
    EXPECT_EQ(audit.emitted_bridge_triangle_count, 0U);
    EXPECT_EQ(audit.generated_top_triangle_count, client::terrainLodSceneFaceCount(*scene));
    EXPECT_EQ(static_cast<int64_t>(audit.original_top_triangle_count) + audit.top_face_count_change,
        static_cast<int64_t>(audit.generated_top_triangle_count));
    EXPECT_LE(audit.maximum_xy_quantization_error_m, std::sqrt(2.0) / 64.0 + 1e-12);
    size_t observed = 0U, encoded_reversals = 0U;
    for (auto const& group : scene->groups) {
        bool const near_startup = group.origin_x + group.extent >= 32'256U && group.origin_x <= 33'280U
            && group.origin_y + group.extent >= 32'256U && group.origin_y <= 33'280U;
        for (auto const& face : client::terrainLodGroupFaces(group)) {
            ASSERT_EQ(face.faceDirection(), 6U);
            auto const points = trianglePoints(face);
            double const area = (points[1][0] - points[0][0]) * (points[2][1] - points[0][1])
                - (points[1][1] - points[0][1]) * (points[2][0] - points[0][0]);
            encoded_reversals += area < 0.0 ? 1U : 0U;
            if (!near_startup) {
                continue;
            }
            for (auto const& point : points) {
                EXPECT_NEAR(point[2], generation::v10L0ElevationAt(*atlas, point[0], point[1]) + 513.0,
                    1.0 / 64.0 + 1e-8);
                ++observed;
            }
        }
    }
    EXPECT_EQ(encoded_reversals, 0U);
    EXPECT_GT(observed, 1'000U);
    RecordProperty("normal_scene_inspected_fans", std::to_string(audit.inspected_fan_count));
    RecordProperty("normal_scene_faces", std::to_string(client::terrainLodSceneFaceCount(*scene)));
    RecordProperty("normal_scene_canonical_vertices_checked", std::to_string(observed));
}

TEST(TerrainLodPresentationTest, V10ConfluenceSceneUsesExactConstrainedBedAndPreservesSteppedAuthority)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const world{atlas};
    auto const start = std::chrono::steady_clock::now();
    auto const scene = client::buildTerrainLodScene(world, 34'816U, 30'208U);
    double const initial_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    RecordProperty("initial_scene_build_seconds", std::to_string(initial_seconds));
    RecordProperty("initial_scene_face_count", std::to_string(client::terrainLodSceneFaceCount(*scene)));
    ASSERT_TRUE(scene->river_constraints);
    auto const& audit = scene->river_geometry_audit;
    RecordProperty("maximum_xy_quantization_error_m", std::to_string(audit.maximum_xy_quantization_error_m));
    RecordProperty("snapped_top_triangle_reversal_count", std::to_string(audit.snapped_top_triangle_reversal_count));
    RecordProperty("nearest_snap_top_triangle_reversal_count",
        std::to_string(audit.nearest_snap_top_triangle_reversal_count));
    RecordProperty("role_aware_top_triangle_reversal_count",
        std::to_string(audit.role_aware_top_triangle_reversal_count));
    RecordProperty("radial_steiner_lattice_adjustment_count",
        std::to_string(audit.radial_steiner_lattice_adjustment_count));
    RecordProperty("steiner_node_reassignment_count", std::to_string(audit.steiner_node_reassignment_count));
    RecordProperty("unconstrained_edge_flip_count", std::to_string(audit.unconstrained_edge_flip_count));
    RecordProperty("constrained_cavity_retriangulation_count",
        std::to_string(audit.constrained_cavity_retriangulation_count));
    RecordProperty("cavity_retriangulated_triangle_count", std::to_string(audit.cavity_retriangulated_triangle_count));
    RecordProperty("original_top_triangle_count", std::to_string(audit.original_top_triangle_count));
    RecordProperty("cavity_output_triangle_count", std::to_string(audit.cavity_output_triangle_count));
    RecordProperty("duplicate_renderer_node_quotient_count",
        std::to_string(audit.duplicate_renderer_node_quotient_count));
    RecordProperty("top_face_count_change", std::to_string(audit.top_face_count_change));
    EXPECT_EQ(static_cast<int64_t>(audit.original_top_triangle_count) + audit.top_face_count_change,
        static_cast<int64_t>(audit.generated_top_triangle_count));
    RecordProperty("snapped_top_triangle_collapse_count", std::to_string(audit.snapped_top_triangle_collapse_count));
    EXPECT_EQ(audit.generated_top_triangle_count, client::terrainLodSceneFaceCount(*scene));
    EXPECT_EQ(audit.snapped_top_triangle_reversal_count, 0U);
    EXPECT_EQ(audit.emitted_bridge_triangle_count, 0U);
    EXPECT_LE(audit.maximum_xy_quantization_error_m, std::sqrt(2.0) / 64.0 + 1e-12);
    size_t observed = 0U, fractional = 0U;
    std::ofstream output;
    if (char const* const filename = std::getenv("MC_RIVER_SCENE_EVIDENCE")) {
        output.open(filename);
    }
    if (output) {
        output << std::setprecision(17) << "{\"center\":[34816,30208],\"faces\":[";
    }
    bool first = true;
    for (auto const& group : scene->groups) {
        if (group.origin_x + group.extent < 34'304U || group.origin_x > 35'328U
            || group.origin_y + group.extent < 29'696U || group.origin_y > 30'720U) {
            continue;
        }
        for (auto const& face : client::terrainLodGroupFaces(group)) {
            ASSERT_EQ(face.faceDirection(), 6U);
            auto const points = trianglePoints(face);
            if (output) {
                if (!first) {
                    output << ',';
                }
                first = false;
                output << '[';
                for (size_t i = 0; i < 3U; ++i) {
                    if (i != 0U) {
                        output << ',';
                    }
                    output << '[' << points[i][0] << ',' << points[i][1] << ',' << points[i][2] << ']';
                }
                output << ']';
            }
            for (auto const& point : points) {
                EXPECT_NEAR(point[2], generation::v10L0ElevationAt(*atlas, point[0], point[1]) + 513.0, 1.0 / 64.0 + 1e-8);
                fractional += point[0] != std::floor(point[0]) || point[1] != std::floor(point[1]);
                ++observed;
            }
        }
    }
    EXPECT_GT(observed, 1'000U);
    EXPECT_GT(fractional, 0U);
    if (output) {
        output << "],\"logical_face_count\":" << client::terrainLodSceneFaceCount(*scene)
        << ",\"group_count\":" << scene->groups.size() << ",\"initial_build_seconds\":" << initial_seconds
        << ",\"maximum_xy_quantization_error_m\":" << audit.maximum_xy_quantization_error_m
        << ",\"generated_top_triangle_count\":" << audit.generated_top_triangle_count
        << ",\"snapped_top_triangle_reversal_count\":" << audit.snapped_top_triangle_reversal_count
        << ",\"nearest_snap_top_triangle_reversal_count\":" << audit.nearest_snap_top_triangle_reversal_count
        << ",\"role_aware_top_triangle_reversal_count\":" << audit.role_aware_top_triangle_reversal_count
        << ",\"radial_steiner_lattice_adjustment_count\":" << audit.radial_steiner_lattice_adjustment_count
        << ",\"steiner_node_reassignment_count\":" << audit.steiner_node_reassignment_count
        << ",\"unconstrained_edge_flip_count\":" << audit.unconstrained_edge_flip_count
        << ",\"constrained_cavity_retriangulation_count\":" << audit.constrained_cavity_retriangulation_count
        << ",\"cavity_retriangulated_triangle_count\":" << audit.cavity_retriangulated_triangle_count
        << ",\"original_top_triangle_count\":" << audit.original_top_triangle_count
        << ",\"cavity_output_triangle_count\":" << audit.cavity_output_triangle_count
        << ",\"duplicate_renderer_node_quotient_count\":" << audit.duplicate_renderer_node_quotient_count
        << ",\"top_face_count_change\":" << audit.top_face_count_change
        << ",\"snapped_top_triangle_collapse_count\":" << audit.snapped_top_triangle_collapse_count << '}';
    }
    output.flush();

    client::TerrainLodActualColumn actual{.x = 2'176U, .y = 1'888U, .meshes = {}, .faces = {}};
    for (auto& edge : actual.edge_heights) {
        for (uint32_t i = 0U; i < 16U; ++i) {
            edge[i] = static_cast<uint16_t>(530U + i % 2U);
        }
    }
    actual.faces.push_back({.x = 34'816, .y = 30'208, .z = 529, .direction = 5U,
        .u_extent = 16U, .v_extent = 16U, .material_id = static_cast<uint32_t>(shared::Block::Grass)});
    std::array const changed{client::TerrainLodColumnCoordinate{actual.x, actual.y}};
    auto const promotion_start = std::chrono::steady_clock::now();
    auto const stepped = client::buildTerrainLodScene(world, 34'816U, 30'208U,
        std::span{&actual, 1U}, {}, scene.get(), changed);
    RecordProperty("authority_promotion_seconds", std::to_string(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - promotion_start).count()));
    auto const reuse_start = std::chrono::steady_clock::now();
    auto const reused = client::buildTerrainLodScene(world, 34'816U, 30'208U,
        std::span{&actual, 1U}, {}, stepped.get());
    RecordProperty("unchanged_reuse_seconds", std::to_string(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - reuse_start).count()));
    EXPECT_EQ(reused->root_artifact, stepped->root_artifact);
    EXPECT_EQ(client::terrainLodSceneFaceCount(*reused), client::terrainLodSceneFaceCount(*stepped));
    EXPECT_EQ(reused->river_geometry_audit.generated_top_triangle_count, 0U);
    EXPECT_EQ(stepped->river_geometry_audit.snapped_top_triangle_reversal_count, 0U);
    RecordProperty("promotion_snapped_top_triangle_collapse_count",
        std::to_string(stepped->river_geometry_audit.snapped_top_triangle_collapse_count));
    RecordProperty("promotion_bridge_triangle_count",
        std::to_string(stepped->river_geometry_audit.emitted_bridge_triangle_count));
    RecordProperty("promotion_nearest_snap_reversal_count",
        std::to_string(stepped->river_geometry_audit.nearest_snap_top_triangle_reversal_count));
    RecordProperty("promotion_steiner_node_reassignment_count",
        std::to_string(stepped->river_geometry_audit.steiner_node_reassignment_count));
    RecordProperty("promotion_unconstrained_edge_flip_count",
        std::to_string(stepped->river_geometry_audit.unconstrained_edge_flip_count));
    RecordProperty("promotion_constrained_cavity_count",
        std::to_string(stepped->river_geometry_audit.constrained_cavity_retriangulation_count));
    RecordProperty("promotion_duplicate_node_quotients",
        std::to_string(stepped->river_geometry_audit.duplicate_renderer_node_quotient_count));
    RecordProperty("promotion_face_count_change", std::to_string(stepped->river_geometry_audit.top_face_count_change));
    size_t exact_groups = 0U, fractional_bridge_triangles = 0U;
    std::array<double, 16> joined{};
    for (auto const& group : stepped->groups) {
        if (group.origin_x == actual.x * 16U && group.origin_y == actual.y * 16U && group.extent == 16U) {
            auto const faces = client::terrainLodGroupFaces(group);
            ASSERT_EQ(faces.size(), actual.faces.size());
            EXPECT_EQ(faces[0], actual.faces[0]);
            ++exact_groups;
            continue;
        }
        if (group.origin_x + group.extent < 34'816U || group.origin_x > 34'832U
            || group.origin_y + group.extent < 30'208U || group.origin_y > 30'208U) {
            continue;
        }
        for (auto const& face : client::terrainLodGroupFaces(group)) {
            if (face.faceDirection() != 6U) {
                continue;
            }
            auto const points = trianglePoints(face);
            double const area = (points[1][0] - points[0][0]) * (points[2][1] - points[0][1])
                - (points[1][1] - points[0][1]) * (points[2][0] - points[0][0]);
            bool const fractional_xy = std::ranges::any_of(points, [](auto const& point) {
                return point[0] != std::floor(point[0]) || point[1] != std::floor(point[1]);
            });
            double const area_x = (points[1][1] - points[0][1]) * (points[2][2] - points[0][2])
                - (points[1][2] - points[0][2]) * (points[2][1] - points[0][1]);
            double const area_y = (points[1][2] - points[0][2]) * (points[2][0] - points[0][0])
                - (points[1][0] - points[0][0]) * (points[2][2] - points[0][2]);
            if (area == 0.0 && fractional_xy && (area_x != 0.0 || area_y != 0.0)) {
                ++fractional_bridge_triangles;
            }
            for (size_t i = 0; i < 3U; ++i) {
                auto const& a = points[i];
                auto const& b = points[(i + 1U) % 3U];
                if (a[1] != 30'208.0 || b[1] != 30'208.0 || a[0] == b[0]) {
                    continue;
                }
                double const lo = std::min(a[0], b[0]), hi = std::max(a[0], b[0]);
                if (lo < 34'816.0 || hi > 34'832.0) {
                    continue;
                }
                uint32_t const along = std::min(15U, static_cast<uint32_t>(std::floor((lo + hi) * 0.5 - 34'816.0)));
                if (a[2] == actual.edge_heights[2][along] && b[2] == a[2]) {
                    joined[along] += hi - lo;
                }
            }
        }
    }
    EXPECT_EQ(exact_groups, 1U);
    EXPECT_GT(fractional_bridge_triangles, 0U);
    for (size_t i = 0U; i < joined.size(); ++i) {
        EXPECT_GE(joined[i], 1.0) << "authority segment " << i;
    }

    // Audit the actual emitted 3D patch. Each bridge is intentionally double
    // sided, so count its geometric triangle once before checking edge closure.
    using FixedPoint = std::array<int64_t, 3>;
    std::set<std::array<FixedPoint, 3>> unique_triangles;
    for (auto const& group : stepped->groups) {
        if (group.origin_x < 34'784U || group.origin_x + group.extent > 34'864U
            || group.origin_y < 30'176U || group.origin_y + group.extent > 30'256U) {
            continue;
        }
        for (auto const& face : client::terrainLodGroupFaces(group)) {
            if (face.faceDirection() != 6U) {
                continue;
            }
            auto const points = trianglePoints(face);
            std::array<FixedPoint, 3> fixed{};
            for (size_t i = 0U; i < 3U; ++i) {
                for (size_t axis = 0U; axis < 3U; ++axis) {
                    fixed[i][axis] = std::llround(points[i][axis] * 32.0);
                }
            }
            if (fixed[0] == fixed[1] || fixed[1] == fixed[2] || fixed[2] == fixed[0]) {
                continue;
            }
            std::sort(fixed.begin(), fixed.end());
            unique_triangles.insert(fixed);
        }
    }
    std::map<std::array<FixedPoint, 2>, uint32_t> edge_counts;
    for (auto const& triangle : unique_triangles) {
        for (size_t i = 0U; i < 3U; ++i) {
            std::array<FixedPoint, 2> edge{triangle[i], triangle[(i + 1U) % 3U]};
            std::sort(edge.begin(), edge.end());
            ++edge_counts[edge];
        }
    }
    size_t interior_open = 0U, interior_nonmanifold = 0U;
    std::vector<std::pair<std::array<FixedPoint, 2>, uint32_t>> unmatched_edges;
    for (auto const& [edge, count] : edge_counts) {
        auto const& a = edge[0];
        auto const& b = edge[1];
        bool const patch_perimeter = (a[0] == b[0] && (a[0] == 34'784LL * 32 || a[0] == 34'864LL * 32))
            || (a[1] == b[1] && (a[1] == 30'176LL * 32 || a[1] == 30'256LL * 32));
        bool const actual_perimeter = (a[0] == b[0] && (a[0] == 34'816LL * 32 || a[0] == 34'832LL * 32)
                && a[1] >= 30'208LL * 32 && a[1] <= 30'224LL * 32 && b[1] >= 30'208LL * 32 && b[1] <= 30'224LL * 32)
            || (a[1] == b[1] && (a[1] == 30'208LL * 32 || a[1] == 30'224LL * 32)
                && a[0] >= 34'816LL * 32 && a[0] <= 34'832LL * 32 && b[0] >= 34'816LL * 32 && b[0] <= 34'832LL * 32);
        if (patch_perimeter || actual_perimeter) {
            continue;
        }
        interior_open += count == 1U ? 1U : 0U;
        interior_nonmanifold += count > 2U ? 1U : 0U;
        if (count != 2U) {
            unmatched_edges.emplace_back(edge, count);
        }
    }
    if (char const* const filename = std::getenv("MC_RIVER_SCENE_EVIDENCE")) {
        std::ofstream diagnostic(std::string(filename) + ".stepped-patch.json");
        diagnostic << "{\"coordinate_scale\":32,\"unique_triangles\":[";
        bool first_triangle = true;
        for (auto const& triangle : unique_triangles) {
            if (!first_triangle) {
                diagnostic << ',';
            }
            first_triangle = false;
            diagnostic << '[';
            for (size_t i = 0U; i < 3U; ++i) {
                if (i != 0U) {
                    diagnostic << ',';
                }
                diagnostic << '[' << triangle[i][0] << ',' << triangle[i][1] << ',' << triangle[i][2] << ']';
            }
            diagnostic << ']';
        }
        diagnostic << "],\"unmatched_edges\":[";
        for (size_t i = 0U; i < unmatched_edges.size(); ++i) {
            if (i != 0U) {
                diagnostic << ',';
            }
            auto const& [edge, count] = unmatched_edges[i];
            diagnostic << "{\"count\":" << count << ",\"vertices\":[[" << edge[0][0] << ',' << edge[0][1]
                << ',' << edge[0][2] << "],[" << edge[1][0] << ',' << edge[1][1] << ',' << edge[1][2] << "]]}";
        }
        diagnostic << "]}\n";
    }
    RecordProperty("stepped_patch_interior_open_edges", std::to_string(interior_open));
    RecordProperty("stepped_patch_interior_nonmanifold_edges", std::to_string(interior_nonmanifold));
    EXPECT_FALSE(unique_triangles.empty());
    EXPECT_EQ(interior_open, 0U);
    EXPECT_EQ(interior_nonmanifold, 0U);
}

namespace {
void expectSameTerrainScene(client::TerrainLodScene const& actual, client::TerrainLodScene const& expected)
{
    EXPECT_EQ(actual.identity, expected.identity);
    EXPECT_EQ(actual.center_x, expected.center_x);
    EXPECT_EQ(actual.center_y, expected.center_y);
    EXPECT_EQ(actual.visual_center, expected.visual_center);
    EXPECT_EQ(actual.root_extent, expected.root_extent);
    EXPECT_EQ(actual.block_sea_level, expected.block_sea_level);
    EXPECT_EQ(actual.leaves, expected.leaves);
    EXPECT_EQ(actual.actual_column_count, expected.actual_column_count);
    EXPECT_EQ(actual.logical_face_count, expected.logical_face_count);
    EXPECT_EQ(actual.chunk_sources, expected.chunk_sources);
    EXPECT_EQ(actual.edit_sources, expected.edit_sources);
    EXPECT_EQ(actual.authority_sources, expected.authority_sources);
    ASSERT_EQ(actual.groups.size(), expected.groups.size());
    for (size_t i = 0U; i < actual.groups.size(); ++i) {
        auto const& a = actual.groups[i];
        auto const& b = expected.groups[i];
        ASSERT_EQ(a.origin_x, b.origin_x);
        ASSERT_EQ(a.origin_y, b.origin_y);
        ASSERT_EQ(a.extent, b.extent);
        EXPECT_EQ(a.minimum_z, b.minimum_z);
        EXPECT_EQ(a.maximum_z, b.maximum_z);
        EXPECT_EQ(a.range.first_instance, b.range.first_instance);
        ASSERT_EQ(a.range.instance_count, b.range.instance_count);
        auto const af = client::terrainLodGroupFaces(a);
        auto const bf = client::terrainLodGroupFaces(b);
        ASSERT_EQ(af.size(), bf.size());
        if (!af.empty()) {
            EXPECT_EQ(std::memcmp(af.data(), bf.data(), af.size_bytes()), 0)
            << "group=" << a.origin_x << ',' << a.origin_y << " extent=" << a.extent;
        }
    }
}

TEST(TerrainLodPresentationTest, InitialColumnSceneReusesPreparedWorldRootWithIdenticalGeometry)
{
    auto const world = presentationWorld(256U, true);
    auto const root = shared::buildTerrainLod(*world, shared::TerrainLodLevel::L0);
    auto const fresh = client::buildTerrainLodSceneFromColumns(*world, 0U, 0U, {});
    auto const reused = client::buildTerrainLodSceneFromColumns(
        *world, 0U, 0U, {}, {}, nullptr, {}, false, std::nullopt, root);
    EXPECT_EQ(reused->root_artifact, root);
    expectSameTerrainScene(*reused, *fresh);
}

TEST(TerrainLodPresentationTest, InitialColumnSceneRebuildsForeignOrIncompletePreparedRoot)
{
    auto const world = presentationWorld(256U, true);
    auto const foreign = presentationWorld(512U);
    auto const foreign_root = shared::buildTerrainLod(*foreign, shared::TerrainLodLevel::L0);
    auto const fresh = client::buildTerrainLodSceneFromColumns(*world, 0U, 0U, {});
    auto incomplete = std::make_shared<shared::TerrainLodArtifact>(*fresh->root_artifact);
    incomplete->nodes.pop_back();
    for (auto const& root : {foreign_root, std::shared_ptr<shared::TerrainLodArtifact const>(incomplete)}) {
        auto const rebuilt = client::buildTerrainLodSceneFromColumns(
            *world, 0U, 0U, {}, {}, nullptr, {}, false, std::nullopt, root);
        EXPECT_NE(rebuilt->root_artifact, root);
        expectSameTerrainScene(*rebuilt, *fresh);
    }
}

void writeRecenterCrop(client::TerrainLodScene const& scene, char const* const suffix)
{
    char const* const base = std::getenv("MC_RIVER_SCENE_EVIDENCE");
    if (base == nullptr) {
        return;
    }
    std::ofstream output(std::string(base) + suffix);
    output << std::setprecision(17) << "{\"center\":[" << scene.center_x << ',' << scene.center_y << "],\"faces\":[";
    bool first = true;
    for (auto const& group : scene.groups) {
        if (int64_t(group.origin_x + group.extent) < int64_t(scene.center_x) - 512
            || int64_t(group.origin_x) > int64_t(scene.center_x) + 512
            || int64_t(group.origin_y + group.extent) < int64_t(scene.center_y) - 512
            || int64_t(group.origin_y) > int64_t(scene.center_y) + 512) {
            continue;
        }
        for (auto const& face : client::terrainLodGroupFaces(group)) {
            if (face.faceDirection() != 6U) {
                continue;
            }
            if (!first) {
                output << ',';
            }
            first = false;
            auto const points = trianglePoints(face);
            output << '[';
            for (size_t i = 0U; i < points.size(); ++i) {
                if (i != 0U) {
                    output << ',';
                }
                output << '[' << points[i][0] << ',' << points[i][1] << ',' << points[i][2] << ']';
            }
            output << ']';
        }
    }
    output << "],\"logical_face_count\":" << scene.logical_face_count << '}';
}

void checkV10Recenter(uint32_t const old_x, uint32_t const new_x, bool const change_authority,
    bool const promote_columns = false)
{
    static auto const terrain = [] {
        generation::GenerationConfiguration configuration;
        configuration.model_tag = generation::TerrainModelTag::V10;
        return std::make_unique<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    }();
    uint32_t constexpr Y = 30'208U;
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain->identity()));
    for (uint32_t const cx : {old_x / 16U, new_x / 16U}) {
        for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
                 std::array{0, -1}, std::array{0, 1}}) {
            int32_t const x = (static_cast<int32_t>(cx) + offset[0] + 4'096) % 4'096;
            int32_t const y = static_cast<int32_t>(Y / 16U) + offset[1];
            ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain->sampleSurface({x, y, 0})));
        }
    }
    auto const a = client::captureTerrainLodColumn(sparse, old_x / 16U, Y / 16U);
    auto const b = client::captureTerrainLodColumn(sparse, new_x / 16U, Y / 16U);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    std::array const initial{std::make_shared<client::TerrainLodColumnInput const>(*a)};
    std::array const expanded{initial[0], std::make_shared<client::TerrainLodColumnInput const>(*b)};
    auto const previous = client::buildTerrainLodSceneFromColumns(*terrain, old_x, Y, initial, {}, nullptr, {}, false,
        client::TerrainLodColumnCoordinate{old_x / 16U, Y / 16U});
    std::shared_ptr<client::TerrainLodScene const> reused;
    auto start = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(reused = client::buildTerrainLodSceneFromColumns(*terrain, new_x, Y, expanded, {},
        previous.get(), initial, promote_columns, client::TerrainLodColumnCoordinate{new_x / 16U, Y / 16U}));
    double const reused_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(reused);
    start = std::chrono::steady_clock::now();
    auto const fresh = client::buildTerrainLodSceneFromColumns(*terrain, new_x, Y, expanded, {}, nullptr, {}, promote_columns,
        client::TerrainLodColumnCoordinate{new_x / 16U, Y / 16U});
    double const fresh_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expectSameTerrainScene(*reused, *fresh);
    EXPECT_EQ(reused->root_artifact, previous->root_artifact);
    EXPECT_NE(fresh->root_artifact, previous->root_artifact);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*reused, sparse));
    EXPECT_EQ(reused->river_geometry_audit.regional_export_build_count, 0U);
    EXPECT_GT(reused->river_geometry_audit.regional_export_reuse_count, 0U);
    EXPECT_GT(fresh->river_geometry_audit.regional_export_build_count, 0U);
    std::set<void const*> old_storage;
    for (auto const& group : previous->groups) {
        if (group.face_storage) {
        old_storage.insert(group.face_storage.get());
    }
    }
    size_t reused_groups = 0U;
    for (auto const& group : reused->groups) {
        reused_groups += group.face_storage && old_storage.contains(group.face_storage.get());
    }
    EXPECT_GT(reused_groups, 0U);
    testing::Test::RecordProperty("recenter_fresh_seconds", std::to_string(fresh_seconds));
    testing::Test::RecordProperty("recenter_reused_seconds", std::to_string(reused_seconds));
    testing::Test::RecordProperty("recenter_speedup", std::to_string(fresh_seconds / reused_seconds));
    testing::Test::RecordProperty("recenter_reused_groups", std::to_string(reused_groups));
    testing::Test::RecordProperty("recenter_rebuilt_groups", std::to_string(reused->groups.size() - reused_groups));
    testing::Test::RecordProperty("recenter_fresh_exports",
        std::to_string(fresh->river_geometry_audit.regional_export_build_count));
    testing::Test::RecordProperty("recenter_reused_exports",
        std::to_string(reused->river_geometry_audit.regional_export_build_count));
    // Only rendered exact inputs can reuse geometry when the selected footprint moves.
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selection_unchanged;
    for (auto const& input : expanded) {
        if (std::ranges::any_of(reused->leaves, [&](auto const leaf) {
                return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == input->x && leaf.y == input->y;
            })) {
            selection_unchanged.push_back(input);
        }
    }
    auto const selection_only = client::buildTerrainLodSceneFromColumns(*terrain, old_x, Y, expanded, {},
        reused.get(), selection_unchanged, promote_columns, client::TerrainLodColumnCoordinate{old_x / 16U, Y / 16U});
    auto const selection_only_fresh = client::buildTerrainLodSceneFromColumns(*terrain, old_x, Y, expanded, {},
        nullptr, {}, promote_columns, client::TerrainLodColumnCoordinate{old_x / 16U, Y / 16U});
    {
        SCOPED_TRACE("reverse selection after recenter");
        expectSameTerrainScene(*selection_only, *selection_only_fresh);
    }
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*selection_only, sparse));
    if (change_authority) {
        writeRecenterCrop(*fresh, ".recenter-fresh.json");
        writeRecenterCrop(*reused, ".recenter-reused.json");
        int32_t const edit_z = initial[0]->edge_heights[0][0];
        ASSERT_TRUE(sparse.applyEditDelta({static_cast<int32_t>(old_x), static_cast<int32_t>(Y), edit_z},
            shared::Block::Snow, 0U, 1U, 1U));
        EXPECT_FALSE(client::terrainLodSourcesCurrent(*reused, sparse));
        auto changed = client::captureTerrainLodColumn(sparse, old_x / 16U, Y / 16U);
        ASSERT_TRUE(changed);
        ASSERT_EQ(changed->authority_profiles.size(), 18U * 18U);
        EXPECT_EQ(changed->authority_profiles[19U].materialAt(static_cast<uint16_t>(edit_z)), shared::Block::Snow);
        EXPECT_EQ(changed->edge_heights[0][0], edit_z + 1);
        // Change authority without changing any selection key. Its independent
        // dependency invalidation must still rebuild affected transition geometry.
        std::array const changed_inputs{std::make_shared<client::TerrainLodColumnInput const>(*changed), expanded[1]};
        std::array const unchanged_inputs{expanded[1]};
        auto const updated = client::buildTerrainLodSceneFromColumns(*terrain, new_x, Y, changed_inputs, {},
            reused.get(), unchanged_inputs, promote_columns, client::TerrainLodColumnCoordinate{new_x / 16U, Y / 16U});
        auto const updated_fresh = client::buildTerrainLodSceneFromColumns(*terrain, new_x, Y, changed_inputs, {},
            nullptr, {}, promote_columns, client::TerrainLodColumnCoordinate{new_x / 16U, Y / 16U});
        expectSameTerrainScene(*updated, *updated_fresh);
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*updated, sparse));
        // Removing an unchanged exact column changes the rendered selection.
        std::array const removed_inputs{changed_inputs[0]};
        auto const removed = client::buildTerrainLodSceneFromColumns(*terrain, new_x, Y, removed_inputs, {},
            updated.get(), removed_inputs, promote_columns, client::TerrainLodColumnCoordinate{new_x / 16U, Y / 16U});
        auto const removed_fresh = client::buildTerrainLodSceneFromColumns(*terrain, new_x, Y, removed_inputs, {},
            nullptr, {}, promote_columns, client::TerrainLodColumnCoordinate{new_x / 16U, Y / 16U});
        expectSameTerrainScene(*removed, *removed_fresh);
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*removed, sparse));
    }
}
} // namespace

TEST(TerrainLodPresentationTest, V10Confluence64RecenterMatchesFreshGeometryAndAuthorityChanges)
{
    checkV10Recenter(34'816U, 34'880U, true);
}

TEST(TerrainLodPresentationTest, V10Wrapped64RecenterMatchesFreshGeometry)
{
    checkV10Recenter(65'472U, 0U, false);
}

TEST(TerrainLodPresentationTest, V10CombinedRecenterAndFrontierPromotionMatchesFreshGeometry)
{
    checkV10Recenter(32'768U, 33'472U, false, true);
}

TEST(TerrainLodPresentationTest, V10CombinedConfluenceRecenterPromotionAndAuthorityChangeMatchFreshGeometry)
{
    checkV10Recenter(34'816U, 34'880U, true, true);
}

TEST(TerrainLodPresentationTest, V10CombinedWrappedRecenterAndPromotionMatchesFreshGeometry)
{
    checkV10Recenter(65'472U, 0U, false, true);
}

TEST(TerrainLodPresentationTest, V10FineProgressiveSelectionCacheMatchesFreshAcrossWrappedForceChanges)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    auto const terrain = std::make_unique<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    static constexpr uint32_t COLUMN_COUNT = shared::WorldExtent::WIDTH / 16U;
    static constexpr uint32_t CENTER_X = 0U;
    static constexpr uint32_t CENTER_Y = 2'048U;
    static constexpr uint32_t WRAPPED_X = COLUMN_COUNT - 1U;
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain->identity()));
    for (int32_t dy = -3; dy <= 3; ++dy) {
        for (int32_t dx = -3; dx <= 3; ++dx) {
            uint32_t const x = static_cast<uint32_t>((dx + static_cast<int32_t>(COLUMN_COUNT))
                % static_cast<int32_t>(COLUMN_COUNT));
            int32_t const y = static_cast<int32_t>(CENTER_Y) + dy;
            ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain->sampleSurface({
                .x = static_cast<int32_t>(x), .y = y, .z = 0})));
        }
    }
    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    auto const base_capture = client::captureTerrainLodColumn(sparse, WRAPPED_X, CENTER_Y);
    ASSERT_TRUE(base_capture);
    Input const base_input = std::make_shared<client::TerrainLodColumnInput const>(*base_capture);
    std::vector<Input> const initial{base_input};
    std::vector<Input> expanded;
    expanded.reserve(25U);
    for (int32_t dy = -2; dy <= 2; ++dy) {
        for (int32_t dx = -2; dx <= 2; ++dx) {
            uint32_t const x = static_cast<uint32_t>((dx + static_cast<int32_t>(COLUMN_COUNT))
                % static_cast<int32_t>(COLUMN_COUNT));
            uint32_t const y = static_cast<uint32_t>(static_cast<int32_t>(CENTER_Y) + dy);
            if (x == WRAPPED_X && y == CENTER_Y) {
                expanded.push_back(base_input);
                continue;
            }
            auto captured = client::captureTerrainLodColumn(sparse, x, y);
            ASSERT_TRUE(captured);
            expanded.push_back(std::make_shared<client::TerrainLodColumnInput const>(*captured));
        }
    }
    ASSERT_EQ(expanded.size(), 25U);
    client::TerrainLodColumnCoordinate const visual_center{CENTER_X, CENTER_Y};
    auto const base = client::buildTerrainLodSceneFromColumns(*terrain, CENTER_X * 16U, CENTER_Y * 16U,
        initial, {}, nullptr, {}, true, visual_center);

    auto start = std::chrono::steady_clock::now();
    auto const added_reused = client::buildTerrainLodSceneFromColumns(*terrain, CENTER_X * 16U, CENTER_Y * 16U,
        expanded, {}, base.get(), initial, true);
    double const added_reused_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    auto const added_fresh = client::buildTerrainLodSceneFromColumns(*terrain, CENTER_X * 16U, CENTER_Y * 16U,
        expanded, {}, nullptr, {}, true);
    double const added_fresh_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    auto const same_forcing_reused = client::buildTerrainLodSceneFromColumns(*terrain,
        CENTER_X * 16U, CENTER_Y * 16U, expanded, {}, added_reused.get(), expanded, true);
    double const same_forcing_reused_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    expectSameTerrainScene(*added_reused, *added_fresh);
    expectSameTerrainScene(*same_forcing_reused, *added_fresh);
    EXPECT_FALSE(added_reused->visual_center);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*added_reused, sparse));
    EXPECT_GT(added_reused->selection_audit.reused_l0_root_count, 0U);
    EXPECT_GT(added_reused->selection_audit.rebuilt_l0_root_count, 0U);
    EXPECT_EQ(added_fresh->selection_audit.reused_l0_root_count, 0U);
    EXPECT_EQ(added_fresh->selection_audit.rebuilt_l0_root_count,
        added_reused->selection_audit.reused_l0_root_count + added_reused->selection_audit.rebuilt_l0_root_count);
    EXPECT_EQ(same_forcing_reused->selection_audit.reused_l0_root_count,
        added_fresh->selection_audit.rebuilt_l0_root_count);
    EXPECT_EQ(same_forcing_reused->selection_audit.rebuilt_l0_root_count, 0U);

    start = std::chrono::steady_clock::now();
    auto const removed_reused = client::buildTerrainLodSceneFromColumns(*terrain, CENTER_X * 16U, CENTER_Y * 16U,
        initial, {}, added_reused.get(), initial, true, visual_center);
    double const removed_reused_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    auto const removed_fresh = client::buildTerrainLodSceneFromColumns(*terrain, CENTER_X * 16U, CENTER_Y * 16U,
        initial, {}, nullptr, {}, true, visual_center);
    double const removed_fresh_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    expectSameTerrainScene(*removed_reused, *removed_fresh);
    EXPECT_EQ(removed_reused->visual_center, visual_center);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*removed_reused, sparse));
    EXPECT_GT(removed_reused->selection_audit.reused_l0_root_count, 0U);
    EXPECT_GT(removed_reused->selection_audit.rebuilt_l0_root_count, 0U);
    EXPECT_EQ(removed_fresh->selection_audit.reused_l0_root_count, 0U);
    EXPECT_EQ(removed_fresh->selection_audit.rebuilt_l0_root_count,
        removed_reused->selection_audit.reused_l0_root_count + removed_reused->selection_audit.rebuilt_l0_root_count);
    RecordProperty("fine_selection_added_reused_seconds", std::to_string(added_reused_seconds));
    RecordProperty("fine_selection_added_fresh_seconds", std::to_string(added_fresh_seconds));
    RecordProperty("fine_selection_same_forcing_reused_seconds", std::to_string(same_forcing_reused_seconds));
    RecordProperty("fine_selection_removed_reused_seconds", std::to_string(removed_reused_seconds));
    RecordProperty("fine_selection_removed_fresh_seconds", std::to_string(removed_fresh_seconds));
    RecordProperty("fine_selection_added_reused_l0_roots",
        std::to_string(added_reused->selection_audit.reused_l0_root_count));
    RecordProperty("fine_selection_added_rebuilt_l0_roots",
        std::to_string(added_reused->selection_audit.rebuilt_l0_root_count));
    RecordProperty("fine_selection_removed_reused_l0_roots",
        std::to_string(removed_reused->selection_audit.reused_l0_root_count));
    RecordProperty("fine_selection_removed_rebuilt_l0_roots",
        std::to_string(removed_reused->selection_audit.rebuilt_l0_root_count));
}

TEST(TerrainLodPresentationTest, V10ReusedGeometryRefreshesAtlasAndReleasesPreviousContext)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    shared::TerrainWorld const terrain{atlas};
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain.identity()));
    for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
             std::array{0, -1}, std::array{0, 1}}) {
        ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain.sampleSurface(
            {2'048 + offset[0], 2'048 + offset[1], 0})));
    }
    auto const captured = client::captureTerrainLodColumn(sparse, 2'048U, 2'048U);
    ASSERT_TRUE(captured);
    std::array const inputs{std::make_shared<client::TerrainLodColumnInput const>(*captured)};
    auto previous = client::buildTerrainLodSceneFromColumns(terrain, 32'768U, 32'768U, inputs);
    std::weak_ptr<client::TerrainLodRiverConstraints const> const previous_context = previous->river_constraints;
    auto const warm = client::buildTerrainLodSceneFromColumns(terrain, 32'704U, 32'768U, inputs,
        {}, previous.get(), inputs);
    previous.reset();
    EXPECT_TRUE(previous_context.expired());
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*warm, sparse));

    auto const other_atlas = std::make_shared<generation::GlobalAtlas>(*atlas);
    shared::TerrainWorld const other{other_atlas};
    auto const refreshed = client::buildTerrainLodSceneFromColumns(other, 32'704U, 32'768U, inputs,
        {}, warm.get(), inputs);
    auto const fresh = client::buildTerrainLodSceneFromColumns(other, 32'704U, 32'768U, inputs);
    expectSameTerrainScene(*refreshed, *fresh);
    EXPECT_NE(refreshed->root_artifact, warm->root_artifact);
    EXPECT_GT(refreshed->river_geometry_audit.inspected_fan_count, 0U);
    EXPECT_EQ(refreshed->river_geometry_audit.inspected_fan_count, fresh->river_geometry_audit.inspected_fan_count);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*refreshed, sparse));
}

namespace {
struct StarInput final {
    std::array<double, 2> minimum;
    std::array<double, 2> maximum;
    std::vector<std::array<double, 2>> anchors;
    std::vector<std::array<uint32_t, 2>> edges;
};

StarInput starInput(double const extent, std::array<uint32_t, 4> const divisions, double const origin)
{
    StarInput result{{origin, origin}, {origin + extent, origin + extent},
        {{origin + extent / 2.0, origin + extent / 2.0}}, {}};
    std::array<std::array<double, 2>, 4> const corners{result.minimum,
        std::array{result.maximum[0], origin}, result.maximum, std::array{origin, result.maximum[1]}};
    for (size_t side = 0U; side < 4U; ++side) {
        auto const& a = corners[side];
        auto const& b = corners[(side + 1U) % 4U];
        for (uint32_t i = 0U; i < divisions[side]; ++i) {
            auto const point = [&](uint32_t const step) {
                double const t = double(step) / divisions[side];
                return std::array{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])};
            };
            uint32_t const first = static_cast<uint32_t>(result.anchors.size());
            result.anchors.insert(result.anchors.end(), {result.anchors[0], point(i), point(i), point(i + 1U)});
            result.edges.push_back({first, first + 1U});
            result.edges.push_back({first + 2U, first + 3U});
        }
    }
    return result;
}
} // namespace

TEST(TerrainLodPresentationTest, DirectRiverStarMatchesObservedCdtGeometryAndSampling)
{
    std::vector<std::pair<double, std::array<uint32_t, 4>>> const patterns{
        {2, {2, 2, 2, 2}}, {16, {1, 1, 1, 1}}, {16, {16, 1, 1, 1}},
        {16, {16, 16, 1, 1}}, {16, {16, 16, 16, 1}}, {64, {4, 1, 1, 1}},
        {64, {4, 4, 1, 1}}, {64, {16, 16, 4, 4}}, {64, {64, 1, 1, 1}},
        {64, {16, 4, 4, 1}}, {64, {64, 64, 1, 1}}, {256, {1, 1, 1, 1}},
        {256, {4, 1, 1, 1}}, {256, {4, 4, 1, 1}}};
    for (auto const& [extent, divisions] : patterns) {
        for (double const origin : {0.0, 48'128.0, 65'472.0}) {
            SCOPED_TRACE(testing::Message() << "extent=" << extent << " origin=" << origin);
            auto const input = starInput(extent, divisions, origin);
            std::vector<std::array<double, 2>> direct_calls, cdt_calls;
            auto const bed = [](double const x, double const y) { return x * x + y * y + x * y / 4.0; };
            auto const direct = client::detail::tryBuildRiverStarMesh(input.minimum, input.maximum,
                input.anchors, input.edges, [&](double const x, double const y) {
                    direct_calls.push_back({x, y});
                    return bed(x, y);
                });
            ASSERT_TRUE(direct);
            auto const cdt = client::buildRiverBedMesh(input.minimum, input.maximum, input.anchors, input.edges,
                [&](double const x, double const y) {
                    cdt_calls.push_back({x, y});
                    return bed(x, y);
                });
            EXPECT_EQ(direct_calls, cdt_calls);
            EXPECT_EQ(direct->vertices, cdt.vertices);
            auto const triangles = [](client::RiverBedMesh const& mesh) {
                std::set<std::array<std::array<double, 3>, 3>> result;
                for (auto const& t : mesh.triangles) {
                    std::array points{mesh.vertices[t[0]], mesh.vertices[t[1]], mesh.vertices[t[2]]};
                    std::sort(points.begin(), points.end());
                    EXPECT_TRUE(result.insert(points).second);
                }
                return result;
            };
            EXPECT_EQ(triangles(*direct), triangles(cdt));
            double area = 0.0;
            std::map<std::array<uint32_t, 2>, uint32_t> incidence;
            for (auto const& t : direct->triangles) {
                auto const& a = direct->vertices[t[0]];
                auto const& b = direct->vertices[t[1]];
                auto const& c = direct->vertices[t[2]];
                double const signed_area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                EXPECT_GT(signed_area, 0.0);
                area += signed_area;
                for (size_t j = 0U; j < 3U; ++j) {
                    ++incidence[{std::min(t[j], t[(j + 1U) % 3U]), std::max(t[j], t[(j + 1U) % 3U])}];
                }
            }
            EXPECT_EQ(area, 2.0 * extent * extent);
            for (auto const& [edge, count] : incidence) {
                EXPECT_LE(count, 2U);
                if (count == 1U) {
                    auto const& a = direct->vertices[edge[0]];
                    auto const& b = direct->vertices[edge[1]];
                    EXPECT_TRUE((a[0] == b[0] && (a[0] == origin || a[0] == origin + extent))
                        || (a[1] == b[1] && (a[1] == origin || a[1] == origin + extent)));
                }
            }
        }
    }
}

TEST(TerrainLodPresentationTest, DirectRiverStarRejectsCourseTangencyAndInvalidPerimeters)
{
    auto const input = starInput(2, {2, 2, 2, 2}, 0);
    auto const height = [](double const x, double const y) { return x * x + y * y; };
    ASSERT_TRUE(client::detail::tryBuildRiverStarMesh(input.minimum, input.maximum, input.anchors, input.edges, height));
    auto const rejected = [&](StarInput const& value) {
        EXPECT_FALSE(client::detail::tryBuildRiverStarMesh(value.minimum, value.maximum,
            value.anchors, value.edges, height));
    };
    auto tangent = input;
    tangent.anchors.push_back({1.0, 0.0});
    rejected(tangent);
    auto course = input;
    course.edges.push_back({0U, 2U});
    rejected(course);
    auto open = input;
    open.anchors.back() = {0.0, 0.5};
    rejected(open);
    auto nonfinite = input;
    nonfinite.anchors[0][0] = std::nan("");
    rejected(nonfinite);
    auto boundary_center = input;
    boundary_center.anchors[0] = {0.0, 1.0};
    rejected(boundary_center);
    auto diagonal = input;
    diagonal.anchors[2] = diagonal.anchors[3] = diagonal.anchors.back() = {0.5, 0.5};
    rejected(diagonal);
    auto repeated = input;
    repeated.anchors[4] = repeated.anchors[6] = repeated.anchors[7] = repeated.anchors[2];
    rejected(repeated);
}

TEST(TerrainLodPresentationTest, RiverLatticeNodingRetainsActualCrossingPathsAndSectorBarrier)
{
    // Four exact endpoints from Android graph025190ab; the separate horizontal
    // barrier checks that lattice noding does not change semantic partitioning.
    std::ifstream input(std::string(MC_V10_FIXTURE_DIRECTORY) + "/river_q5_actual_crossing.txt");
    ASSERT_TRUE(input);
    size_t point_count = 0U, edge_count = 0U;
    input >> point_count >> edge_count;
    std::vector<std::array<double, 2>> points(point_count);
    std::vector<std::array<uint32_t, 2>> edges(edge_count);
    for (auto& p : points) {
        input >> p[0] >> p[1];
    }
    for (auto& e : edges) {
        input >> e[0] >> e[1];
    }
    ASSERT_TRUE(input);
    auto const saved = points;
    auto const height = [](double x, double y) { return x * x + y * y; };
    auto const before = client::buildRiverBedMesh({48'131, 9'462}, {48'132, 9'464}, points, edges, height);
    EXPECT_TRUE(std::any_of(before.vertices.begin(), before.vertices.end(), [](auto const& p) {
        return p[0] * 32.0 != std::round(p[0] * 32.0) || p[1] * 32.0 != std::round(p[1] * 32.0);
    }));
    std::vector<std::vector<uint32_t>> paths;
    ASSERT_TRUE(client::detail::nodeRiverConstraintCrossings(points, edges, paths));
    EXPECT_EQ(points, saved);
    ASSERT_EQ(paths, (std::vector<std::vector<uint32_t>>{{0U, 2U, 1U}, {2U, 3U}, {4U, 5U}}));
    std::vector<std::array<uint32_t, 2>> noded;
    for (auto const& path : paths) {
        for (size_t j = 1U; j < path.size(); ++j) {
            noded.push_back({path[j - 1U], path[j]});
        }
    }
    auto const mesh = client::buildRiverBedMesh({48'131, 9'462}, {48'132, 9'464}, points, noded, height);
    std::map<std::array<double, 2>, uint32_t> index;
    for (uint32_t i = 0U; i < mesh.vertices.size(); ++i) {
        auto const& v = mesh.vertices[i];
        EXPECT_EQ(v[0] * 32.0, std::round(v[0] * 32.0));
        EXPECT_EQ(v[1] * 32.0, std::round(v[1] * 32.0));
        EXPECT_DOUBLE_EQ(v[2], height(v[0], v[1]));
        index.emplace(std::array<double, 2>{v[0], v[1]}, i);
    }
    std::map<std::array<uint32_t, 2>, uint32_t> mesh_edges;
    std::array<double, 2> sector_area{};
    for (auto const& t : mesh.triangles) {
        auto const& a = mesh.vertices[t[0]];
        auto const& b = mesh.vertices[t[1]];
        auto const& c = mesh.vertices[t[2]];
        double const area = ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2.0;
        ASSERT_GT(area, 0.0);
        bool const below = (a[1] + b[1] + c[1]) / 3.0 < 9'463.5;
        EXPECT_TRUE(below ? std::max({a[1], b[1], c[1]}) <= 9'463.5
            : std::min({a[1], b[1], c[1]}) >= 9'463.5);
        sector_area[below ? 0U : 1U] += area;
        for (size_t j = 0U; j < 3U; ++j) {
            ++mesh_edges[{std::min(t[j], t[(j + 1U) % 3U]), std::max(t[j], t[(j + 1U) % 3U])}];
        }
    }
    EXPECT_DOUBLE_EQ(sector_area[0], 1.5);
    EXPECT_DOUBLE_EQ(sector_area[1], 0.5);
    for (auto const& edge : noded) {
        uint32_t const a = index.at(points[edge[0]]), b = index.at(points[edge[1]]);
        EXPECT_TRUE(mesh_edges.contains({std::min(a, b), std::max(a, b)}));
    }
    for (auto const& [edge, count] : mesh_edges) {
        EXPECT_LE(count, 2U);
        if (count != 1U) {
            continue;
        }
        auto const& a = mesh.vertices[edge[0]];
        auto const& b = mesh.vertices[edge[1]];
        EXPECT_TRUE((a[0] == b[0] && (a[0] == 48'131 || a[0] == 48'132))
            || (a[1] == b[1] && (a[1] == 9'462 || a[1] == 9'464)));
    }
}

TEST(TerrainLodPresentationTest, RiverLatticeNodingRejectsCrossingWithoutExistingHotPixel)
{
    std::vector<std::array<double, 2>> const points{{0, 0}, {1, 1}, {0, 1}, {1, 0}};
    std::vector<std::array<uint32_t, 2>> const edges{{0U, 1U}, {2U, 3U}};
    std::vector<std::vector<uint32_t>> paths;
    EXPECT_FALSE(client::detail::nodeRiverConstraintCrossings(points, edges, paths));
}

TEST(TerrainLodPresentationTest, RiverLatticeIntersectionRetainsActualNewNodeAndStraightChains)
{
    // Exact crossing endpoints from Android graph8c8c3846. The horizontal
    // barrier independently protects sector coverage and shared edge ownership.
    std::ifstream input(std::string(MC_V10_FIXTURE_DIRECTORY) + "/river_q5_new_intersection.txt");
    ASSERT_TRUE(input);
    size_t point_count = 0U, edge_count = 0U;
    input >> point_count >> edge_count;
    std::vector<std::array<double, 2>> points(point_count);
    std::vector<std::array<uint32_t, 2>> edges(edge_count);
    for (auto& point : points) {
        input >> point[0] >> point[1];
    }
    for (auto& edge : edges) {
        input >> edge[0] >> edge[1];
    }
    ASSERT_TRUE(input);
    auto const saved = points;
    auto const mesh = client::buildRiverBedMesh({44'019, 11'251}, {44'020, 11'252}, points, edges,
        [](double, double) { return 0.0; });
    ASSERT_EQ(mesh.vertices.size(), points.size() + 1U);
    auto const& added = mesh.vertices.back();
    EXPECT_EQ(added[0], 44'019.593'75);
    EXPECT_EQ(added[1], 11'251.25);
    ASSERT_TRUE(client::detail::isRiverConstraintIntersection(points, edges, {added[0], added[1]}));
    EXPECT_EQ(points, saved);
    std::map<std::array<double, 2>, uint32_t> index;
    for (uint32_t i = 0U; i < mesh.vertices.size(); ++i) {
        index.emplace(std::array<double, 2>{mesh.vertices[i][0], mesh.vertices[i][1]}, i);
    }
    for (auto const& point : points) {
        EXPECT_TRUE(index.contains(point));
    }
    std::map<std::array<uint32_t, 2>, std::vector<size_t>> mesh_edges;
    std::vector<size_t> owners;
    std::array<double, 2> areas{};
    for (size_t i = 0U; i < mesh.triangles.size(); ++i) {
        auto const& t = mesh.triangles[i];
        auto const& a = mesh.vertices[t[0]];
        auto const& b = mesh.vertices[t[1]];
        auto const& c = mesh.vertices[t[2]];
        double const area = ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2.0;
        ASSERT_GT(area, 0.0);
        size_t const owner = (a[1] + b[1] + c[1]) / 3.0 < 11'251.75 ? 0U : 1U;
        EXPECT_TRUE(owner == 0U ? std::max({a[1], b[1], c[1]}) <= 11'251.75
            : std::min({a[1], b[1], c[1]}) >= 11'251.75);
        owners.push_back(owner);
        areas[owner] += area;
        for (size_t j = 0U; j < 3U; ++j) {
            mesh_edges[{std::min(t[j], t[(j + 1U) % 3U]), std::max(t[j], t[(j + 1U) % 3U])}].push_back(i);
        }
    }
    EXPECT_EQ(areas, (std::array<double, 2>{0.75, 0.25}));
    uint32_t const added_id = index.at({added[0], added[1]});
    for (auto const& edge : std::vector<std::array<uint32_t, 2>>{
        {index.at(points[0]), added_id}, {added_id, index.at(points[1])},
        {index.at(points[2]), added_id}, {added_id, index.at(points[3])},
        {index.at(points[4]), index.at(points[5])}}) {
        EXPECT_TRUE(mesh_edges.contains({std::min(edge[0], edge[1]), std::max(edge[0], edge[1])}));
    }
    uint32_t const barrier_a = index.at(points[4]), barrier_b = index.at(points[5]);
    auto const& barrier_faces = mesh_edges.at({std::min(barrier_a, barrier_b), std::max(barrier_a, barrier_b)});
    ASSERT_EQ(barrier_faces.size(), 2U);
    EXPECT_NE(owners[barrier_faces[0]], owners[barrier_faces[1]]);
    for (auto const& v : mesh.vertices) {
        EXPECT_EQ(v[0] * 32.0, std::round(v[0] * 32.0));
        EXPECT_EQ(v[1] * 32.0, std::round(v[1] * 32.0));
    }
    for (auto const& [edge, faces] : mesh_edges) {
        EXPECT_LE(faces.size(), 2U);
        if (faces.size() != 1U) {
            continue;
        }
        auto const& a = mesh.vertices[edge[0]];
        auto const& b = mesh.vertices[edge[1]];
        EXPECT_TRUE((a[0] == b[0] && (a[0] == 44'019 || a[0] == 44'020))
            || (a[1] == b[1] && (a[1] == 11'251 || a[1] == 11'252)));
    }
}

TEST(TerrainLodPresentationTest, RiverLatticeIntersectionRejectsUnattestedPoints)
{
    std::vector<std::array<double, 2>> const points{{0, 0}, {1, 1}, {0, 1}, {1, 0}};
    std::vector<std::array<uint32_t, 2>> const crossing{{0U, 1U}, {2U, 3U}};
    EXPECT_TRUE(client::detail::isRiverConstraintIntersection(points, crossing, {0.5, 0.5}));
    EXPECT_FALSE(client::detail::isRiverConstraintIntersection(points, crossing, {0.5, 0.5 + 1.0 / 32.0}));
    EXPECT_FALSE(client::detail::isRiverConstraintIntersection(points, crossing, {0.5, 0.5 + 1e-12}));
    EXPECT_FALSE(client::detail::isRiverConstraintIntersection(points, crossing, {0, 0}));
    std::vector<std::array<uint32_t, 2>> const collinear{{0U, 1U}, {1U, 0U}};
    EXPECT_FALSE(client::detail::isRiverConstraintIntersection(points, collinear, {0.5, 0.5}));
}

TEST(TerrainLodPresentationTest, RiverRenderNewAndMovedNodesSampleCanonicalBed)
{
    generation::GenerationConfiguration configuration;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto const atlas = generation::prepareAtlas(configuration);
    auto const height = [&](double const x, double const y) {
        return generation::v10L0ElevationAt(*atlas, x, y) + 513.0;
    };
    std::vector<std::array<double, 3>> const original{
        {44'019.531'25, 11'251.3125, height(44'019.531'25, 11'251.3125)},
        {44'019.765'972'222'22, 11'251.077'777'777'778, height(44'019.765'972'222'22, 11'251.077'777'777'778)},
    };
    auto positions = original;
    positions[1] = {44'019.781'25, 11'251.0625, -999.0};
    positions.push_back({44'019.593'75, 11'251.25, -999.0});
    client::TerrainLodRiverGeometryAudit audit;
    std::vector<std::array<double, 2>> calls;
    client::detail::resampleRiverRenderHeights(original, positions, [&](double const x, double const y) {
        calls.push_back({x, y});
        return height(x, y);
    }, audit);
    EXPECT_EQ(positions[0], original[0]);
    ASSERT_EQ(calls, (std::vector<std::array<double, 2>>{{44'019.781'25, 11'251.0625}, {44'019.593'75, 11'251.25}}));
    EXPECT_EQ(positions[1][2], height(positions[1][0], positions[1][1]));
    EXPECT_EQ(positions[2][2], height(positions[2][0], positions[2][1]));
    EXPECT_EQ(audit.maximum_xy_quantization_error_m,
        std::hypot(positions[1][0] - original[1][0], positions[1][1] - original[1][1]));
}

namespace {
void checkLocalPromotion(uint32_t px, uint32_t py, uint32_t qx)
{
    static auto const terrain = [] {
        generation::GenerationConfiguration configuration;
        configuration.seed = 42U;
        configuration.model_tag = generation::TerrainModelTag::V10;
        return std::make_unique<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    }();
    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    uint32_t const q2 = (qx + 1U) % 4'096U, outside = (qx + 24U) % 4'096U;
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain->identity()));
    for (uint32_t cx : {px, qx, q2, outside}) {
        for (auto offset : {std::array<int32_t, 2>{0,0}, std::array<int32_t, 2>{-1,0},
                std::array<int32_t, 2>{1,0}, std::array<int32_t, 2>{0,-1}, std::array<int32_t, 2>{0,1}}) {
            ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain->sampleSurface({
                (int32_t(cx) + offset[0] + 4'096) % 4'096, (int32_t(py) + offset[1] + 4'096) % 4'096, 0})));
        }
    }
    auto capture = [&](uint32_t x) -> Input {
        auto input = client::captureTerrainLodColumn(sparse,x,py);
        return input ? std::make_shared<client::TerrainLodColumnInput const>(*input) : nullptr;
    };
    std::vector<Input> initial{capture(px),capture(outside)};
    ASSERT_TRUE(initial[0]); ASSERT_TRUE(initial[1]);
    std::vector<Input> one = initial; one.push_back(capture(qx));
    std::vector<Input> two = one; two.push_back(capture(q2));
    ASSERT_TRUE(one.back()); ASSERT_TRUE(two.back());
    auto const base = client::buildTerrainLodSceneFromColumns(*terrain,px*16U,py*16U,initial,{},nullptr,{},false,
        client::TerrainLodColumnCoordinate{px,py});
    std::promise<void> release;
    auto gate = release.get_future();
    auto far = std::async(std::launch::async,[&] {
        gate.wait();
        return client::buildTerrainLodSceneFromColumns(*terrain,(px*16U+64U)%65'536U,py*16U,initial,{},base.get(),initial,false,
            client::TerrainLodColumnCoordinate{qx,py});
    });
    std::array const target1{client::TerrainLodColumnCoordinate{qx,py}}, target2{client::TerrainLodColumnCoordinate{q2,py}};
    auto first = client::prepareTerrainLodLocalScene(*terrain,*base,initial,one,target1);
    // Release before fatal assertions so the actual held worker always terminates.
    auto second = first.scene ? client::prepareTerrainLodLocalScene(*terrain,*first.scene,one,two,target2)
                              : client::TerrainLodLocalPreparation{};
    EXPECT_EQ(far.wait_for(std::chrono::seconds{0}),std::future_status::timeout);
    release.set_value();
    auto prepared_far = far.get();
    ASSERT_TRUE(first.scene); ASSERT_TRUE(first.unit); ASSERT_TRUE(second.scene); ASSERT_TRUE(second.unit);
    auto const fresh_live = client::buildTerrainLodSceneFromColumns(
        *terrain, px * 16U, py * 16U, two, {}, nullptr, {}, true, base->visual_center);
    expectSameTerrainScene(*second.scene,*fresh_live);
    std::weak_ptr<client::TerrainLodScene const> old_scene = first.scene;
    first.scene.reset();
    EXPECT_TRUE(old_scene.expired()); // The second scene/unit must not retain its parent.
    std::array const targets{target1[0],target2[0]};
    std::array const constraints{first.unit,second.unit};
    auto rebased = client::prepareTerrainLodLocalScene(*terrain,*prepared_far,initial,two,targets,constraints);
    ASSERT_TRUE(rebased.scene); ASSERT_TRUE(rebased.unit);
    auto fresh = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, two, {}, nullptr, {}, true, prepared_far->visual_center);
    expectSameTerrainScene(*rebased.scene,*fresh);
    EXPECT_TRUE(client::terrainLodLocalSourcesCurrent(*rebased.unit,sparse));
    // An unrelated source edit refreshes far, while the local region stays compatible.
    auto column = sparse.authoritativeColumn(outside*16U,py*16U);
    ASSERT_TRUE(column);
    auto prior = sparse.queryBlock({outside*16U,py*16U,column->highest_solid_z}); ASSERT_TRUE(prior.block);
    ASSERT_TRUE(sparse.applyEditDelta({int32_t(outside*16U),int32_t(py*16U),column->highest_solid_z},
        *prior.block == shared::Block::Snow ? shared::Block::Stone : shared::Block::Snow,0U,1U,1U));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*second.scene,sparse));
    auto refreshed = initial; refreshed[1] = capture(outside); ASSERT_TRUE(refreshed[1]);
    auto refreshed_far = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, refreshed, {}, prepared_far.get(), {}, false, prepared_far->visual_center);
    auto merged = refreshed; merged.push_back(one.back()); merged.push_back(two.back());
    auto refreshed_rebase = client::prepareTerrainLodLocalScene(*terrain,*refreshed_far,refreshed,merged,targets,constraints);
    ASSERT_TRUE(refreshed_rebase.scene);
    fresh = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, merged, {}, nullptr, {}, true, prepared_far->visual_center);
    expectSameTerrainScene(*refreshed_rebase.scene,*fresh);
    std::array const retired{initial[0]};
    auto retired_far = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, retired, {}, refreshed_far.get(), retired, false, prepared_far->visual_center);
    std::array const retired_merged{initial[0],one.back(),two.back()};
    auto retired_rebase = client::prepareTerrainLodLocalScene(*terrain,*retired_far,retired,retired_merged,targets,constraints);
    ASSERT_TRUE(retired_rebase.scene);
    fresh = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, retired_merged, {}, nullptr, {}, true, prepared_far->visual_center);
    expectSameTerrainScene(*retired_rebase.scene,*fresh);
    EXPECT_EQ(retired_rebase.scene->actual_column_count,3U);
    // A changed boundary dependency inside the halo must reject composition.
    auto changed = std::make_shared<client::TerrainLodColumnInput>(*initial[0]);
    changed->edge_heights[0][0]++;
    auto bad = refreshed; bad[0] = changed;
    auto bad_far = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, bad, {}, nullptr, {}, false, prepared_far->visual_center);
    auto bad_merged = bad; bad_merged.push_back(one.back()); bad_merged.push_back(two.back());
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(*terrain,*bad_far,bad,bad_merged,targets,constraints).scene);
    auto stale = sparse.authoritativeColumn(qx*16U,py*16U); ASSERT_TRUE(stale);
    ASSERT_TRUE(sparse.applyEditDelta({int32_t(qx*16U),int32_t(py*16U),stale->highest_solid_z+1},shared::Block::Stone,0U,1U,1U));
    EXPECT_FALSE(client::terrainLodLocalSourcesCurrent(*rebased.unit,sparse));
    std::array const edited_inputs{capture(px),capture(qx),capture(q2)};
    ASSERT_TRUE(edited_inputs[0]); ASSERT_TRUE(edited_inputs[1]); ASSERT_TRUE(edited_inputs[2]);
    auto edit_patch = client::buildTerrainLodColumnPatch(terrain->identity(),edited_inputs);
    std::vector<client::StoneIndirectDraws::Range> edit_ranges(retired_rebase.scene->groups.size());
    auto edited_base = client::detail::terrainLodReplacementPlan(*retired_rebase.scene,edit_ranges,*edit_patch).scene;
    ASSERT_NE(edited_base->local_layout,nullptr);
    auto repaired_edit = client::prepareTerrainLodLocalScene(*terrain,*edited_base,edited_inputs,edited_inputs,targets);
    ASSERT_TRUE(repaired_edit.scene);
    auto fresh_edit = client::buildTerrainLodSceneFromColumns(
        *terrain, prepared_far->center_x, py * 16U, edited_inputs, {}, nullptr, {}, true, prepared_far->visual_center);
    expectSameTerrainScene(*repaired_edit.scene,*fresh_edit);
    EXPECT_TRUE(client::terrainLodLocalSourcesCurrent(*repaired_edit.unit,sparse));

    auto incompatible = *prepared_far; incompatible.local_layout.reset();
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(*terrain,incompatible,initial,two,targets,constraints).scene);
    incompatible = *prepared_far; incompatible.visual_center = client::TerrainLodColumnCoordinate{(qx+128U)%4'096U,py};
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(*terrain,incompatible,initial,two,targets,constraints).scene);
}
} // namespace

TEST(TerrainLodPresentationTest, LocalPromotionRebasesHeldFarAndRetiresUnrelatedAuthority)
{
    checkLocalPromotion(2'048U,2'048U,2'052U);
}
TEST(TerrainLodPresentationTest, LocalPromotionConfluenceMatchesCompleteFreshGeometry)
{
    checkLocalPromotion(2'176U,1'888U,2'180U);
}
TEST(TerrainLodPresentationTest, LocalPromotionWrapMatchesCompleteFreshGeometry)
{
    checkLocalPromotion(4'094U,2'048U,2U);
}

TEST(TerrainLodPresentationTest, FineL1HeightErrorRetainsPlanesAndDetectsInteriorRidges)
{
    shared::TerrainLodArtifact child;
    child.spacing = 16U;
    child.extent = 64U;
    child.side = 5U;
    for (uint32_t y = 0U; y < child.side; ++y) {
        for (uint32_t x = 0U; x < child.side; ++x) {
            child.nodes.push_back({.sample = {.height = 100.0 + x * 2.0 + y * 3.0}});
        }
    }
    std::array<int32_t, 4> const corners{101 * 32, 109 * 32, 113 * 32, 121 * 32};
    EXPECT_DOUBLE_EQ(client::detail::terrainLodChildHeightError(corners, child), 0.0);
    child.nodes[6U].sample.height += 1.0;
    EXPECT_DOUBLE_EQ(client::detail::terrainLodChildHeightError(corners, child), 1.0);
    child.nodes[6U].sample.height += 0.5;
    EXPECT_DOUBLE_EQ(client::detail::terrainLodChildHeightError(corners, child), 1.5);
}

TEST(TerrainLodPresentationTest, FineRidgeOutsideExactDiskRefinesAndReducesActualMeshHeightError)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    shared::TerrainWorld const world{generation::prepareAtlas(configuration)};
    auto const child = shared::buildTerrainLod(world, shared::TerrainLodLevel::L2, 52'352U, 5'696U, 64U);
    std::array<int32_t, 4> corners{};
    for (uint32_t i = 0U; i < 4U; ++i) {
        corners[i] = static_cast<int32_t>((child->nodes[(i / 2U) * 20U + (i % 2U) * 4U].sample.height + 1.0) * 32.0);
    }
    double const old_error = client::detail::terrainLodChildHeightError(corners, *child);
    RecordProperty("ridge_parent_error_blocks", old_error);
    EXPECT_DOUBLE_EQ(old_error, 1.5);
    auto const start = std::chrono::steady_clock::now();
    auto const scene = client::buildTerrainLodScene(world, 52'372U, 4'937U);
    RecordProperty("fine_ridge_scene_seconds", std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count());
    EXPECT_EQ(scene->actual_column_count, 0U);
    EXPECT_EQ(scene->identity, world.identity());
    RecordProperty("fine_configuration_identity", world.identity().configuration_identity);
    RecordProperty("scene_group_count", scene->groups.size());
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            EXPECT_NE(std::ranges::find(scene->leaves, client::TerrainLodRegion{
                shared::TerrainLodLevel::L2, 3'272U + x, 356U + y}), scene->leaves.end());
        }
    }
    double maximum = 0.0;
    for (uint32_t y = 1U; y < 4U; ++y) {
        for (uint32_t x = 1U; x < 4U; ++x) {
            auto const top = topHeightAt(*scene, 52'352U + x * 16U, 5'696U + y * 16U);
            ASSERT_TRUE(top);
            maximum = std::max(maximum, std::abs(*top - child->nodes[y * 5U + x].sample.height - 1.0));
        }
    }
    RecordProperty("ridge_refined_interior_mesh_error_blocks", maximum);
    EXPECT_LE(maximum, 1.0);
    EXPECT_LT(maximum, old_error);
    std::set<std::array<uint32_t, 2>> tested_parents;
    uint32_t mixed_edges = 0U;
    for (auto const leaf : scene->leaves) {
        if (leaf.level == shared::TerrainLodLevel::L0) {
            continue;
        }
        uint32_t const extent = client::terrainLodRegionExtent(leaf.level, scene->root_extent);
        uint32_t const ox = leaf.x * extent / 64U * 64U;
        uint32_t const oy = leaf.y * extent / 64U * 64U;
        double const dx = std::max({double(ox) - 52'372.0, 52'372.0 - double(ox + 64U), 0.0});
        double const dy = std::max({double(oy) - 4'937.0, 4'937.0 - double(oy + 64U), 0.0});
        if (std::hypot(dx, dy) >= 672.0) {
            tested_parents.insert({ox, oy});
        }
        if (leaf.level != shared::TerrainLodLevel::L1 || mixed_edges != 0U || ox == 0U) {
            continue;
        }
        // Test an actual L1/L2 seam, using decoded production triangles on both sides.
        auto const neighbor = client::TerrainLodRegion{shared::TerrainLodLevel::L2,
            (ox - 1U) / 16U, (oy + 32U) / 16U};
        if (std::ranges::find(scene->leaves, neighbor) == scene->leaves.end()) {
            continue;
        }
        for (uint32_t along = 1U; along < 64U; ++along) {
            auto const left = topHeightAt(*scene, double(ox) - 0.0001, oy + along);
            auto const right = topHeightAt(*scene, double(ox) + 0.0001, oy + along);
            ASSERT_TRUE(left);
            ASSERT_TRUE(right);
            EXPECT_NEAR(*left, *right, 0.01);
        }
        ++mixed_edges;
    }
    EXPECT_GT(mixed_edges, 0U);
    RecordProperty("error_checked_l1_cells", tested_parents.size());
    RecordProperty("expected_canonical_error_probe_samples", tested_parents.size() * 25U);
    RecordProperty("mixed_l1_l2_seams_checked", mixed_edges);
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_THROW(static_cast<void>(client::buildTerrainLodScene(
        world, 52'372U, 4'937U, {}, cancelled.get_token())), std::runtime_error);
}

TEST(TerrainLodPresentationTest, SharedMeshNormalsRefreshReusedNeighborsAcrossGroupsAndWrap)
{
    auto const make_scene = [](int32_t origin) {
        client::TerrainLodScene scene;
        auto const add = [&](std::array<std::array<int32_t, 3>, 3> const& vertices) {
            auto face = client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Snow);
            client::TerrainLodDrawGroup group;
            group.range = {static_cast<uint32_t>(scene.groups.size()), 1U};
            group.origin_x = static_cast<uint32_t>(vertices[0][0]) & 0xffffU;
            group.extent = 16U;
            group.minimum_z = 100;
            group.maximum_z = 132;
            group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(
                std::vector<client::StoneFaceInstance>{face});
            scene.groups.push_back(std::move(group));
        };
        add({{{origin, 0, 100 * 32}, {origin + 16, 0, 100 * 32}, {origin + 16, 16, 100 * 32}}});
        int32_t const right = (origin + 16) % 65'536;
        add({{{right, 0, 100 * 32}, {right + 16, 0, 116 * 32}, {right, 16, 100 * 32}}});
        scene.logical_face_count = 2U;
        return scene;
    };
    for (int32_t const origin : {0, 65'520}) {
        auto scene = make_scene(origin);
        auto const before = scene.groups[0].face_storage;
        client::detail::prepareTerrainLodMeshNormals(scene, {});
        auto const& a = scene.groups[0].face_storage->front();
        auto const& b = scene.groups[1].face_storage->front();
        EXPECT_EQ(a.terrain_normals[3], 1U);
        EXPECT_EQ(a.terrain_normals[1], b.terrain_normals[0]);
        EXPECT_EQ(a.terrain_normals[2], b.terrain_normals[2]);
        EXPECT_EQ(a.terrain_normals[0], 0U); // Exact +Z, distinguished by the validity word.
        EXPECT_EQ(a.terrain_normals[1], 0x0000'd556U); // Shared area sum (-1, 0, 2).
        EXPECT_EQ(before->front().terrain_normals[3], 0U);
        auto geometry_only = a;
        geometry_only.terrain_normals = {};
        EXPECT_EQ(geometry_only, before->front());
        auto const unchanged = scene.groups[0].face_storage;
        client::detail::prepareTerrainLodMeshNormals(scene, {});
        EXPECT_EQ(scene.groups[0].face_storage, unchanged);
        auto edited = scene;
        auto changed = std::make_shared<std::vector<client::StoneFaceInstance>>(*edited.groups[1].face_storage);
        int32_t const right = (origin + 16) % 65'536;
        auto replacement = client::StoneFaceInstance::fromTerrainTriangle(
            {{{right, 0, 100 * 32}, {right + 16, 0, 132 * 32}, {right, 16, 100 * 32}}}, shared::Block::Snow);
        changed->front() = replacement;
        edited.groups[1].face_storage = changed;
        client::detail::prepareTerrainLodMeshNormals(edited, {});
        EXPECT_NE(edited.groups[0].face_storage, unchanged);
        EXPECT_NE(edited.groups[0].face_storage->front().terrain_normals[1], a.terrain_normals[1]);
        EXPECT_EQ(edited.groups[0].face_storage->front().terrain_normals[1],
            edited.groups[1].face_storage->front().terrain_normals[0]);
        EXPECT_EQ(scene.groups[0].face_storage, unchanged);
        auto fresh = make_scene(origin);
        fresh.groups[1].face_storage = changed;
        client::detail::prepareTerrainLodMeshNormals(fresh, {});
        EXPECT_EQ(logicalFaces(edited), logicalFaces(fresh));
        std::stop_source cancelled;
        cancelled.request_stop();
        EXPECT_THROW(client::detail::prepareTerrainLodMeshNormals(edited, cancelled.get_token()), std::runtime_error);
    }
}

TEST(TerrainLodPresentationTest, MeshNormalsPreserveVoxelAndVerticalSkirtPayloads)
{
    client::TerrainLodScene scene;
    std::vector<client::StoneFaceInstance> faces{
        {.x = 1, .y = 2, .z = 3, .direction = 5U, .material_id = static_cast<uint32_t>(shared::Block::Sand)},
        client::StoneFaceInstance::fromTerrainTriangle(
            {{{0, 0, 100 * 32}, {0, 16, 100 * 32}, {0, 16, 116 * 32}}}, shared::Block::Stone),
    };
    client::TerrainLodDrawGroup group;
    group.range = {0U, 2U};
    group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(faces);
    scene.groups.push_back(group);
    scene.logical_face_count = 2U;
    client::detail::prepareTerrainLodMeshNormals(scene, {});
    EXPECT_EQ(logicalFaces(scene), faces);
    EXPECT_EQ(scene.groups.front().face_storage, group.face_storage);
}

TEST(TerrainLodPresentationTest, IncrementalNormalsIncludeRemovedAddedAndReorderedIncidentTriangles)
{
    auto const group = [](std::array<std::array<int32_t, 3>, 3> const& vertices) {
        client::TerrainLodDrawGroup result;
        result.range = {0U, 1U};
        result.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(
            std::vector{client::StoneFaceInstance::fromTerrainTriangle(vertices, shared::Block::Snow)});
        return result;
    };
    client::TerrainLodScene base;
    base.groups = {
        group({{{0, 0, 3200}, {16, 0, 3200}, {16, 16, 3200}}}),
        group({{{16, 0, 3200}, {32, 0, 3712}, {16, 16, 3200}}}),
        group({{{0, -16, 3200}, {16, 0, 3200}, {0, 0, 3200}}}),
        group({{{1024, 0, 3200}, {1040, 0, 3200}, {1040, 16, 3200}}}),
    };
    base.logical_face_count = base.groups.size();
    client::detail::prepareTerrainLodMeshNormals(base, {});
    auto const remote = base.groups.back().face_storage;
    auto changed = base;
    changed.groups.erase(changed.groups.begin() + 2);
    changed.groups[1] = group({{{16, 0, 3200}, {32, 0, 4224}, {16, 16, 3200}}});
    changed.groups.push_back(group({{{16, -16, 3456}, {32, 0, 3712}, {16, 0, 3200}}}));
    changed.logical_face_count = changed.groups.size();
    auto cold = changed;
    cold.normal_context.reset();
    client::detail::prepareTerrainLodMeshNormals(changed, {});
    client::detail::prepareTerrainLodMeshNormals(cold, {});
    EXPECT_EQ(logicalFaces(changed), logicalFaces(cold));
    EXPECT_NE(changed.groups.front().face_storage, base.groups.front().face_storage);
    EXPECT_EQ(changed.groups[2].face_storage, remote);

    std::swap(changed.groups[0], changed.groups[1]);
    cold = changed;
    cold.normal_context.reset();
    client::detail::prepareTerrainLodMeshNormals(changed, {});
    client::detail::prepareTerrainLodMeshNormals(cold, {});
    EXPECT_EQ(logicalFaces(changed), logicalFaces(cold));

    std::weak_ptr<std::vector<client::StoneFaceInstance> const> payload;
    {
        client::TerrainLodScene owner;
        owner.groups.push_back(group({{{0, 0, 3200}, {16, 0, 3200}, {16, 16, 3200}}}));
        owner.logical_face_count = 1U;
        client::detail::prepareTerrainLodMeshNormals(owner, {});
        payload = owner.groups.front().face_storage;
        changed.normal_context = owner.normal_context;
    }
    EXPECT_TRUE(payload.expired()); // Incidence metadata cannot retain departed scene geometry.
    cold = changed;
    cold.normal_context.reset();
    client::detail::prepareTerrainLodMeshNormals(changed, {});
    client::detail::prepareTerrainLodMeshNormals(cold, {});
    EXPECT_EQ(logicalFaces(changed), logicalFaces(cold));
}

TEST(TerrainLodPresentationTest, VoxelReplacementRetainsNormalContextWithoutKeepingOldFaces)
{
    client::TerrainLodScene active;
    auto const terrain_face = client::StoneFaceInstance::fromTerrainTriangle(
        {{{0, 0, 3200}, {16, 0, 3200}, {16, 16, 3200}}}, shared::Block::Snow);
    auto const voxel_face = client::StoneFaceInstance{.x = 16, .direction = 5U};
    active.groups = {
        {.range = {0U, 1U}, .extent = 16U, .face_storage =
            std::make_shared<std::vector<client::StoneFaceInstance> const>(std::vector{terrain_face})},
        {.range = {1U, 1U}, .origin_x = 16U, .extent = 16U, .face_storage =
            std::make_shared<std::vector<client::StoneFaceInstance> const>(std::vector{voxel_face})},
    };
    active.logical_face_count = 2U;
    client::detail::prepareTerrainLodMeshNormals(active, {});
    ASSERT_NE(active.normal_context, nullptr);
    std::weak_ptr<std::vector<client::StoneFaceInstance> const> old_voxel = active.groups[1].face_storage;
    auto const normal_context = active.normal_context;
    auto replacement = voxel_face;
    replacement.z = 1;
    client::TerrainLodScene patch;
    patch.groups = {{.range = {0U, 1U}, .origin_x = 16U, .extent = 16U, .face_storage =
        std::make_shared<std::vector<client::StoneFaceInstance> const>(std::vector{replacement})}};
    patch.logical_face_count = 1U;
    std::array const ranges{active.groups[0].range, active.groups[1].range};
    auto const merged = client::detail::terrainLodReplacementPlan(active, ranges, patch).scene;
    active = {};
    EXPECT_TRUE(old_voxel.expired());
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->normal_context, normal_context);
    auto incremental = *merged;
    auto fresh = incremental;
    fresh.normal_context.reset();
    client::detail::prepareTerrainLodMeshNormals(incremental, {});
    client::detail::prepareTerrainLodMeshNormals(fresh, {});
    EXPECT_EQ(logicalFaces(incremental), logicalFaces(fresh));
}

TEST(TerrainLodPresentationTest, FineLocalSelectionReusePreservesMaterialsOrderAndContextFallbacks)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    auto const terrain = std::make_unique<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    uint32_t const px = 2'048U, py = 2'048U, qx = 2'052U, distant = 2'176U, recenter_x = px + 43U;
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain->identity()));
    for (uint32_t const cx : {px, qx, distant, recenter_x}) {
        for (auto const offset : {std::array<int32_t, 2>{0, 0}, std::array<int32_t, 2>{-1, 0},
                std::array<int32_t, 2>{1, 0}, std::array<int32_t, 2>{0, -1}, std::array<int32_t, 2>{0, 1}}) {
            ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain->sampleSurface(
                {static_cast<int32_t>(cx) + offset[0], static_cast<int32_t>(py) + offset[1], 0})));
        }
    }
    auto const capture = [&](uint32_t const x) -> Input {
        auto input = client::captureTerrainLodColumn(sparse, x, py);
        return input ? std::make_shared<client::TerrainLodColumnInput const>(*input) : nullptr;
    };
    std::vector<Input> const initial{capture(px)}, expanded{initial.front(), capture(qx)};
    ASSERT_TRUE(initial.front());
    ASSERT_TRUE(expanded.back());
    auto const base = client::buildTerrainLodSceneFromColumns(*terrain, px * 16U, py * 16U, initial,
        {}, nullptr, {}, false, client::TerrainLodColumnCoordinate{px, py});
    std::array const target{client::TerrainLodColumnCoordinate{qx, py}};
    auto const promoted = client::prepareTerrainLodLocalScene(*terrain, *base, initial, expanded, target);
    ASSERT_TRUE(promoted.scene);
    auto const fresh = client::buildTerrainLodSceneFromColumns(*terrain, px * 16U, py * 16U, expanded,
        {}, nullptr, {}, true, base->visual_center);
    expectSameTerrainScene(*promoted.scene, *fresh);

    // A different visual context can keep identical physical selection. It must
    // pass through fresh context validation, preserving exact center materials/order.
    auto shifted = *promoted.scene;
    shifted.visual_center = client::TerrainLodColumnCoordinate{px + 1U, py};
    auto const context = client::prepareTerrainLodLocalScene(*terrain, shifted, expanded, expanded, target);
    ASSERT_TRUE(context.scene);
    auto const context_fresh = client::buildTerrainLodSceneFromColumns(*terrain, px * 16U, py * 16U, expanded,
        {}, nullptr, {}, true, shifted.visual_center);
    expectSameTerrainScene(*context.scene, *context_fresh);

    // Transient recentering may reuse the existing selection beyond the visual
    // detail disk when the new forcing is a safe superset in its retained L2 halo.
    std::vector<Input> const recenter_columns{initial.front(), capture(recenter_x)};
    ASSERT_TRUE(recenter_columns.back());
    auto const recenter_base = client::buildTerrainLodSceneFromColumns(*terrain, px * 16U, py * 16U,
        initial, {}, nullptr, {}, true, client::TerrainLodColumnCoordinate{px, py});
    ASSERT_EQ(recenter_base->root_extent, 256U);
    ASSERT_EQ(client::terrainLodRegionExtent(shared::TerrainLodLevel::L2, recenter_base->root_extent), 16U);
    client::TerrainLodColumnCoordinate const recenter_target{recenter_x, py};
    std::array const recenter_targets{recenter_target};
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(
        *terrain, *recenter_base, initial, recenter_columns, recenter_targets).scene);
    auto const transient = client::prepareTerrainLodLocalScene(
        *terrain, *recenter_base, initial, recenter_columns, recenter_targets, {}, {}, true);
    ASSERT_TRUE(transient.scene);
    ASSERT_TRUE(transient.unit);
    EXPECT_FALSE(transient.scene->visual_center);
    EXPECT_TRUE(client::terrainLodLocalSourcesCurrent(*transient.unit, sparse));
    EXPECT_GT(transient.scene->selection_audit.reused_l0_root_count, 0U);
    auto const transient_fresh = client::buildTerrainLodSceneFromColumns(*terrain,
        recenter_base->center_x, recenter_base->center_y, recenter_columns, {}, nullptr, {}, true, std::nullopt);
    expectSameTerrainScene(*transient.scene, *transient_fresh);

    // Removing a previous forcing source cannot silently retain/drop its actual
    // column outside this local unit. Existing outside-mask guards still reject it.
    std::array const retained_target{client::TerrainLodColumnCoordinate{px, py}};
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(
        *terrain, *promoted.scene, expanded, initial, retained_target).scene);

    std::array<Input, 1> const distant_columns{capture(distant)};
    ASSERT_TRUE(distant_columns.front());
    auto const coarse = client::buildTerrainLodSceneFromColumns(*terrain, px * 16U, py * 16U, {},
        {}, nullptr, {}, false, client::TerrainLodColumnCoordinate{distant, py});
    std::array const distant_target{client::TerrainLodColumnCoordinate{distant, py}};
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(
        *terrain, *coarse, {}, distant_columns, distant_target).scene);

    auto other_atlas = std::make_shared<generation::GlobalAtlas>(*terrain->atlas());
    shared::TerrainWorld const other{std::move(other_atlas)};
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(other, *base, initial, expanded, target).scene);
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_THROW(static_cast<void>(client::prepareTerrainLodLocalScene(
        *terrain, *base, initial, expanded, target, {}, cancelled.get_token())), std::runtime_error);
}

TEST(TerrainLodPresentationTest, TransientLocalRecenterRebuildsMixedLodRimFromSameSelection)
{
    auto const terrain = presentationWorld(256U);
    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    uint32_t const visual_x = 2'048U, visual_y = 2'048U;
    uint32_t const base_x = visual_x * 16U + 1U, base_y = visual_y * 16U;
    std::vector<client::TerrainLodColumnCoordinate> const targets{{visual_x + 41U, visual_y}};
    std::set<std::pair<uint32_t, uint32_t>> surfaces;
    for (auto const coordinate : {client::TerrainLodColumnCoordinate{visual_x + 39U, visual_y},
             client::TerrainLodColumnCoordinate{visual_x + 40U, visual_y}}) {
        surfaces.emplace(coordinate.x, coordinate.y);
    }
    for (auto const target : targets) {
        surfaces.emplace(target.x, target.y);
    }
    auto add_neighbors = [&](uint32_t const x, uint32_t const y) {
        for (auto const [dx, dy] : {std::pair<int32_t, int32_t>{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
            surfaces.emplace(static_cast<uint32_t>((static_cast<int32_t>(x) + dx + 4'096) % 4'096),
                static_cast<uint32_t>((static_cast<int32_t>(y) + dy + 4'096) % 4'096));
        }
    };
    std::vector<std::pair<uint32_t, uint32_t>> profile_columns(surfaces.begin(), surfaces.end());
    for (auto const [x, y] : profile_columns) {
        add_neighbors(x, y);
    }
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain->identity()));
    for (auto const [x, y] : surfaces) {
        ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain->sampleSurface(
            {static_cast<int32_t>(x), static_cast<int32_t>(y), 0})));
    }
    auto const capture = [&](uint32_t const x, uint32_t const y) -> Input {
        auto input = client::captureTerrainLodColumn(sparse, x, y);
        return input ? std::make_shared<client::TerrainLodColumnInput const>(*input) : nullptr;
    };
    std::vector<Input> initial{capture(visual_x + 39U, visual_y), capture(visual_x + 40U, visual_y)};
    ASSERT_TRUE(initial[0]);
    ASSERT_TRUE(initial[1]);
    std::vector<Input> expanded = initial;
    for (auto const target : targets) {
        expanded.push_back(capture(target.x, target.y));
        ASSERT_TRUE(expanded.back());
    }
    client::TerrainLodColumnCoordinate const visual_center{visual_x, visual_y};
    auto const base = client::buildTerrainLodSceneFromColumns(
        *terrain, base_x, base_y, initial, {}, nullptr, {}, true, visual_center);
    ASSERT_EQ(base->root_extent, 256U);
    EXPECT_FALSE(client::prepareTerrainLodLocalScene(*terrain, *base, initial, expanded, targets).scene);
    auto const transient = client::prepareTerrainLodLocalScene(
        *terrain, *base, initial, expanded, targets, {}, {}, true);
    ASSERT_TRUE(transient.scene);
    ASSERT_TRUE(transient.unit);
    EXPECT_FALSE(transient.scene->visual_center);
    EXPECT_GT(transient.scene->selection_audit.reused_l0_root_count, 0U);
    EXPECT_TRUE(client::terrainLodLocalSourcesCurrent(*transient.unit, sparse));
    auto const fresh = client::buildTerrainLodSceneFromColumns(
        *terrain, base_x, base_y, expanded, {}, nullptr, {}, true, std::nullopt);
    expectSameTerrainScene(*transient.scene, *fresh);
}

TEST(TerrainLodPresentationTest, RepeatedTransientRecenterFallsBackWhenDroppingRetiredL3)
{
    auto const terrain = presentationWorld(256U);
    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    uint32_t const visual_x = 2'048U, visual_y = 2'048U;
    uint32_t const base_x = visual_x * 16U + 1U, base_y = visual_y * 16U;
    uint32_t const retired_x = visual_x + 39U, retired_y = visual_y;
    uint32_t const target_x = visual_x + 41U;
    std::set<std::pair<uint32_t, uint32_t>> surfaces;
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            surfaces.emplace(static_cast<uint32_t>(static_cast<int32_t>(retired_x) + dx),
                static_cast<uint32_t>(static_cast<int32_t>(retired_y) + dy));
        }
    }
    surfaces.emplace(target_x, retired_y);
    auto add_neighbors = [&](uint32_t const x, uint32_t const y) {
        for (auto const [dx, dy] : {std::pair<int32_t, int32_t>{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
            surfaces.emplace(static_cast<uint32_t>((static_cast<int32_t>(x) + dx + 4'096) % 4'096),
                static_cast<uint32_t>((static_cast<int32_t>(y) + dy + 4'096) % 4'096));
        }
    };
    std::vector<std::pair<uint32_t, uint32_t>> profile_columns(surfaces.begin(), surfaces.end());
    for (auto const [x, y] : profile_columns) {
        add_neighbors(x, y);
    }
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain->identity()));
    for (auto const [x, y] : surfaces) {
        ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain->sampleSurface(
            {static_cast<int32_t>(x), static_cast<int32_t>(y), 0})));
    }
    auto const capture = [&](uint32_t const x, uint32_t const y) -> Input {
        auto input = client::captureTerrainLodColumn(sparse, x, y);
        return input ? std::make_shared<client::TerrainLodColumnInput const>(*input) : nullptr;
    };
    std::vector<Input> previous_columns;
    std::vector<Input> current_columns;
    std::vector<Input> base_columns;
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            Input const input = capture(static_cast<uint32_t>(static_cast<int32_t>(retired_x) + dx),
                static_cast<uint32_t>(static_cast<int32_t>(retired_y) + dy));
            ASSERT_TRUE(input);
            previous_columns.push_back(input);
            if (dx == 0 && dy == 0) {
                continue;
            }
            base_columns.push_back(input);
            current_columns.push_back(input);
        }
    }
    Input const target = capture(target_x, retired_y);
    ASSERT_TRUE(target);
    current_columns.push_back(target);

    client::TerrainLodColumnCoordinate const visual_center{visual_x, visual_y};
    auto const previous = client::buildTerrainLodSceneFromColumns(*terrain, base_x, base_y,
        previous_columns, {}, nullptr, {}, true, visual_center);
    ASSERT_EQ(previous->root_extent, 256U);
    ASSERT_TRUE(std::ranges::find(previous->leaves,
        client::TerrainLodRegion{shared::TerrainLodLevel::L3, retired_x, retired_y}) != previous->leaves.end());
    auto const previous_exact = std::ranges::find_if(previous->groups, [&](auto const& group) {
        return group.origin_x == retired_x * 16U && group.origin_y == retired_y * 16U;
    });
    ASSERT_NE(previous_exact, previous->groups.end());

    std::array const targets{client::TerrainLodColumnCoordinate{target_x, retired_y}};
    auto const transient = client::prepareTerrainLodLocalScene(*terrain, *previous,
        base_columns, current_columns, targets, {}, {}, true);
    EXPECT_FALSE(transient.scene);
    EXPECT_FALSE(transient.unit);

    auto const fresh = client::buildTerrainLodSceneFromColumns(*terrain, base_x, base_y,
        current_columns, {}, nullptr, {}, true, std::nullopt);
    ASSERT_TRUE(std::ranges::find(fresh->leaves,
        client::TerrainLodRegion{shared::TerrainLodLevel::L2, retired_x, retired_y}) != fresh->leaves.end());
    auto const fresh_rim = std::ranges::find_if(fresh->groups, [&](auto const& group) {
        return group.origin_x == retired_x * 16U && group.origin_y == retired_y * 16U;
    });
    ASSERT_NE(fresh_rim, fresh->groups.end());
    auto const old_faces = client::terrainLodGroupFaces(*previous_exact);
    auto const fresh_faces = client::terrainLodGroupFaces(*fresh_rim);
    EXPECT_FALSE(old_faces.size() == fresh_faces.size()
        && (old_faces.empty() || std::memcmp(old_faces.data(), fresh_faces.data(), old_faces.size_bytes()) == 0));
}
