#include <client/TerrainLodRuntime.hpp>
#if defined(MC_TERRAIN_RUNTIME_CPU_CALLTHROUGH)
#include <client/render/ShaderAssets.hpp>
#endif
#include <client/render/TerrainLodUploadPlan.hpp>
#if defined(MC_TERRAIN_RUNTIME_CPU_CALLTHROUGH)
#include <client/render/VulkanRenderer.hpp>
#endif
#include <shared/world/ChunkMesher.hpp>
#if defined(MC_TERRAIN_RUNTIME_CPU_CALLTHROUGH)
#include <generation/Generation.hpp>
#endif

#include <gtest/gtest.h>
#include <testsupport/TerrainFixtures.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace client {
namespace {

class IncrementalSources final {
public:
    IncrementalSources()
        : identity(testsupport::makeTerrainIdentity(42U, 1U)),
          world({}, shared::SparseWorldGenerationMode::MaterializedOnly)
    {
        EXPECT_TRUE(world.setTerrainIdentity(identity));
    }

    void publish(uint32_t x, uint32_t y)
    {
        std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
        heights.fill(385U);
        auto tile = std::make_unique<shared::SurfaceTile>(testsupport::makeSurfaceTile(
            { static_cast<int32_t>(x), static_cast<int32_t>(y) }, identity, heights));
        EXPECT_TRUE(world.publishAuthoritativeSurface(*tile));
    }

    void cross(uint32_t x, uint32_t y)
    {
        for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
                std::array{0, -1}, std::array{0, 1}}) {
            publish(static_cast<uint32_t>((static_cast<int32_t>(x) + offset[0] + 4'096) % 4'096),
                static_cast<uint32_t>((static_cast<int32_t>(y) + offset[1] + 4'096) % 4'096));
        }
    }

    void capture(uint32_t x, uint32_t y)
    {
        auto captured = captureTerrainLodColumnForBatch(world, x, y, batch);
        ASSERT_TRUE(captured);
        columns.push_back(captured);
        index.upsert(std::move(captured));
    }

    void compare(bool scan_residents = true)
    {
        index.select(world, scan_residents, selection);
        auto const expected = staleCapturedTerrainLodColumns(columns, world, batch);
        auto const actual = staleCapturedTerrainLodColumns(selection.columns, world, batch);
        EXPECT_EQ(actual, expected);
        for (auto const coordinate : expected) {
            index.remove({coordinate.x, coordinate.y});
        }
        std::erase_if(columns, [&expected](auto const& column) {
            auto const& input = column->input();
            return std::ranges::find(expected, TerrainLodColumnCoordinate{input.x, input.y}) != expected.end();
        });
    }

    shared::TerrainIdentity identity;
    shared::SparseWorld world;
    TerrainLodAuthorityBatch batch;
    TerrainLodSourceIndex index;
    TerrainLodSourceIndex::Selection selection;
    std::vector<std::shared_ptr<TerrainLodCapturedColumn const>> columns;
};

TEST(TerrainLodRuntimeTest, IncrementalUnrelatedAuthorityEventsSelectNoCapturedProfiles)
{
    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.cross(10U, 10U);
    fixture.capture(0U, 0U);
    fixture.capture(10U, 10U);
    fixture.compare();
    EXPECT_TRUE(fixture.selection.full_resync);
    fixture.publish(100U, 100U);
    fixture.compare();
    EXPECT_FALSE(fixture.selection.full_resync);
    EXPECT_TRUE(fixture.selection.columns.empty());
    EXPECT_TRUE(fixture.world.removeAuthoritativeSurface({100, 100, 0}));
    fixture.compare();
    EXPECT_TRUE(fixture.selection.columns.empty());
}

TEST(TerrainLodRuntimeTest, SourceIndexDiagnosticsCountResidentAndSelectionWork)
{
    static constexpr uint64_t EXPECTED_SELECTION_COUNT = 1U;
    static constexpr uint64_t EXPECTED_RESIDENT_SNAPSHOT_COUNT = 1U;
    static constexpr uint64_t EXPECTED_SELECTED_COLUMN_COUNT = 1U;
    static constexpr uint64_t EXPECTED_FULL_RESYNC_COUNT = 1U;

    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.capture(0U, 0U);
    TerrainLodSourceIndexDiagnostics diagnostics;

    fixture.index.select(fixture.world, true, fixture.selection, &diagnostics);

    EXPECT_EQ(diagnostics.selection_count, EXPECTED_SELECTION_COUNT);
    EXPECT_EQ(diagnostics.resident_snapshot_count, EXPECTED_RESIDENT_SNAPSHOT_COUNT);
    EXPECT_EQ(diagnostics.selected_column_count, EXPECTED_SELECTED_COLUMN_COUNT);
    EXPECT_EQ(diagnostics.full_resync_count, EXPECTED_FULL_RESYNC_COUNT);
    ASSERT_EQ(fixture.selection.columns.size(), EXPECTED_SELECTED_COLUMN_COUNT);
}

TEST(TerrainLodRuntimeTest, IncrementalCrossDependenciesMatchFullValidationForRemovalAndReadiness)
{
    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.cross(10U, 10U);
    fixture.capture(0U, 0U);
    fixture.capture(10U, 10U);
    fixture.compare();
    EXPECT_TRUE(fixture.world.removeAuthoritativeSurface({4'095, 0, 0}));
    // Cache availability changes here, but the captured canonical profile remains current.
    fixture.compare();
    ASSERT_EQ(fixture.selection.columns.size(), 1U);
    ASSERT_EQ(fixture.columns.size(), 2U);
    fixture.publish(4'095U, 0U);
    fixture.compare();
    ASSERT_EQ(fixture.selection.columns.size(), 1U);
    ASSERT_EQ(fixture.columns.size(), 1U);
    EXPECT_NE(std::ranges::find(fixture.selection.discovery, TerrainLodColumnKey{0U, 0U}),
        fixture.selection.discovery.end());
    fixture.capture(0U, 0U);
    fixture.compare();
    shared::EditedChunkRevision const requirement{{0, 0, 24}, 1U};
    EXPECT_TRUE(fixture.world.setEditedChunkRequirements({0, 0, 0}, std::span{&requirement, 1U}));
    fixture.compare();
    EXPECT_EQ(fixture.columns.size(), 1U);
    EXPECT_TRUE(fixture.world.setEditedChunkRequirements({0, 0, 0}, {}));
    fixture.compare();
    EXPECT_NE(std::ranges::find(fixture.selection.discovery, TerrainLodColumnKey{0U, 0U}),
        fixture.selection.discovery.end());
}

TEST(TerrainLodRuntimeTest, IncrementalResidentDiffPreservesBorrowedMutationsAtFormerValidationBoundaries)
{
    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    auto* chunk = fixture.world.ensureChunk({0, 0, 24});
    ASSERT_NE(chunk, nullptr);
    fixture.capture(0U, 0U);
    fixture.compare();
    uint64_t const revision = fixture.world.authoritativeSurfaceRevision();
    EXPECT_TRUE(chunk->setBlock({1U, 1U, 1U}, shared::Block::Stone));
    EXPECT_EQ(fixture.world.authoritativeSurfaceRevision(), revision);
    // Dispatch/interest rediscovery explicitly scan residents even with no world revision change.
    fixture.compare();
    EXPECT_TRUE(fixture.columns.empty());
    fixture.capture(0U, 0U);
    fixture.compare();
    EXPECT_TRUE(chunk->setBlock({2U, 1U, 1U}, shared::Block::Stone));
    fixture.publish(200U, 200U);
    fixture.compare(false);
    EXPECT_TRUE(fixture.columns.empty());
}

TEST(TerrainLodRuntimeTest, IncrementalEditsAndSnapshotsMatchFullReference)
{
    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.capture(0U, 0U);
    fixture.compare();
    EXPECT_TRUE(fixture.world.setBlock({1, 1, 384}, shared::Block::Air));
    fixture.compare();
    EXPECT_TRUE(fixture.columns.empty());
    fixture.capture(0U, 0U);
    fixture.compare();
    EXPECT_TRUE(fixture.world.applyEditDelta({2, 1, 384}, shared::Block::Air, 1U, 2U, 2U));
    fixture.compare();
    EXPECT_TRUE(fixture.columns.empty());
    fixture.capture(0U, 0U);
    fixture.compare();
    auto snapshot = fixture.world.editSnapshot({0, 0, 24});
    auto effective = fixture.world.effectiveChunkSnapshot({0, 0, 24});
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(effective);
    snapshot->revision = 3U;
    EXPECT_TRUE(fixture.world.installEditSnapshot(*snapshot, *effective));
    fixture.compare();
    EXPECT_TRUE(fixture.columns.empty());
}

TEST(TerrainLodRuntimeTest, IncrementalChunkNeighborDependenciesMatchBorrowedChangesAndEviction)
{
    IncrementalSources fixture;
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
    heights.fill(385U);
    auto surface = std::make_unique<shared::SurfaceTile>(testsupport::makeSurfaceTile({1, 1}, fixture.identity, heights));
    ASSERT_TRUE(fixture.world.publishSurfaceMetadata(*surface));
    int32_t const surface_z = surface->columns[0].highest_solid_z / 16;
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t x = 0; x < 3; ++x) {
            for (int32_t z = surface_z - 1; z <= surface_z + 1; ++z) {
                shared::Chunk::Blocks blocks{};
                for (uint32_t local_z = 0U; local_z < 16U; ++local_z) {
                    for (uint32_t i = 0U; i < 256U; ++i) {
                        blocks[local_z * 256U + i] = surface->columns[i].materialAt(
                            static_cast<uint16_t>(z * 16 + static_cast<int32_t>(local_z)), surface->block_sea_level);
                    }
                }
                ASSERT_TRUE(fixture.world.publishMaterializedChunk(shared::Chunk{{x, y, z}, blocks}, 1U, 42U));
            }
        }
    }
    fixture.capture(1U, 1U);
    ASSERT_FALSE(fixture.columns.empty());
    EXPECT_TRUE(fixture.columns.front()->input().authority_sources.empty());
    EXPECT_FALSE(fixture.columns.front()->input().sources.empty());
    fixture.compare();
    auto* neighbor = fixture.world.ensureChunk({0, 1, surface_z});
    ASSERT_NE(neighbor, nullptr);
    EXPECT_TRUE(neighbor->setBlock({1U, 1U, 1U}, shared::Block::Snow));
    fixture.compare();
    EXPECT_TRUE(fixture.columns.empty());
    fixture.capture(1U, 1U);
    fixture.compare();
    EXPECT_TRUE(fixture.world.evict({0, 1, surface_z}));
    fixture.compare();
    EXPECT_TRUE(fixture.columns.empty());
}

TEST(TerrainLodRuntimeTest, UnresidentFallbackEditInvalidatesCapturedSourcesBeforeAndAfterMaterialization)
{
    static constexpr uint32_t COLUMN_X = 0U;
    static constexpr uint32_t COLUMN_Y = 0U;
    static constexpr uint32_t COLUMN_COUNT = shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH;
    static constexpr uint32_t VERTICAL_CHUNK_COUNT = shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH;
    static constexpr int32_t EDIT_CHUNK_Z = 10;
    static constexpr shared::ChunkCoordinate EDIT_CHUNK{
        static_cast<int32_t>(COLUMN_COUNT - 1U), 0, EDIT_CHUNK_Z};
    static constexpr shared::WorldCoordinate EDITED_BLOCK{
        static_cast<int64_t>(EDIT_CHUNK.x) * shared::Chunk::SIDE_LENGTH + 1,
        1,
        EDIT_CHUNK_Z * shared::Chunk::SIDE_LENGTH + 5,
    };
    static constexpr shared::WorldCoordinate UNRELATED_BLOCK{1601, 1601, EDIT_CHUNK_Z * 16 + 5};
    static constexpr shared::BlockCoordinate EDITED_LOCAL{1U, 1U, 5U};

    IncrementalSources fixture;
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
    heights.fill(385U);
    shared::SurfaceTile const surface = testsupport::makeSurfaceTile(
        {static_cast<int32_t>(COLUMN_X), static_cast<int32_t>(COLUMN_Y)}, fixture.identity, heights);
    ASSERT_TRUE(fixture.world.publishSurfaceMetadata(surface));
    int32_t const surface_z = surface.columns[0].highest_solid_z / shared::Chunk::SIDE_LENGTH;
    auto const canonical_blocks_for_z = [&surface](int32_t const chunk_z) {
        shared::Chunk::Blocks blocks{};
        for (uint32_t local_z = 0U; local_z < shared::Chunk::SIDE_LENGTH; ++local_z) {
            for (uint32_t local_y = 0U; local_y < shared::Chunk::SIDE_LENGTH; ++local_y) {
                for (uint32_t local_x = 0U; local_x < shared::Chunk::SIDE_LENGTH; ++local_x) {
                    uint32_t const profile_index = local_y * shared::Chunk::SIDE_LENGTH + local_x;
                    uint32_t const block_index = local_z * shared::Chunk::FACE_BLOCK_COUNT + profile_index;
                    blocks[block_index] = surface.columns[profile_index].materialAt(
                        static_cast<uint16_t>(
                            static_cast<uint32_t>(chunk_z) * shared::Chunk::SIDE_LENGTH + local_z),
                        surface.block_sea_level);
                }
            }
        }
        return blocks;
    };
    for (int32_t y_offset = -1; y_offset <= 1; ++y_offset) {
        for (int32_t x_offset = -1; x_offset <= 1; ++x_offset) {
            uint32_t const chunk_x = static_cast<uint32_t>(
                (static_cast<int64_t>(COLUMN_X) + x_offset + COLUMN_COUNT) % COLUMN_COUNT);
            uint32_t const chunk_y = static_cast<uint32_t>(
                (static_cast<int64_t>(COLUMN_Y) + y_offset + COLUMN_COUNT) % COLUMN_COUNT);
            for (int32_t z = surface_z - 1; z <= surface_z + 1; ++z) {
                ASSERT_TRUE(fixture.world.publishMaterializedChunk(
                    shared::Chunk{
                        {static_cast<int32_t>(chunk_x), static_cast<int32_t>(chunk_y), z}, canonical_blocks_for_z(z)},
                    fixture.identity.revision, fixture.identity.seed));
            }
        }
    }

    ASSERT_EQ(surface.columns[shared::Chunk::SIDE_LENGTH + 1U].materialAt(
                  static_cast<uint16_t>(EDITED_BLOCK.z), surface.block_sea_level),
        shared::Block::Stone);
    fixture.capture(COLUMN_X, COLUMN_Y);
    ASSERT_EQ(fixture.columns.size(), 1U);
    std::shared_ptr<TerrainLodColumnInput const> const captured_input{
        fixture.columns.front(), &fixture.columns.front()->input()};
    EXPECT_TRUE(captured_input->authority_sources.empty());
    EXPECT_EQ(captured_input->edit_sources.size(), 5U * VERTICAL_CHUNK_COUNT);
    EXPECT_NE(std::ranges::find(captured_input->edit_sources, TerrainLodEditSource{EDIT_CHUNK, 0U}),
        captured_input->edit_sources.end());
    EXPECT_TRUE(std::ranges::none_of(captured_input->sources, [](TerrainLodChunkSource const& source) {
        return source.coordinate == EDIT_CHUNK;
    }));

    std::array const scene_inputs{captured_input};
    auto const old_scene = buildTerrainLodColumnPatch(fixture.identity, scene_inputs);
    EXPECT_EQ(old_scene->edit_sources.size(), captured_input->edit_sources.size());
    ASSERT_TRUE(terrainLodColumnSourcesCurrent(*captured_input, fixture.world));
    ASSERT_TRUE(terrainLodSourcesCurrent(*old_scene, fixture.world));
    fixture.index.select(fixture.world, false, fixture.selection);
    ASSERT_EQ(fixture.selection.columns.size(), 1U);
    EXPECT_EQ(fixture.selection.columns.front(), fixture.columns.front());

    uint64_t const sparse_revision_before_unrelated = fixture.world.sparseEditRevision();
    uint64_t const surface_revision_before = fixture.world.authoritativeSurfaceRevision();
    ASSERT_TRUE(fixture.world.applyEditDelta(UNRELATED_BLOCK, shared::Block::Snow, 0U, 1U, 1U));
    EXPECT_EQ(fixture.world.sparseEditRevision(), sparse_revision_before_unrelated + 1U);
    EXPECT_EQ(fixture.world.authoritativeSurfaceRevision(), surface_revision_before);
    fixture.index.select(fixture.world, false, fixture.selection);
    EXPECT_FALSE(fixture.selection.full_resync);
    EXPECT_TRUE(fixture.selection.columns.empty());
    EXPECT_TRUE(staleCapturedTerrainLodColumns(fixture.columns, fixture.world, fixture.batch).empty());
    EXPECT_TRUE(terrainLodColumnSourcesCurrent(*captured_input, fixture.world));
    EXPECT_TRUE(terrainLodSourcesCurrent(*old_scene, fixture.world));

    uint64_t const sparse_revision_before_target = fixture.world.sparseEditRevision();
    ASSERT_TRUE(fixture.world.applyEditDelta(EDITED_BLOCK, shared::Block::Air, 0U, 1U, 1U));
    EXPECT_EQ(fixture.world.sparseEditRevision(), sparse_revision_before_target + 1U);
    EXPECT_EQ(fixture.world.authoritativeSurfaceRevision(), surface_revision_before);

    fixture.index.select(fixture.world, false, fixture.selection);
    EXPECT_FALSE(fixture.selection.full_resync);
    ASSERT_EQ(fixture.selection.columns.size(), 1U);
    EXPECT_EQ(fixture.selection.columns.front(), fixture.columns.front());
    EXPECT_EQ(staleCapturedTerrainLodColumns(fixture.selection.columns, fixture.world, fixture.batch).size(), 1U);
    EXPECT_FALSE(terrainLodColumnSourcesCurrent(*captured_input, fixture.world));
    EXPECT_FALSE(terrainLodSourcesCurrent(*old_scene, fixture.world));

    shared::Chunk::Blocks const canonical_blocks = canonical_blocks_for_z(EDIT_CHUNK_Z);
    shared::Chunk const baseline_chunk{EDIT_CHUNK, canonical_blocks};
    shared::ChunkMesher baseline_mesher;
    shared::ChunkMesh const baseline_mesh = baseline_mesher.update(baseline_chunk);
    ASSERT_TRUE(fixture.world.publishMaterializedChunk(
        shared::Chunk{EDIT_CHUNK, canonical_blocks}, fixture.identity.revision, fixture.identity.seed));
    shared::Chunk const* const materialized = fixture.world.residentChunk(EDIT_CHUNK);
    ASSERT_NE(materialized, nullptr);
    ASSERT_EQ(materialized->blockAt(EDITED_LOCAL), shared::Block::Air);
    shared::ChunkMesher edited_mesher;
    shared::ChunkMesh const edited_mesh = edited_mesher.update(*materialized);
    EXPECT_NE(edited_mesh.faces, baseline_mesh.faces);

    fixture.index.select(fixture.world, false, fixture.selection);
    ASSERT_EQ(fixture.selection.columns.size(), 1U);
    EXPECT_EQ(fixture.selection.columns.front(), fixture.columns.front());
    EXPECT_EQ(staleCapturedTerrainLodColumns(fixture.selection.columns, fixture.world, fixture.batch).size(), 1U);
    EXPECT_FALSE(terrainLodColumnSourcesCurrent(*captured_input, fixture.world));
    EXPECT_FALSE(terrainLodSourcesCurrent(*old_scene, fixture.world));
}

TEST(TerrainLodRuntimeTest, PartialSharedFallbackEditCaptureCannotAdvanceSceneFreshness)
{
    static constexpr uint32_t COLUMN_COUNT = shared::WorldExtent::WIDTH / shared::Chunk::SIDE_LENGTH;
    static constexpr int32_t EDIT_CHUNK_Z = 24;
    static constexpr shared::ChunkCoordinate EDIT_CHUNK{1, 0, EDIT_CHUNK_Z};

    IncrementalSources fixture;
    std::array<uint16_t, shared::HEIGHT_TILE_SAMPLE_COUNT> heights;
    heights.fill(385U);
    shared::SurfaceTile const left_surface = testsupport::makeSurfaceTile({0, 0}, fixture.identity, heights);
    shared::SurfaceTile const right_surface = testsupport::makeSurfaceTile({1, 0}, fixture.identity, heights);
    ASSERT_TRUE(fixture.world.publishSurfaceMetadata(left_surface));
    ASSERT_TRUE(fixture.world.publishSurfaceMetadata(right_surface));

    auto const canonical_blocks_for_z = [&left_surface](int32_t const chunk_z) {
        shared::Chunk::Blocks blocks{};
        for (uint32_t local_z = 0U; local_z < shared::Chunk::SIDE_LENGTH; ++local_z) {
            uint16_t const block_z = static_cast<uint16_t>(
                static_cast<uint32_t>(chunk_z) * shared::Chunk::SIDE_LENGTH + local_z);
            for (uint32_t index = 0U; index < shared::Chunk::FACE_BLOCK_COUNT; ++index) {
                blocks[static_cast<size_t>(local_z) * shared::Chunk::FACE_BLOCK_COUNT + index] =
                    left_surface.columns[index].materialAt(block_z, left_surface.block_sea_level);
            }
        }
        return blocks;
    };
    for (int32_t y_offset = -1; y_offset <= 1; ++y_offset) {
        uint32_t const chunk_y = static_cast<uint32_t>((y_offset + static_cast<int32_t>(COLUMN_COUNT)) %
            static_cast<int32_t>(COLUMN_COUNT));
        for (int32_t x_offset = -1; x_offset <= 2; ++x_offset) {
            uint32_t const chunk_x = static_cast<uint32_t>((x_offset + static_cast<int32_t>(COLUMN_COUNT)) %
                static_cast<int32_t>(COLUMN_COUNT));
            for (int32_t chunk_z = 22; chunk_z <= 25; ++chunk_z) {
                ASSERT_TRUE(fixture.world.publishMaterializedChunk(
                    shared::Chunk{{static_cast<int32_t>(chunk_x), static_cast<int32_t>(chunk_y), chunk_z},
                        canonical_blocks_for_z(chunk_z)},
                    fixture.identity.revision,
                    fixture.identity.seed));
            }
        }
    }

    std::vector<std::shared_ptr<TerrainLodColumnInput const>> presented;
    for (uint32_t x : {0U, 1U}) {
        auto const capture = captureTerrainLodColumnForBatch(fixture.world, x, 0U, fixture.batch);
        ASSERT_NE(capture, nullptr);
        presented.emplace_back(capture, &capture->input());
    }
    auto const active_scene = buildTerrainLodColumnPatch(fixture.identity, presented);
    ASSERT_EQ(active_scene->groups.size(), 2U);
    ASSERT_TRUE(terrainLodSourcesCurrent(*active_scene, fixture.world));
    shared::Chunk const* const initial_right_chunk = fixture.world.residentChunk(EDIT_CHUNK);
    ASSERT_NE(initial_right_chunk, nullptr);
    shared::Chunk const original_right_chunk = *initial_right_chunk;
    shared::ChunkMesher baseline_mesher;
    shared::ChunkMesh const baseline_mesh = baseline_mesher.update(original_right_chunk);
    EXPECT_NE(std::ranges::find(presented[0]->edit_sources, TerrainLodEditSource{EDIT_CHUNK, 0U}),
        presented[0]->edit_sources.end());
    EXPECT_NE(std::ranges::find(presented[1]->edit_sources, TerrainLodEditSource{EDIT_CHUNK, 0U}),
        presented[1]->edit_sources.end());

    TerrainLodPresentedEditIndex edit_index;
    edit_index.replace(presented, fixture.world);
    ASSERT_TRUE(fixture.world.applyEditDelta({17, 1, 384}, shared::Block::Air, 0U, 1U, 1U));
    EXPECT_EQ(fixture.world.editChunkRevision(EDIT_CHUNK), 1U);
    shared::Chunk const* const edited_right_chunk = fixture.world.residentChunk(EDIT_CHUNK);
    ASSERT_NE(edited_right_chunk, nullptr);
    EXPECT_EQ(edited_right_chunk->blockAt({1U, 1U, 0U}), shared::Block::Air);
    shared::ChunkMesher edited_mesher;
    shared::ChunkMesh const edited_mesh = edited_mesher.update(*edited_right_chunk);
    EXPECT_NE(edited_mesh.faces, baseline_mesh.faces);
    EXPECT_FALSE(terrainLodSourcesCurrent(*active_scene, fixture.world));

    std::vector<std::shared_ptr<TerrainLodColumnInput const>> selected;
    edit_index.select(fixture.world, selected);
    ASSERT_EQ(selected.size(), 2U);
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> ready;
    EXPECT_FALSE(captureEditedTerrainLodColumns(selected, fixture.world, fixture.batch, ready));
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready.front()->x, 0U);
    EXPECT_TRUE(terrainLodColumnSourcesCurrent(*ready.front(), fixture.world));
    auto const retained_partial_capture = ready.front();

    TerrainLodEditCheckpoint checkpoint;
    uint64_t const first_sparse_revision = fixture.world.sparseEditRevision();
    checkpoint.captured(first_sparse_revision, false);
    EXPECT_FALSE(checkpoint.canPublish());
    std::shared_ptr<TerrainLodScene const> partial_scene;
    if (checkpoint.canPublish()) {
        partial_scene = buildTerrainLodColumnPatch(fixture.identity, ready);
    }
    EXPECT_EQ(partial_scene, nullptr);
    EXPECT_FALSE(terrainLodSourcesCurrent(*active_scene, fixture.world));
    checkpoint.published();
    EXPECT_TRUE(checkpoint.needsCapture(first_sparse_revision));

    ASSERT_TRUE(fixture.world.applyEditDelta({17, 1, 384}, shared::Block::Grass, 1U, 2U, 2U));
    uint64_t const second_sparse_revision = fixture.world.sparseEditRevision();
    EXPECT_EQ(fixture.world.editChunkRevision(EDIT_CHUNK), 2U);
    EXPECT_TRUE(checkpoint.needsCapture(second_sparse_revision));
    EXPECT_FALSE(terrainLodColumnSourcesCurrent(*retained_partial_capture, fixture.world));

    ASSERT_TRUE(fixture.world.publishSurfaceMetadata(right_surface));
    edit_index.select(fixture.world, selected);
    ASSERT_EQ(selected.size(), 2U);
    ASSERT_TRUE(captureEditedTerrainLodColumns(selected, fixture.world, fixture.batch, ready));
    ASSERT_EQ(ready.size(), 2U);
    EXPECT_EQ(ready.front()->x, 0U);
    EXPECT_EQ(ready.back()->x, 1U);
    EXPECT_TRUE(terrainLodColumnSourcesCurrent(*ready[0], fixture.world));
    EXPECT_TRUE(terrainLodColumnSourcesCurrent(*ready[1], fixture.world));

    checkpoint.captured(second_sparse_revision, true);
    ASSERT_TRUE(checkpoint.canPublish());
    auto const complete_patch = buildTerrainLodColumnPatch(fixture.identity, ready);
    ASSERT_EQ(complete_patch->groups.size(), 2U);
    std::vector<StoneIndirectDraws::Range> active_ranges;
    for (auto const& group : active_scene->groups) {
        active_ranges.push_back(group.range);
    }
    auto const replacement = detail::terrainLodReplacementPlan(*active_scene, active_ranges, *complete_patch);
    ASSERT_NE(replacement.scene, nullptr);
    EXPECT_TRUE(terrainLodSourcesCurrent(*replacement.scene, fixture.world));
    for (size_t index = 0U; index < complete_patch->groups.size(); ++index) {
        EXPECT_TRUE(std::ranges::equal(
            terrainLodGroupFaces(replacement.scene->groups[index]),
            terrainLodGroupFaces(complete_patch->groups[index])));
    }
    EXPECT_TRUE(std::ranges::equal(
        terrainLodGroupFaces(active_scene->groups[1]), terrainLodGroupFaces(complete_patch->groups[1])));
    checkpoint.published();
    EXPECT_FALSE(checkpoint.needsCapture(second_sparse_revision));

    edit_index.replace(ready, fixture.world);
    edit_index.select(fixture.world, selected);
    EXPECT_TRUE(selected.empty());
    ASSERT_TRUE(fixture.world.applyEditDelta({18, 1, 384}, shared::Block::Air, 2U, 3U, 3U));
    ASSERT_TRUE(fixture.world.removeSurfaceMetadata({0, 0, 0}));
    uint64_t const third_sparse_revision = fixture.world.sparseEditRevision();
    edit_index.select(fixture.world, selected);
    ASSERT_EQ(selected.size(), 2U);
    EXPECT_FALSE(captureEditedTerrainLodColumns(selected, fixture.world, fixture.batch, ready));
    EXPECT_TRUE(ready.empty());
    checkpoint.captured(third_sparse_revision, false);
    EXPECT_FALSE(checkpoint.canPublish());
    checkpoint.published();
    EXPECT_TRUE(checkpoint.needsCapture(third_sparse_revision));
    EXPECT_FALSE(terrainLodSourcesCurrent(*replacement.scene, fixture.world));
}

TEST(TerrainLodRuntimeTest, IncrementalHistoryLossAndResetResynchronizeIndependentConsumers)
{
    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.capture(0U, 0U);
    fixture.compare();
    TerrainLodSourceIndex second;
    second.upsert(fixture.columns.front());
    TerrainLodSourceIndex::Selection selected;
    second.select(fixture.world, true, selected);
    for (uint32_t i = 0U; i < 4'100U; ++i) {
        shared::EditedChunkRevision const requirement{{100, 100, 24}, 1U + i % 2U};
        EXPECT_TRUE(fixture.world.setEditedChunkRequirements({100, 100, 0}, std::span{&requirement, 1U}));
    }
    fixture.compare();
    EXPECT_TRUE(fixture.selection.full_resync);
    second.select(fixture.world, true, selected);
    EXPECT_TRUE(selected.full_resync);
    fixture.publish(200U, 200U);
    fixture.compare();
    EXPECT_FALSE(fixture.selection.full_resync);
    second.select(fixture.world, true, selected);
    EXPECT_FALSE(selected.full_resync);
    fixture.world.setWorldIdentity(2U, 43U);
    fixture.compare();
    EXPECT_TRUE(fixture.selection.full_resync);
    EXPECT_TRUE(fixture.columns.empty());
}

TEST(TerrainLodRuntimeTest, IncrementalUnrelatedStreamWorkDoesNotScaleWithCaptureCount)
{
    IncrementalSources fixture;
    for (uint32_t y = 0U; y < 18U; ++y) {
        for (uint32_t x = 0U; x < 18U; ++x) {
            fixture.publish(x, y);
        }
    }
    for (uint32_t y = 1U; y < 17U; ++y) {
        for (uint32_t x = 1U; x < 17U; ++x) {
            fixture.capture(x, y);
        }
    }
    fixture.compare();
    ASSERT_EQ(fixture.columns.size(), 256U);
    uint64_t full_duration = 0U;
    uint64_t incremental_duration = 0U;
    for (uint32_t i = 0U; i < 32U; ++i) {
        fixture.publish(40U + i % 8U, 40U + i / 8U);
        auto const before_full = std::chrono::steady_clock::now();
        auto const full = staleCapturedTerrainLodColumns(fixture.columns, fixture.world, fixture.batch);
        auto const available = fixture.world.availableAuthoritativeColumns();
        auto const before_incremental = std::chrono::steady_clock::now();
        fixture.index.select(fixture.world, true, fixture.selection);
        auto const incremental = staleCapturedTerrainLodColumns(fixture.selection.columns, fixture.world, fixture.batch);
        auto const finished = std::chrono::steady_clock::now();
        full_duration += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            before_incremental - before_full).count());
        incremental_duration += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - before_incremental).count());
        EXPECT_EQ(incremental, full);
        EXPECT_TRUE(full.empty());
        EXPECT_FALSE(available.empty());
        EXPECT_TRUE(fixture.selection.columns.empty());
        EXPECT_FALSE(fixture.selection.full_resync);
    }
    RecordProperty("captured_columns", 256U);
    RecordProperty("streamed_events", 32U);
    RecordProperty("full_reference_column_checks", 8'192U);
    RecordProperty("incremental_column_checks", 0U);
    RecordProperty("full_reference_duration_ns", full_duration);
    RecordProperty("incremental_duration_ns", incremental_duration);
}

TEST(TerrainLodRuntimeTest, CrossingCenterRetainsPresentedCurrentColumns)
{
    static constexpr std::array<TerrainLodColumnKey, 3> COLUMNS{{{3U, 2U}, {4U, 2U}, {5U, 2U}}};
    TerrainLodRuntimePlan plan;
    EXPECT_TRUE(plan.updateInterest(63U, 32U));
    for (auto const column : COLUMNS) {
        plan.remember(column);
    }
    plan.presented(COLUMNS);
    EXPECT_TRUE(plan.updateInterest(64U, 32U));
    EXPECT_EQ(plan.ticket().columns.size(), COLUMNS.size());
    EXPECT_FALSE(plan.canPublish(plan.ticket(), std::span{COLUMNS}.subspan(1U, 1U)));
}

TEST(TerrainLodRuntimeTest, SameColumnInterestUpdateKeepsPlanAndPresentedMembership)
{
    static constexpr std::array<TerrainLodColumnKey, 2> COLUMNS{{{3U, 2U}, {4U, 2U}}};
    TerrainLodRuntimePlan plan;
    ASSERT_TRUE(plan.updateInterest(63U, 32U));
    for (auto const column : COLUMNS) {
        plan.remember(column);
    }
    plan.presented(COLUMNS);

    EXPECT_FALSE(plan.updateInterest(62U, 47U));

    auto const ticket = plan.ticket();
    EXPECT_EQ(ticket.center_x, 62U);
    EXPECT_EQ(ticket.center_y, 47U);
    ASSERT_EQ(ticket.columns.size(), COLUMNS.size());
    EXPECT_NE(std::ranges::find(ticket.columns, COLUMNS[0]), ticket.columns.end());
    EXPECT_NE(std::ranges::find(ticket.columns, COLUMNS[1]), ticket.columns.end());
    auto const issue = plan.publicationIssue(ticket, std::span{COLUMNS}.first(1U));
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::MissingPresented);
    EXPECT_EQ(issue.column, COLUMNS[1]);
}

TEST(TerrainLodRuntimeTest, DelayedSceneCannotOmitCurrentReadyCenter)
{
    static constexpr TerrainLodColumnKey OLD{3U, 2U};
    static constexpr TerrainLodColumnKey CURRENT{8U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    plan.remember(OLD);
    auto const old = plan.ticket();
    plan.updateInterest(128U, 32U);
    plan.remember(CURRENT);
    EXPECT_FALSE(plan.canPublish(old, std::array{OLD}));
}

TEST(TerrainLodRuntimeTest, DiscoveryAdvancesAcrossRepeatedCenterChanges)
{
    static constexpr std::array<TerrainLodColumnKey, 3> AVAILABLE{{{10U, 2U}, {11U, 2U}, {12U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[0]);
    plan.remember(AVAILABLE[0]);
    plan.updateInterest(128U, 32U);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[1]);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[2]);
}

TEST(TerrainLodRuntimeTest, ForgettingInvalidSourceAllowsItsReplacementToBeOmitted)
{
    static constexpr std::array<TerrainLodColumnKey, 2> COLUMNS{{{3U, 2U}, {4U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    for (auto const key : COLUMNS) {
        plan.remember(key);
    }
    plan.presented(COLUMNS);
    plan.forget(COLUMNS[0]);
    auto const ticket = plan.ticket();
    EXPECT_TRUE(plan.canPublish(ticket, std::span{COLUMNS}.subspan(1U)));
}

TEST(TerrainLodRuntimeTest, OlderInterestMayPublishWhenItRetainsAllCurrentValidDetail)
{
    static constexpr std::array<TerrainLodColumnKey, 2> COLUMNS{{{3U, 2U}, {4U, 2U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    for (auto const key : COLUMNS) {
        plan.remember(key);
    }
    plan.presented(COLUMNS);
    auto const ticket = plan.ticket();
    plan.updateInterest(64U, 32U);
    EXPECT_TRUE(plan.canPublish(ticket, COLUMNS));
}

TEST(TerrainLodRuntimeTest, DistantColumnsRetireOnlyAfterLeavingPreservationInterest)
{
    static constexpr TerrainLodColumnKey OLD{0U, 0U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    plan.remember(OLD);
    plan.presented(std::array{OLD});
    EXPECT_TRUE(plan.updateInterest(640U, 0U));
    EXPECT_EQ(plan.ticket().columns.size(), 1U);
    EXPECT_TRUE(plan.updateInterest(672U, 0U));
    EXPECT_TRUE(plan.ticket().columns.empty());
    EXPECT_TRUE(plan.canPublish(plan.ticket(), {}));
}

TEST(TerrainLodRuntimeTest, ReadyCurrentCenterPreemptsPreviouslyQueuedDistantColumns)
{
    static constexpr std::array<TerrainLodColumnKey, 3> AVAILABLE{{{10U, 0U}, {11U, 0U}, {12U, 0U}}};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    plan.discover(AVAILABLE);
    plan.updateInterest(192U, 0U);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[2]);
}

TEST(TerrainLodRuntimeTest, SourceInvalidationCanRediscoverAnAlreadyPresentedColumn)
{
    static constexpr std::array<TerrainLodColumnKey, 1> AVAILABLE{{{0U, 0U}}};
    TerrainLodRuntimePlan plan;
    plan.remember(AVAILABLE[0]);
    plan.discover(AVAILABLE);
    EXPECT_FALSE(plan.nextDiscovery());
    plan.forget(AVAILABLE[0]);
    plan.discover(AVAILABLE);
    EXPECT_EQ(plan.nextDiscovery(), AVAILABLE[0]);
}

TEST(TerrainLodRuntimeTest, RelevantColumnsIncludeFortyChunksAndPeriodicWrap)
{
    TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    EXPECT_TRUE(plan.relevant({40U, 0U}));
    EXPECT_TRUE(plan.relevant({4'056U, 0U}));
    EXPECT_FALSE(plan.relevant({42U, 0U}));
}

TEST(TerrainLodRuntimeTest, SourceResetRejectsPriorWork)
{
    TerrainLodRuntimePlan plan;
    auto const old = plan.ticket();
    plan.reset();
    EXPECT_FALSE(plan.canPublish(old, {}));
}

TEST(TerrainLodRuntimeTest, PublicationReportsMissingCurrentCenter)
{
    static constexpr TerrainLodColumnKey OLD{3U, 2U};
    static constexpr TerrainLodColumnKey CURRENT{8U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(63U, 32U);
    plan.remember(OLD);
    auto const ticket = plan.ticket();
    plan.updateInterest(128U, 32U);
    plan.remember(CURRENT);
    auto const issue = plan.publicationIssue(ticket, std::array{OLD});
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::MissingCenter);
    EXPECT_EQ(issue.column, CURRENT);
}

TEST(TerrainLodRuntimeTest, PublicationReportsRetainedPresentedDetail)
{
    static constexpr TerrainLodColumnKey CENTER{4U, 2U};
    static constexpr TerrainLodColumnKey RETAINED{5U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    plan.remember(CENTER);
    plan.remember(RETAINED);
    plan.presented(std::array{CENTER, RETAINED});
    auto const issue = plan.publicationIssue(plan.ticket(), std::array{CENTER});
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::MissingPresented);
    EXPECT_EQ(issue.column, RETAINED);
}

TEST(TerrainLodRuntimeTest, PublicationReportsUncapturedColumn)
{
    static constexpr TerrainLodColumnKey CAPTURED{4U, 2U};
    static constexpr TerrainLodColumnKey UNCAPTURED{5U, 2U};
    TerrainLodRuntimePlan plan;
    plan.updateInterest(64U, 32U);
    plan.remember(CAPTURED);
    auto const issue = plan.publicationIssue(plan.ticket(), std::array{CAPTURED, UNCAPTURED});
    EXPECT_EQ(issue.failure, TerrainLodPublicationFailure::UncapturedColumn);
    EXPECT_EQ(issue.column, UNCAPTURED);
}

} // namespace
TEST(TerrainLodRuntimeTest, PresentedMembershipMatchesFlatTraversalWithoutChangingOrder)
{
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> pending;
    std::vector<TerrainLodColumnKey> rendered;
    for (uint32_t x = 0U; x < 83U; ++x) {
        for (uint32_t y = 0U; y < 83U; ++y) {
            auto column = std::make_shared<TerrainLodColumnInput>();
            column->x = (x + 4'055U) % 4'096U;
            column->y = (y + 4'055U) % 4'096U;
            pending.push_back(column);
            rendered.push_back({column->x, column->y});
        }
    }
    std::minstd_rand random{57'500U};
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> expected;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> actual;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> retired;
    for (uint32_t trial = 0U; trial < 8U; ++trial) {
        if (trial != 0U) {
            std::shuffle(pending.begin(), pending.end(), random);
            std::shuffle(rendered.begin(), rendered.end(), random);
            rendered.resize(rendered.size() / 2U);
        }
        rendered.push_back({UINT32_MAX, UINT32_MAX});
        rendered.push_back(rendered.front());
        pending.push_back(pending.front());
        auto const original_rendered = rendered;
        expected.clear();
        for (auto const& column : pending) {
            if (std::ranges::find(rendered, TerrainLodColumnKey{column->x, column->y}) != rendered.end()) {
                expected.push_back(column);
            }
        }
        selectPresentedTerrainLodColumns(pending, rendered, actual, retired);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(rendered, original_rendered);
        retired.clear();
    }
    selectPresentedTerrainLodColumns(pending, {}, actual, retired);
    EXPECT_TRUE(actual.empty());
    EXPECT_FALSE(retired.empty());
    retired.clear();
    selectPresentedTerrainLodColumns({}, rendered, actual, retired);
    EXPECT_TRUE(actual.empty());
}

TEST(TerrainLodRuntimeTest, PresentedSelectionTransfersOldColumnOwnershipForDeferredRetirement)
{
    auto old_column = std::make_shared<TerrainLodColumnInput>();
    old_column->x = 2U;
    old_column->y = 3U;
    std::weak_ptr<TerrainLodColumnInput const> old_owner = old_column;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> pending{old_column};
    std::vector<TerrainLodColumnKey> rendered{{2U, 3U}};
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> presented;
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> retired;

    selectPresentedTerrainLodColumns(pending, rendered, presented, retired);
    auto new_column = std::make_shared<TerrainLodColumnInput>();
    new_column->x = 4U;
    new_column->y = 5U;
    pending = {new_column};
    rendered = {{4U, 5U}};
    old_column.reset();
    selectPresentedTerrainLodColumns(pending, rendered, presented, retired);

    EXPECT_EQ(presented.size(), 1U);
    EXPECT_EQ(presented.front()->x, 4U);
    ASSERT_EQ(retired.size(), 1U);
    EXPECT_EQ(retired.front()->x, 2U);
    EXPECT_FALSE(old_owner.expired());
    retired.clear();
    EXPECT_TRUE(old_owner.expired());
}


} // namespace client

TEST(TerrainLodRuntimeTest, MovingIntoKnownBacklogPromotesCurrentColumnWithoutNewSurfaceEvents)
{
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    std::array const available{client::TerrainLodColumnKey{10U, 0U}, client::TerrainLodColumnKey{11U, 0U},
        client::TerrainLodColumnKey{12U, 0U}};
    plan.discover(available);
    plan.updateInterest(192U, 0U);
    EXPECT_EQ(plan.nextDiscovery(), available[2]);
}

TEST(TerrainLodRuntimeTest, TurningAcrossPeriodicEdgePromotesKnownCurrentColumnOnce)
{
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    std::array const available{client::TerrainLodColumnKey{1U, 0U},
        client::TerrainLodColumnKey{4'095U, 0U}, client::TerrainLodColumnKey{0U, 0U}};
    plan.discover(available);
    EXPECT_TRUE(plan.updateInterest(65'535U, 0U));
    EXPECT_EQ(plan.nextDiscovery(), available[1]);
    plan.remember(available[1]);
    EXPECT_TRUE(plan.updateInterest(0U, 0U));
    EXPECT_EQ(plan.nextDiscovery(), available[2]);
    EXPECT_EQ(plan.nextDiscovery(), available[0]);
    EXPECT_FALSE(plan.nextDiscovery());
}

TEST(TerrainLodRuntimeTest, UnavailableEditedColumnDefersReadyEditsUntilTheSetCanPublish)
{
    client::IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.cross(10U, 10U);
    fixture.capture(0U, 0U);
    fixture.capture(10U, 10U);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> presented;
    for (auto const& captured : fixture.columns) {
        presented.emplace_back(captured, &captured->input());
    }
    auto const before = client::buildTerrainLodColumnPatch(
        fixture.identity, std::span{presented}.subspan(1U));
    ASSERT_EQ(fixture.world.editChunkRevision({0, 0, 24}), 0U);
    ASSERT_EQ(fixture.world.editChunkRevision({10, 10, 24}), 0U);
    ASSERT_TRUE(fixture.world.applyEditDelta({2, 1, 384}, shared::Block::Air, 0U, 1U, 1U));
    ASSERT_TRUE(fixture.world.applyEditDelta({162, 161, 385}, shared::Block::Stone, 0U, 1U, 1U));
    ASSERT_TRUE(fixture.world.removeAuthoritativeSurface({4'095, 0, 0}));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> ready;

    EXPECT_FALSE(client::captureEditedTerrainLodColumns(presented, fixture.world, fixture.batch, ready));
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready.front()->x, 10U);
    EXPECT_EQ(ready.front()->y, 10U);
    EXPECT_TRUE(client::terrainLodColumnSourcesCurrent(*ready.front(), fixture.world));
    uint64_t const sparse_revision = fixture.world.sparseEditRevision();
    client::TerrainLodEditCheckpoint checkpoint;
    checkpoint.captured(sparse_revision, false);
    EXPECT_FALSE(checkpoint.canPublish());
    std::shared_ptr<client::TerrainLodScene const> published_partial;
    if (checkpoint.canPublish()) {
        published_partial = client::buildTerrainLodColumnPatch(fixture.identity, ready);
    }
    EXPECT_EQ(published_partial, nullptr);
    checkpoint.published();
    EXPECT_TRUE(checkpoint.needsCapture(sparse_revision));

    fixture.publish(4'095U, 0U);
    EXPECT_TRUE(client::captureEditedTerrainLodColumns(presented, fixture.world, fixture.batch, ready));
    ASSERT_EQ(ready.size(), 2U);
    EXPECT_EQ(ready.front()->x, 0U);
    EXPECT_EQ(ready.back()->x, 10U);
    checkpoint.captured(sparse_revision, true);
    ASSERT_TRUE(checkpoint.canPublish());
    auto const patch = client::buildTerrainLodColumnPatch(fixture.identity, ready);
    ASSERT_EQ(patch->groups.size(), 2U);
    ASSERT_EQ(before->groups.size(), 1U);
    EXPECT_FALSE(std::ranges::equal(
        client::terrainLodGroupFaces(patch->groups.back()),
        client::terrainLodGroupFaces(before->groups.front())));
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*patch, fixture.world));
    checkpoint.published();
    EXPECT_FALSE(checkpoint.needsCapture(sparse_revision));
}

TEST(TerrainLodRuntimeTest, ProgressivePublicationPreventsOlderFullSceneFromRemovingNewExactColumn)
{
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(16U, 16U);
    client::TerrainLodColumnKey const first{1U, 1U};
    client::TerrainLodColumnKey const arrived{2U, 1U};
    plan.remember(first);
    std::array const original{first};
    plan.presented(original);
    auto const full_scene_in_flight = plan.ticket();
    plan.remember(arrived);
    auto const frontier = plan.ticket();
    std::array const promoted{first, arrived};
    ASSERT_TRUE(plan.canPublish(frontier, promoted));
    plan.presented(promoted);
    auto const issue = plan.publicationIssue(full_scene_in_flight, original);
    EXPECT_EQ(issue.failure, client::TerrainLodPublicationFailure::MissingPresented);
    EXPECT_EQ(issue.column, arrived);
}

TEST(TerrainLodRuntimeTest, CompleteEditCaptureAcknowledgesOnlyAfterPublication)
{
    client::TerrainLodEditCheckpoint checkpoint;
    EXPECT_FALSE(checkpoint.canPublish());
    checkpoint.captured(7U, true);
    EXPECT_TRUE(checkpoint.canPublish());
    EXPECT_TRUE(checkpoint.needsCapture(7U));
    checkpoint.published();
    EXPECT_FALSE(checkpoint.canPublish());
    EXPECT_FALSE(checkpoint.needsCapture(7U));
}

TEST(TerrainLodRuntimeTest, PublishedEditCaptureDoesNotAcknowledgeInterveningRevision)
{
    client::TerrainLodEditCheckpoint checkpoint;
    checkpoint.captured(7U, true);
    EXPECT_TRUE(checkpoint.needsCapture(8U));
    checkpoint.published();
    EXPECT_FALSE(checkpoint.canPublish());
    EXPECT_FALSE(checkpoint.needsCapture(7U));
    EXPECT_TRUE(checkpoint.needsCapture(8U));
}

TEST(TerrainLodRuntimeTest, PartialEditPublicationKeepsUnavailableCaptureRetryable)
{
    client::TerrainLodEditCheckpoint checkpoint;
    checkpoint.captured(7U, true);
    checkpoint.published();
    checkpoint.captured(8U, false);
    EXPECT_FALSE(checkpoint.canPublish());
    checkpoint.published();
    EXPECT_FALSE(checkpoint.canPublish());
    EXPECT_FALSE(checkpoint.needsCapture(7U));
    EXPECT_TRUE(checkpoint.needsCapture(8U));
    checkpoint.captured(8U, true);
    EXPECT_TRUE(checkpoint.canPublish());
    checkpoint.published();
    EXPECT_FALSE(checkpoint.needsCapture(8U));
}

TEST(TerrainLodRuntimeTest, RejectedOrFailedEditPreparationCannotAcknowledgeCapture)
{
    client::TerrainLodEditCheckpoint checkpoint;
    checkpoint.captured(7U, true);
    checkpoint.published();
    checkpoint.captured(8U, true);
    checkpoint.discard();
    checkpoint.published();
    EXPECT_FALSE(checkpoint.needsCapture(7U));
    EXPECT_TRUE(checkpoint.needsCapture(8U));
    checkpoint.captured(8U, true);
    checkpoint.published();
    EXPECT_FALSE(checkpoint.needsCapture(8U));
}

TEST(TerrainLodRuntimeTest, ResetInvalidatesPublishedAndPendingEditCheckpoints)
{
    client::TerrainLodEditCheckpoint checkpoint;
    checkpoint.captured(7U, true);
    checkpoint.published();
    checkpoint.captured(8U, true);
    checkpoint.reset();
    checkpoint.published();
    EXPECT_TRUE(checkpoint.needsCapture(7U));
    EXPECT_TRUE(checkpoint.needsCapture(8U));
    EXPECT_FALSE(checkpoint.needsCapture(0U));
}

TEST(TerrainLodRuntimeTest, EditCaptureDiagnosticsSeparateDetectionFromCaptureAttempts)
{
    client::IncrementalSources fixture;
    fixture.cross(1U, 1U);
    fixture.capture(1U, 1U);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> presented;
    for (auto const& captured : fixture.columns) {
        presented.emplace_back(captured, &captured->input());
    }
    client::TerrainLodUpdateDiagnostics diagnostics;
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> ready;
    ASSERT_TRUE(client::captureEditedTerrainLodColumns(
        presented, fixture.world, fixture.batch, ready, &diagnostics));
    EXPECT_EQ(diagnostics.edit_detection_pass_count, 1U);
    EXPECT_EQ(diagnostics.edit_detection_column_count, 1U);
    EXPECT_EQ(diagnostics.edit_capture_pass_count, 0U);
    EXPECT_TRUE(ready.empty());
    ASSERT_TRUE(fixture.world.applyEditDelta({17, 17, 384}, shared::Block::Air, 0U, 1U, 1U));
    ASSERT_TRUE(client::captureEditedTerrainLodColumns(
        presented, fixture.world, fixture.batch, ready, &diagnostics));
    EXPECT_EQ(diagnostics.edit_detection_pass_count, 2U);
    EXPECT_EQ(diagnostics.edit_detection_column_count, 2U);
    EXPECT_EQ(diagnostics.edit_capture_pass_count, 1U);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_TRUE(client::terrainLodColumnSourcesCurrent(*ready.front(), fixture.world));
}

namespace {
std::vector<client::TerrainLodColumnCoordinate> presentedEditCoordinates(
    std::span<std::shared_ptr<client::TerrainLodColumnInput const> const> const columns,
    shared::SparseWorld const& world)
{
    std::vector<client::TerrainLodColumnCoordinate> result;
    for (auto const& column : columns) {
        bool const edited = std::ranges::any_of(column->edit_sources, [&](auto const& stamp) {
            return world.editChunkRevision(stamp.coordinate) != stamp.revision;
        }) || std::ranges::any_of(column->sources, [&](auto const& stamp) {
            return world.editChunkRevision(stamp.coordinate) != stamp.edit_revision;
        });
        if (edited) {
            result.push_back({column->x, column->y});
        }
    }
    std::ranges::sort(result, [](auto const left, auto const right) {
        return left.x != right.x ? left.x < right.x : left.y < right.y;
    });
    return result;
}

std::vector<client::TerrainLodColumnCoordinate> selectedEditCoordinates(
    std::span<std::shared_ptr<client::TerrainLodColumnInput const> const> const columns)
{
    std::vector<client::TerrainLodColumnCoordinate> result;
    for (auto const& column : columns) {
        result.push_back({column->x, column->y});
    }
    return result;
}

std::shared_ptr<client::TerrainLodColumnInput const> presentedEditInput(uint32_t const x, uint64_t const revision = 0U)
{
    auto column = std::make_shared<client::TerrainLodColumnInput>();
    column->x = x;
    column->edit_sources.push_back({{static_cast<int32_t>(x), 0, 24}, revision});
    return column;
}
} // namespace

TEST(TerrainLodRuntimeTest, PresentedEditIndexVisitsOnlyAffectedSubsetAndIgnoresSurfaceOnlyEvents)
{
    client::IncrementalSources fixture;
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> presented;
    for (uint32_t x = 0U; x < 200U; ++x) {
        presented.push_back(presentedEditInput(x));
    }
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, fixture.world);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    client::TerrainLodUpdateDiagnostics diagnostics;
    ASSERT_TRUE(fixture.world.applyEditDelta({320, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    index.select(fixture.world, selected, &diagnostics);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(diagnostics.edit_detection_column_count, 1U);
    EXPECT_LT(diagnostics.edit_detection_column_count, presented.size());
    index.upsert(presentedEditInput(20U, 1U));
    fixture.publish(999U, 999U);
    diagnostics = {};
    index.select(fixture.world, selected, &diagnostics);
    EXPECT_TRUE(selected.empty());
    EXPECT_EQ(diagnostics.edit_detection_column_count, 0U);
    EXPECT_EQ(diagnostics.edit_detection_pass_count, 0U);
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexMatchesExhaustivePeriodicNeighborAndMultipleEdits)
{
    client::IncrementalSources fixture;
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> presented;
    std::array const coordinates{client::TerrainLodColumnKey{0U, 0U},
        client::TerrainLodColumnKey{4095U, 0U}, client::TerrainLodColumnKey{10U, 10U}};
    for (auto const coordinate : coordinates) {
        fixture.cross(coordinate.x, coordinate.y);
    }
    for (auto const coordinate : coordinates) {
        fixture.capture(coordinate.x, coordinate.y);
        auto const& captured = fixture.columns.back();
        presented.emplace_back(captured, &captured->input());
    }
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, fixture.world);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    ASSERT_TRUE(fixture.world.applyEditDelta({0, 1, 384}, shared::Block::Air, 0U, 1U, 1U));
    index.select(fixture.world, selected);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    ASSERT_EQ(selected.size(), 2U);
    // An unrelated accepted edit arriving while the first patch is in flight
    // joins the retained dirty set instead of acknowledging its old captures.
    ASSERT_TRUE(fixture.world.applyEditDelta({161, 161, 384}, shared::Block::Air, 0U, 1U, 1U));
    index.select(fixture.world, selected);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    EXPECT_EQ(selected.size(), 3U);
    fixture.index.remove({0U, 0U});
    fixture.columns.clear();
    index.select(fixture.world, selected);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    EXPECT_EQ(selected.size(), 3U);
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexRetainsUnavailableInputUntilPublicationWithoutNewEdit)
{
    client::IncrementalSources fixture;
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> presented;
    for (auto const coordinate : {client::TerrainLodColumnKey{0U, 0U}, client::TerrainLodColumnKey{10U, 10U}}) {
        fixture.cross(coordinate.x, coordinate.y);
        fixture.capture(coordinate.x, coordinate.y);
        auto const& captured = fixture.columns.back();
        presented.emplace_back(captured, &captured->input());
    }
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, fixture.world);
    ASSERT_TRUE(fixture.world.applyEditDelta({2, 1, 384}, shared::Block::Air, 0U, 1U, 1U));
    ASSERT_TRUE(fixture.world.applyEditDelta({162, 161, 385}, shared::Block::Stone, 0U, 1U, 1U));
    ASSERT_TRUE(fixture.world.removeAuthoritativeSurface({4095, 0, 0}));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> ready;
    index.select(fixture.world, selected);
    EXPECT_FALSE(client::captureEditedTerrainLodColumns(selected, fixture.world, fixture.batch, ready));
    ASSERT_EQ(ready.size(), 1U);
    ASSERT_EQ(ready.front()->x, 10U);
    index.upsert(ready.front());
    uint64_t const revision = fixture.world.sparseEditRevision();
    index.select(fixture.world, selected);
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(selected.front()->x, 0U);
    fixture.publish(4095U, 0U);
    ASSERT_EQ(fixture.world.sparseEditRevision(), revision);
    index.select(fixture.world, selected);
    ASSERT_EQ(selected.size(), 1U);
    ASSERT_TRUE(client::captureEditedTerrainLodColumns(selected, fixture.world, fixture.batch, ready));
    ASSERT_EQ(ready.size(), 1U);
    index.upsert(ready.front());
    index.select(fixture.world, selected);
    EXPECT_TRUE(selected.empty());
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexReplacesMembershipAndChecksNewPublicationDependencies)
{
    client::IncrementalSources fixture;
    std::array const initial{presentedEditInput(20U)};
    client::TerrainLodPresentedEditIndex index;
    index.replace(initial, fixture.world);
    ASSERT_TRUE(fixture.world.applyEditDelta({320, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    index.select(fixture.world, selected);
    ASSERT_EQ(selected.size(), 1U);
    std::array const next{presentedEditInput(30U)};
    index.replace(next, fixture.world);
    index.select(fixture.world, selected);
    EXPECT_TRUE(selected.empty());
    ASSERT_TRUE(fixture.world.applyEditDelta({480, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    index.select(fixture.world, selected);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(next, fixture.world));
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(selected.front()->x, 30U);
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexJournalOverflowChecksAllPresentedInputsWithoutLosingEdit)
{
    client::IncrementalSources fixture;
    std::array const presented{presentedEditInput(20U), presentedEditInput(30U)};
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, fixture.world);
    for (uint64_t revision = 0U; revision < 4097U; ++revision) {
        ASSERT_TRUE(fixture.world.applyEditDelta({320, 0, 384},
            revision % 2U == 0U ? shared::Block::Snow : shared::Block::Air,
            revision, revision + 1U, revision + 1U));
    }
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    client::TerrainLodUpdateDiagnostics diagnostics;
    index.select(fixture.world, selected, &diagnostics);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    EXPECT_EQ(diagnostics.edit_detection_column_count, presented.size());
    ASSERT_EQ(selected.size(), 1U);
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexHandlesBackwardJournalAndExplicitEpochReset)
{
    client::IncrementalSources old_world;
    old_world.publish(100U, 100U);
    old_world.publish(101U, 100U);
    std::array const presented{presentedEditInput(20U), presentedEditInput(30U)};
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, old_world.world);
    client::IncrementalSources new_world;
    ASSERT_TRUE(new_world.world.applyEditDelta({320, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    client::TerrainLodUpdateDiagnostics diagnostics;
    index.select(new_world.world, selected, &diagnostics);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, new_world.world));
    EXPECT_EQ(diagnostics.edit_detection_column_count, presented.size());
    index.reset();
    index.select(new_world.world, selected);
    EXPECT_TRUE(selected.empty());
    std::array const fresh{presentedEditInput(30U)};
    index.replace(fresh, new_world.world);
    ASSERT_TRUE(new_world.world.applyEditDelta({480, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    index.select(new_world.world, selected);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(fresh, new_world.world));
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexResynchronizesAfterJournalReset)
{
    client::IncrementalSources fixture;
    std::array const presented{presentedEditInput(20U), presentedEditInput(30U)};
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, fixture.world);
    shared::AuthoritativeColumnChanges before;
    fixture.world.readAuthoritativeColumnChanges(0U, before);
    generation::GenerationConfiguration configuration;
    configuration.seed = fixture.identity.seed;
    configuration.resolution = 512U;
    auto identity = fixture.identity;
    identity.configuration_identity = generation::configurationIdentity(configuration);
    ASSERT_NE(identity, fixture.identity);
    ASSERT_TRUE(shared::isValidTerrainIdentity(identity));
    ASSERT_TRUE(fixture.world.setTerrainIdentity(identity));
    ASSERT_TRUE(fixture.world.applyEditDelta({320, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    shared::AuthoritativeColumnChanges after;
    fixture.world.readAuthoritativeColumnChanges(before.cursor, after);
    ASSERT_TRUE(after.full_resync);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    client::TerrainLodUpdateDiagnostics diagnostics;
    index.select(fixture.world, selected, &diagnostics);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    EXPECT_EQ(diagnostics.edit_detection_column_count, presented.size());
    ASSERT_EQ(selected.size(), 1U);
}

TEST(TerrainLodRuntimeTest, PresentedEditIndexIncludesDenseChunkSourceDependencies)
{
    client::IncrementalSources fixture;
    auto column = std::make_shared<client::TerrainLodColumnInput>();
    column->x = 20U;
    column->sources.push_back({.coordinate = {30, 0, 24}, .identity = {}, .edit_revision = 0U});
    std::array<std::shared_ptr<client::TerrainLodColumnInput const>, 1U> const presented{column};
    client::TerrainLodPresentedEditIndex index;
    index.replace(presented, fixture.world);
    ASSERT_TRUE(fixture.world.applyEditDelta({480, 0, 384}, shared::Block::Snow, 0U, 1U, 1U));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> selected;
    index.select(fixture.world, selected);
    EXPECT_EQ(selectedEditCoordinates(selected), presentedEditCoordinates(presented, fixture.world));
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(selected.front()->x, 20U);
}

TEST(TerrainLodRuntimeTest, NearestFrontierSelectionAdmitsCurrentColumnBeforeUnrelatedDestinations)
{
    client::IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.cross(100U, 100U);
    fixture.cross(101U, 100U);
    ASSERT_TRUE(fixture.world.applyEditDelta({1601, 1601, 384}, shared::Block::Air, 0U, 1U, 1U));
    fixture.capture(0U, 0U);
    fixture.capture(101U, 100U);
    fixture.capture(100U, 100U);
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 0U);
    plan.remember({0U, 0U});
    auto const unrelated_full_job = plan.ticket();
    plan.updateInterest(1600U, 1600U);
    plan.remember({100U, 100U});
    plan.remember({101U, 100U});
    auto const& old = fixture.columns.front();
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> columns{
        std::shared_ptr<client::TerrainLodColumnInput const>{old, &old->input()}};
    ASSERT_TRUE(client::selectTerrainLodFrontierColumns(plan, fixture.columns, columns));
    ASSERT_EQ(columns.size(), 1U);
    EXPECT_EQ(columns.back()->x, 100U);
    EXPECT_EQ(columns.back()->y, 100U);
    EXPECT_TRUE(client::terrainLodColumnSourcesCurrent(*columns.back(), fixture.world));
    std::array const rendered{client::TerrainLodColumnKey{100U, 100U}};
    EXPECT_TRUE(plan.canPublish(plan.ticket(), rendered));
    EXPECT_FALSE(plan.canPublish(unrelated_full_job, rendered));
    ASSERT_TRUE(client::selectTerrainLodFrontierColumns(plan, fixture.columns, columns));
    ASSERT_EQ(columns.size(), 2U);
    EXPECT_EQ(columns.back()->x, 101U);
}

TEST(TerrainLodRuntimeTest, VisualDiskExcludesAuthorityCornersButIncludesCircleEdgesAndWrappedNeighbors)
{
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    EXPECT_TRUE(plan.relevant({140U, 140U}));
    EXPECT_FALSE(plan.visuallyRelevant({140U, 140U}));
    EXPECT_TRUE(plan.visuallyRelevant({140U, 100U}));
    EXPECT_TRUE(plan.visuallyRelevant({124U, 132U}));
    EXPECT_FALSE(plan.visuallyRelevant({125U, 132U}));
    plan.updateInterest(0U, 0U);
    EXPECT_TRUE(plan.visuallyRelevant({4095U, 0U}));
    EXPECT_TRUE(plan.visuallyRelevant({4056U, 0U}));
    EXPECT_FALSE(plan.visuallyRelevant({4056U, 4056U}));
}

TEST(TerrainLodRuntimeTest, VisualDiscoveryUsesEuclideanNearFirstAcrossOldQueueOrder)
{
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    std::array const available{client::TerrainLodColumnKey{103U, 103U},
        client::TerrainLodColumnKey{104U, 100U}, client::TerrainLodColumnKey{140U, 140U}};
    plan.discover(available);
    EXPECT_EQ(plan.nextDiscovery(), available[1]);
    EXPECT_EQ(plan.nextDiscovery(), available[0]);
    EXPECT_FALSE(plan.nextDiscovery());
}

TEST(TerrainLodRuntimeTest, VisualFrontierUsesEuclideanDistanceAndRetainsValidDetailAcrossTurn)
{
    client::IncrementalSources fixture;
    fixture.cross(103U, 103U);
    fixture.cross(104U, 100U);
    fixture.capture(103U, 103U);
    fixture.capture(104U, 100U);
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> columns;
    ASSERT_TRUE(client::selectTerrainLodFrontierColumns(plan, fixture.columns, columns));
    ASSERT_EQ(columns.size(), 1U);
    EXPECT_EQ(columns.front()->x, 104U);
    EXPECT_EQ(columns.front()->y, 100U);
    auto const retained = columns.front();
    plan.updateInterest(1616U, 1600U);
    ASSERT_TRUE(client::selectTerrainLodFrontierColumns(plan, fixture.columns, columns));
    EXPECT_EQ(columns.front(), retained);
    EXPECT_EQ(columns.size(), 2U);
}

TEST(TerrainLodRuntimeTest, VisualDiskMembershipAndRetainedCoverageFollowThirtyAndEightySpeedReplayEachTick)
{
    for (uint32_t const speedup : {30U, 80U}) {
        client::TerrainLodRuntimePlan plan;
        int64_t x_subcells = 65'520LL * shared::SUBCELLS_PER_CELL;
        int64_t y_subcells = 16LL * shared::SUBCELLS_PER_CELL;
        std::vector<client::TerrainLodColumnKey> rendered;
        for (uint32_t tick = 0U; tick < 80U; ++tick) {
            int64_t const step = static_cast<int64_t>(shared::MOVEMENT_SUBCELLS_PER_TICK) * speedup;
            if (tick < 20U) {
                x_subcells += step;
            } else if (tick < 40U) {
                y_subcells += step;
            } else if (tick < 60U) {
                x_subcells -= step;
            } else {
                y_subcells -= step;
            }
            uint32_t const x = shared::WorldBounds::wrapHorizontal(x_subcells / shared::SUBCELLS_PER_CELL);
            uint32_t const y = shared::WorldBounds::wrapHorizontal(y_subcells / shared::SUBCELLS_PER_CELL);
            plan.updateInterest(x, y);
            auto const center = plan.currentColumn();
            std::erase_if(rendered, [&](auto const column) { return !plan.visuallyRelevant(column); });
            plan.remember(center);
            if (std::ranges::find(rendered, center) == rendered.end()) {
                rendered.push_back(center);
            }
            EXPECT_TRUE(plan.canPublish(plan.ticket(), rendered)) << speedup << ':' << tick;
            for (int32_t dy = -41; dy <= 41; ++dy) {
                for (int32_t dx = -41; dx <= 41; ++dx) {
                    client::TerrainLodColumnKey const column{
                        static_cast<uint32_t>((static_cast<int32_t>(center.x) + dx + 4096) % 4096),
                        static_cast<uint32_t>((static_cast<int32_t>(center.y) + dy + 4096) % 4096)};
                    bool const expected = dx * dx + dy * dy <= 40 * 40;
                    ASSERT_EQ(plan.visuallyRelevant(column), expected) << speedup << ':' << tick << ':' << dx << ',' << dy;
                }
            }
            plan.presented(rendered);
        }
    }
}

TEST(TerrainLodRuntimeTest, CoalescedRecenterRetainsFrontierMembershipWithoutInvalidatingPreparedScene)
{
    client::IncrementalSources fixture;
    fixture.cross(100U, 100U);
    fixture.cross(101U, 100U);
    fixture.capture(100U, 100U);
    fixture.capture(101U, 100U);
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    plan.remember({100U, 100U});
    plan.remember({101U, 100U});
    std::array const old_rendered{client::TerrainLodColumnKey{100U, 100U}};
    std::array const promoted_rendered{old_rendered[0], client::TerrainLodColumnKey{101U, 100U}};
    plan.presented(old_rendered);
    auto const normal_ticket = plan.ticket();
    // The former independent frontier publication invalidates an otherwise current normal result.
    plan.presented(promoted_rendered);
    EXPECT_EQ(plan.publicationIssue(normal_ticket, old_rendered).failure,
        client::TerrainLodPublicationFailure::MissingPresented);
    auto const& first = fixture.columns[0];
    std::array const presented{std::shared_ptr<client::TerrainLodColumnInput const>{first, &first->input()}};
    plan.presented(old_rendered);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> transaction;
    ASSERT_TRUE(client::selectTerrainLodSceneColumns(plan, fixture.columns, presented, true, transaction));
    ASSERT_EQ(transaction.size(), 2U);
    EXPECT_EQ(transaction[0], presented[0]);
    EXPECT_EQ(transaction[1]->x, 101U);
    EXPECT_TRUE(plan.canPublish(normal_ticket, promoted_rendered));
    plan.presented(promoted_rendered);
    EXPECT_TRUE(plan.canPublish(normal_ticket, promoted_rendered));
    auto const stable_presented = transaction;
    EXPECT_FALSE(client::selectTerrainLodSceneColumns(plan, fixture.columns, stable_presented, true, transaction));
}

TEST(TerrainLodRuntimeTest, CoalescedSceneSelectsWrappedCurrentCenterBeforeMoreDistantReadyColumn)
{
    client::IncrementalSources fixture;
    for (uint32_t const x : {4095U, 0U, 1U}) {
        fixture.cross(x, 100U);
        fixture.capture(x, 100U);
    }
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(0U, 1600U);
    for (uint32_t const x : {4095U, 0U, 1U}) {
        plan.remember({x, 100U});
    }
    auto const& first = fixture.columns[0];
    std::array const presented{std::shared_ptr<client::TerrainLodColumnInput const>{first, &first->input()}};
    std::array const old_rendered{client::TerrainLodColumnKey{4095U, 100U}};
    plan.presented(old_rendered);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> transaction;
    ASSERT_TRUE(client::selectTerrainLodSceneColumns(plan, fixture.columns, presented, true, transaction));
    ASSERT_EQ(transaction.size(), 2U);
    EXPECT_EQ(transaction[1]->x, 0U);
    EXPECT_TRUE(plan.canPublish(plan.ticket(), std::array{old_rendered[0], client::TerrainLodColumnKey{0U, 100U}}));
    EXPECT_EQ(plan.publicationIssue(plan.ticket(), old_rendered).failure,
        client::TerrainLodPublicationFailure::MissingCenter);
}

TEST(TerrainLodRuntimeTest, InitialSceneTransactionKeepsAllCurrentCaptures)
{
    client::IncrementalSources fixture;
    for (uint32_t const x : {100U, 101U, 102U}) {
        fixture.cross(x, 100U);
        fixture.capture(x, 100U);
    }
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> transaction;
    EXPECT_FALSE(client::selectTerrainLodSceneColumns(plan, fixture.columns, {}, false, transaction));
    ASSERT_EQ(transaction.size(), 3U);
    for (size_t i = 0U; i < transaction.size(); ++i) {
        EXPECT_EQ(transaction[i].get(), &fixture.columns[i]->input());
    }
}

TEST(TerrainLodRuntimeTest, CoalescedSceneUsesCurrentEditedCaptureAndRetainsPlanFences)
{
    client::IncrementalSources fixture;
    fixture.cross(100U, 100U);
    fixture.cross(101U, 100U);
    fixture.capture(100U, 100U);
    auto const& old = fixture.columns[0];
    std::array const presented{std::shared_ptr<client::TerrainLodColumnInput const>{old, &old->input()}};
    ASSERT_TRUE(fixture.world.applyEditDelta({1601, 1601, 384}, shared::Block::Air, 0U, 1U, 1U));
    fixture.columns.clear();
    fixture.capture(100U, 100U);
    fixture.capture(101U, 100U);
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    plan.remember({100U, 100U});
    plan.remember({101U, 100U});
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> transaction;
    ASSERT_TRUE(client::selectTerrainLodSceneColumns(plan, fixture.columns, presented, true, transaction));
    ASSERT_EQ(transaction.size(), 2U);
    EXPECT_NE(transaction[0], presented[0]);
    EXPECT_FALSE(client::terrainLodColumnSourcesCurrent(*presented[0], fixture.world));
    EXPECT_TRUE(client::terrainLodColumnSourcesCurrent(*transaction[0], fixture.world));
    auto const ticket = plan.ticket();
    std::array const rendered{client::TerrainLodColumnKey{100U, 100U}, client::TerrainLodColumnKey{101U, 100U}};
    EXPECT_TRUE(plan.canPublish(ticket, rendered));
    plan.reset();
    EXPECT_EQ(plan.publicationIssue(ticket, rendered).failure, client::TerrainLodPublicationFailure::Generation);
}

TEST(TerrainLodRuntimeTest, PublicationDemandDrainsReadyFrontierThenIdleSelectionStaysSkipped)
{
    client::IncrementalSources fixture;
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U, 1600U);
    for (uint32_t const x : {100U, 101U, 102U}) {
        fixture.cross(x, 100U);
        fixture.capture(x, 100U);
        plan.remember({x, 100U});
    }
    auto const first = fixture.columns[0];
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> presented{
        std::shared_ptr<client::TerrainLodColumnInput const>{first, &first->input()}};
    plan.presented(std::array{client::TerrainLodColumnKey{100U, 100U}});
    uint32_t selections = 0U;
    auto select = [&] {
        if (!plan.takeSelectionDemand()) {
            return false;
        }
        ++selections;
        std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> next;
        bool const promoted = client::selectTerrainLodSceneColumns(plan, fixture.columns, presented, true, next);
        if (promoted) {
            std::vector<client::TerrainLodColumnKey> rendered;
            for (auto const& input : next) {
                rendered.push_back({input->x, input->y});
            }
            EXPECT_TRUE(plan.canPublish(plan.ticket(), rendered));
            presented = std::move(next);
            plan.presented(rendered);
        }
        return promoted;
    };
    EXPECT_TRUE(select());
    ASSERT_EQ(presented.size(), 2U);
    // No further capture events: publication itself must continue draining ready inputs.
    EXPECT_TRUE(select());
    ASSERT_EQ(presented.size(), 3U);
    EXPECT_FALSE(select());
    EXPECT_EQ(selections, 3U);
    for (uint32_t frame = 0U; frame < 100U; ++frame) {
        EXPECT_FALSE(select());
    }
    EXPECT_EQ(selections, 3U);
    // Crossing a column inside the same 64-block recenter cell still demands a pass.
    ASSERT_TRUE(plan.updateInterest(1616U, 1600U));
    EXPECT_FALSE(select());
    EXPECT_EQ(selections, 4U);
    EXPECT_FALSE(plan.updateInterest(1617U, 1600U));
    EXPECT_FALSE(select());
    EXPECT_EQ(selections, 4U);
    plan.remember({101U, 100U});
    EXPECT_FALSE(plan.takeSelectionDemand());
    plan.forget({102U, 100U});
    EXPECT_TRUE(plan.takeSelectionDemand());
    plan.remember({102U, 100U});
    EXPECT_TRUE(plan.takeSelectionDemand());
}

TEST(TerrainLodRuntimeTest, SelectionDemandCanRemainPendingUntilWorkIsAdmitted)
{
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U,1600U);
    ASSERT_TRUE(plan.selectionDemand());
    plan.acknowledgeSelectionDemand();
    EXPECT_FALSE(plan.selectionDemand());

    ASSERT_TRUE(plan.updateInterest(1616U,1600U));
    EXPECT_TRUE(plan.selectionDemand());
    EXPECT_TRUE(plan.updateInterest(1632U,1600U));
    EXPECT_TRUE(plan.selectionDemand());
    plan.acknowledgeSelectionDemand();
    EXPECT_FALSE(plan.selectionDemand());
}

TEST(TerrainLodRuntimeTest, OrdinaryLocalSelectionPrefersCapturesInsidePresentedDisk)
{
    client::IncrementalSources fixture;
    constexpr client::TerrainLodColumnCoordinate VISUAL_CENTER{2'048U, 2'048U};
    constexpr client::TerrainLodColumnKey CURRENT{2'053U, 2'048U};
    constexpr client::TerrainLodColumnKey OUTSIDE{2'088U, 2'047U};
    constexpr client::TerrainLodColumnKey INSIDE{2'013U, 2'048U};
    for (auto const key : {OUTSIDE, INSIDE}) {
        fixture.cross(key.x, key.y);
        fixture.capture(key.x, key.y);
    }
    ASSERT_EQ(fixture.columns.size(), 2U);

    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(CURRENT.x * 16U, CURRENT.y * 16U);
    ASSERT_TRUE(plan.visuallyRelevant(OUTSIDE));
    ASSERT_TRUE(plan.visuallyRelevant(INSIDE));
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> columns;
    std::vector<client::TerrainLodColumnCoordinate> targets;
    ASSERT_TRUE(client::selectTerrainLodLocalColumns(plan, fixture.columns, {}, columns, targets));
    EXPECT_EQ(targets, (std::vector{client::TerrainLodColumnCoordinate{OUTSIDE.x, OUTSIDE.y}}));

    ASSERT_TRUE(client::selectTerrainLodLocalColumns(plan, fixture.columns, {}, columns, targets, {}, VISUAL_CENTER));
    EXPECT_EQ(targets, (std::vector{client::TerrainLodColumnCoordinate{INSIDE.x, INSIDE.y}}));
    EXPECT_FALSE(client::selectTerrainLodLocalColumns(
        plan, std::span{fixture.columns}.first(1U), {}, columns, targets, {}, VISUAL_CENTER));
    EXPECT_TRUE(targets.empty());
}

TEST(TerrainLodRuntimeTest, LocalCurrentFootprintCatchesFourColumnMotionWhileFarTicketRemainsHeld)
{
    client::IncrementalSources fixture;
    for (auto q : {client::TerrainLodColumnKey{100U,100U},{104U,100U},{103U,100U},{105U,100U},{104U,99U},{104U,101U}}) {
        fixture.cross(q.x,q.y);
        fixture.capture(q.x,q.y);
    }
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U,1600U);
    for (auto const& v : fixture.columns) {
        plan.remember({v->input().x,v->input().y});
    }
    std::array const old_rendered{client::TerrainLodColumnKey{100U,100U}};
    plan.presented(old_rendered);
    auto held_far = plan.ticket();
    plan.updateInterest(1664U,1600U);
    EXPECT_EQ(plan.publicationIssue(held_far,old_rendered).failure,client::TerrainLodPublicationFailure::MissingCenter);
    auto const& old = fixture.columns.front();
    std::array const presented{std::shared_ptr<client::TerrainLodColumnInput const>{old,&old->input()}};
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> local;
    std::vector<client::TerrainLodColumnCoordinate> targets;
    ASSERT_TRUE(client::selectTerrainLodLocalColumns(plan,fixture.columns,presented,local,targets));
    ASSERT_EQ(targets.size(),5U);
    EXPECT_EQ(local.front(),presented.front());
    EXPECT_EQ(targets.front(),(client::TerrainLodColumnCoordinate{104U,100U}));
    std::vector<client::TerrainLodColumnKey> rendered;
    for (auto const& input : local) {
        rendered.push_back({input->x,input->y});
    }
    EXPECT_TRUE(plan.canPublish(held_far,rendered));
    // The far ticket is still the same transaction, and its base captures are immutable.
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> rebased;
    ASSERT_TRUE(client::mergeTerrainLodLocalColumns(presented,local,targets,rebased));
    EXPECT_EQ(rebased,local);
    EXPECT_EQ(presented.size(),1U);
    plan.presented(rendered);
    EXPECT_TRUE(plan.canPublish(held_far,rendered));
    plan.reset();
    EXPECT_EQ(plan.publicationIssue(held_far,rendered).failure,client::TerrainLodPublicationFailure::Generation);
}

TEST(TerrainLodRuntimeTest, SeveralOverlappingNearUpdatesRebaseFarWithoutResurrectingItsRetiredColumns)
{
    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    auto input = [](uint32_t x) -> Input {
        auto value = std::make_shared<client::TerrainLodColumnInput>(); value->x = x; value->y = 100U; return value;
    };
    auto p = input(100U), old_outside = input(130U), fresh_outside = input(130U);
    std::vector<Input> far{p,fresh_outside}, live{p,old_outside};
    std::vector<client::TerrainLodColumnCoordinate> union_targets;
    for (uint32_t x = 104U; x != 108U; ++x) {
        auto newest = input(x);
        live.push_back(newest);
        union_targets.push_back({x,100U});
        std::vector<Input> merged;
        ASSERT_TRUE(client::mergeTerrainLodLocalColumns(far,live,union_targets,merged));
        EXPECT_EQ(merged[1],fresh_outside);
        EXPECT_EQ(merged.back(),newest);
        EXPECT_EQ(far.size(),2U);
    }
    std::array const retired_far{p};
    std::vector<Input> retired;
    ASSERT_TRUE(client::mergeTerrainLodLocalColumns(retired_far,live,union_targets,retired));
    EXPECT_EQ(retired.size(),5U);
    EXPECT_FALSE(std::ranges::any_of(retired,[](auto const& v) { return v->x == 130U; }));
    union_targets.push_back({999U,100U});
    EXPECT_FALSE(client::mergeTerrainLodLocalColumns(far,live,union_targets,retired));
}

TEST(TerrainLodRuntimeTest, PendingFarRepairIncludesNewCaptureWithoutChangingItsGeneration)
{
    client::IncrementalSources fixture;
    fixture.cross(100U,100U); fixture.capture(100U,100U);
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1600U,1600U); plan.remember({100U,100U});
    std::array const first_rendered{client::TerrainLodColumnKey{100U,100U}};
    plan.presented(first_rendered);
    auto ticket = plan.ticket();
    auto const& first = fixture.columns.front();
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> far{
        std::shared_ptr<client::TerrainLodColumnInput const>{first,&first->input()}};
    auto const generation = ticket.generation;
    for (uint32_t x : {104U,108U}) {
        fixture.cross(x,100U);
        fixture.capture(x,100U);
        plan.updateInterest(x*16U,1600U);
        plan.remember({x,100U});
        std::vector<client::TerrainLodColumnKey> old_rendered;
        for (auto const& v : far) {
            old_rendered.push_back({v->x,v->y});
        }
        EXPECT_EQ(plan.publicationIssue(ticket,old_rendered).failure,client::TerrainLodPublicationFailure::MissingCenter);
        std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> ready, repaired;
        std::vector<client::TerrainLodColumnCoordinate> targets;
        ASSERT_TRUE(client::selectTerrainLodLocalColumns(plan,fixture.columns,far,ready,targets));
        ASSERT_TRUE(client::mergeTerrainLodLocalColumns(far,ready,targets,repaired));
        ticket.columns.clear();
        std::vector<client::TerrainLodColumnKey> rendered;
        for (auto const& v : repaired) {
            ticket.columns.push_back({v->x,v->y});
            rendered.push_back({v->x,v->y});
        }
        EXPECT_EQ(ticket.generation,generation);
        EXPECT_TRUE(plan.canPublish(ticket,rendered));
        far = std::move(repaired);
        plan.presented(rendered);
    }
    EXPECT_EQ(far.size(),3U);
}

TEST(TerrainLodRuntimeTest, AcceptedExactEditRemainsAnExplicitRimRepairTarget)
{
    client::IncrementalSources fixture;
    fixture.cross(104U,100U); fixture.capture(104U,100U);
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(1664U,1600U); plan.remember({104U,100U});
    auto edited = std::make_shared<client::TerrainLodColumnInput>(fixture.columns.front()->input());
    edited->edge_heights[0][0]++;
    std::array const presented{std::shared_ptr<client::TerrainLodColumnInput const>{edited}};
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> columns;
    std::vector<client::TerrainLodColumnCoordinate> targets;
    EXPECT_FALSE(client::selectTerrainLodLocalColumns(plan,fixture.columns,presented,columns,targets));
    std::array const dirty{client::TerrainLodColumnCoordinate{104U,100U}};
    ASSERT_TRUE(client::selectTerrainLodLocalColumns(plan,fixture.columns,presented,columns,targets,dirty));
    EXPECT_EQ(targets,(std::vector<client::TerrainLodColumnCoordinate>{dirty[0]}));
    ASSERT_EQ(columns.size(),1U);
    EXPECT_EQ(columns[0],presented[0]); // A stale discovery capture must not overwrite the accepted edit.
}


TEST(TerrainLodRuntimeTest, LocalCurrentUnitIncludesReadyCornerFootprintAcrossWrap)
{
    client::IncrementalSources fixture;
    for (auto q : {client::TerrainLodColumnKey{4095U,4095U},{0U,4095U},{4095U,0U},{0U,0U}}) {
        fixture.cross(q.x,q.y);
        fixture.capture(q.x,q.y);
    }
    client::TerrainLodRuntimePlan plan;
    plan.updateInterest(65'535U,65'535U);
    for (auto const& v : fixture.columns) {
        plan.remember({v->input().x,v->input().y});
    }
    std::vector<std::shared_ptr<client::TerrainLodColumnInput const>> columns;
    std::vector<client::TerrainLodColumnCoordinate> targets;
    ASSERT_TRUE(client::selectTerrainLodLocalColumns(plan,fixture.columns,{},columns,targets));
    EXPECT_EQ(targets.size(),4U);
    EXPECT_NE(std::ranges::find(targets,client::TerrainLodColumnCoordinate{0U,0U}),targets.end());
}

#if defined(MC_TERRAIN_RUNTIME_CPU_CALLTHROUGH)
// This private target exercises the production coordinator with a CPU renderer boundary.
// It deliberately performs no Vulkan, GLFW, frame submission or native coverage qualification.

namespace client {
struct TerrainLodRuntimeTestAccess final {
    static bool selectionDemand(TerrainLodRuntime const& runtime)
    {
        return runtime.m_plan.selectionDemand();
    }

    static std::stop_token localCancellationToken(TerrainLodRuntime const& runtime)
    {
        return runtime.m_local_cancellation.get_token();
    }

    static bool localJobIsNearCurrentWithFarReady(TerrainLodRuntime const& runtime)
    {
        return runtime.m_local_job.valid() && !runtime.m_local_job_far
            && runtime.m_local_job_started_with_far_ready && runtime.m_local_job_targets_current;
    }

    static bool farReady(TerrainLodRuntime const& runtime)
    {
        return runtime.m_far_ready.has_value();
    }

    static bool presentedHasColumn(TerrainLodRuntime const& runtime, uint32_t x, uint32_t y)
    {
        return runtime.m_presented_scene && std::ranges::any_of(runtime.m_presented_scene->leaves, [=](auto leaf) {
            return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == x && leaf.y == y;
        });
    }

    static std::shared_ptr<TerrainLodScene const> presentedScene(TerrainLodRuntime const& runtime)
    {
        return runtime.m_presented_scene;
    }

    static bool holdFarReadyFromPresented(TerrainLodRuntime& runtime)
    {
        if (!runtime.m_presented_scene || runtime.m_pending) {
            return false;
        }
        if (!runtime.m_far_ready) {
            runtime.m_far_ready = TerrainLodRuntime::JobResult{
                runtime.m_presented_scene, runtime.m_presented_columns};
            runtime.m_far_ticket = runtime.m_plan.ticket();
        }
        return true;
    }

    static bool installHeldPendingScene(TerrainLodRuntime& runtime)
    {
        if (runtime.m_pending || !runtime.m_presented_scene) {
            return false;
        }
        runtime.m_pending = runtime.m_presented_scene;
        runtime.m_pending_columns = runtime.m_presented_columns;
        runtime.m_pending_ticket = runtime.m_plan.ticket();
        runtime.m_pending_unit.reset();
        runtime.m_pending_base.reset();
        runtime.m_pending_far = false;
        return true;
    }

    static auto capturedInputs(TerrainLodRuntime const& runtime)
    {
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> inputs;
        for (auto const& column : runtime.m_columns) {
            inputs.emplace_back(column, &column->input());
        }
        return inputs;
    }

    static auto dispatchSnapshot(TerrainLodRuntime const& runtime)
    {
        std::vector<std::shared_ptr<TerrainLodColumnInput const>> inputs;
        for (auto key : runtime.m_job_ticket.columns) {
            auto input = std::ranges::find_if(runtime.m_columns, [=](auto const& value) {
                return value->input().x == key.x && value->input().y == key.y;
            });
            if (input != runtime.m_columns.end()) {
                inputs.emplace_back(*input, &(*input)->input());
            }
        }
        return std::pair{runtime.m_job_ticket, inputs};
    }
};
namespace {
std::function<void(TerrainLodScene const&, bool)> cpu_scene_observer;
bool cpu_hold_upload = false;
bool cpu_hold_scene_only = false;
uint64_t cpu_upserts = 0U;
uint64_t cpu_replacements = 0U;
uint64_t cpu_replacement_attempts = 0U;
uint64_t cpu_clears = 0U;
}
struct VulkanRenderer::Impl final {
    std::shared_ptr<TerrainLodScene const> scene;
};
VulkanRenderer::VulkanRenderer(std::shared_ptr<core::graphics::vulkan::PresentationContext>,
    ShaderAssets const&, VulkanRendererOptions)
    : m_impl(std::make_unique<Impl>())
{ }
VulkanRenderer::~VulkanRenderer()
{
    cpu_scene_observer = {};
}
bool VulkanRenderer::upsertTerrainLodScene(std::shared_ptr<TerrainLodScene const> scene,
    std::chrono::steady_clock::time_point, TerrainLodRendererDiagnostics*)
{
    detail::validateTerrainLodGroups(*scene);
    ++cpu_upserts;
    if (cpu_scene_observer) {
        cpu_scene_observer(*scene, !cpu_hold_upload);
    }
    if (cpu_hold_upload) {
        return false;
    }
    m_impl->scene = std::move(scene);
    return true;
}
std::shared_ptr<TerrainLodScene const> VulkanRenderer::replaceTerrainLodGroups(
    std::shared_ptr<TerrainLodScene const> patch,
    std::chrono::steady_clock::time_point, TerrainLodRendererDiagnostics*)
{
    if (!m_impl->scene) {
        return {};
    }
    std::vector<StoneIndirectDraws::Range> ranges;
    for (auto const& group : m_impl->scene->groups) {
        ranges.push_back(group.range);
    }
    auto scene = detail::terrainLodReplacementPlan(*m_impl->scene, ranges, *patch).scene;
    ++cpu_replacement_attempts;
    bool const hold_replacement = cpu_hold_upload && !cpu_hold_scene_only;
    if (cpu_scene_observer && !cpu_hold_scene_only) {
        cpu_scene_observer(*scene, !hold_replacement);
    }
    if (hold_replacement) {
        return {};
    }
    m_impl->scene = std::move(scene);
    ++cpu_replacements;
    return m_impl->scene;
}
void VulkanRenderer::clearTerrainLodScene()
{
    m_impl->scene.reset();
    ++cpu_clears;
}
RendererRuntimeInfo VulkanRenderer::runtimeInfo() const
{
    RendererRuntimeInfo result;
    result.terrain_lod_scene = m_impl->scene;
    result.terrain_lod_active = static_cast<bool>(m_impl->scene);
    return result;
}
namespace {
class CpuUnusedShaderAssets final : public ShaderAssets {
public:
    core::kernel::SpirvModule load(std::string_view) const override
    {
        throw std::logic_error("CPU renderer stub must not load GPU shaders");
    }
};
bool cpuHasActual(TerrainLodScene const& scene, uint32_t x, uint32_t y)
{
    return std::ranges::any_of(scene.leaves, [=](auto leaf) {
        return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == x && leaf.y == y;
    });
}
}
} // namespace client

TEST(TerrainLodRuntimeTest, CpuCallthroughPublishesNearBeforeFarAndPreservesEditResetGuards)
{
    RecordProperty("renderer_backend", "CPU_LINK_STUB_NO_GPU");
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    auto terrain = std::make_shared<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    RecordProperty("terrain_model", "NativeV10");
    RecordProperty("terrain_configuration_identity", terrain->identity().configuration_identity);
    shared::SparseWorld actual{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(actual.setTerrainIdentity(terrain->identity()));
    uint32_t constexpr PX = 2'048U, PY = 2'048U;
    for (int32_t dx = -3; dx <= 15; ++dx) {
        for (int32_t dy = -3; dy <= 3; ++dy) {
            ASSERT_TRUE(actual.publishAuthoritativeSurface(terrain->sampleSurface({int32_t(PX) + dx, int32_t(PY) + dy, 0})));
        }
    }
    client::CpuUnusedShaderAssets assets;
    client::VulkanRenderer renderer{nullptr, assets};
    auto runtime = std::make_unique<client::TerrainLodRuntime>();
    std::optional<shared::Player> player{shared::Player{.id = 1U, .x = int32_t(PX * 16U),
        .y = int32_t(PY * 16U), .z = 764, .ch = 'P'}};
    shared::TerrainSessionIdentity session{.join_epoch = 7U, .terrain = terrain->identity(), .configuration = configuration};
    uint64_t coverage = 1U, calls = 0U;
    bool local_before_far_ready = false, source_advanced_during_far_job = false;
    bool monitor_near_publication_with_far_ready = false;
    bool near_published_while_far_ready = false;
    uint64_t source_rejected_before_advance = 0U;
    bool wrong_epoch = false, after_reset = false;
    using DispatchSnapshot = decltype(client::TerrainLodRuntimeTestAccess::dispatchSnapshot(*runtime));
    std::vector<DispatchSnapshot> reset_snapshots;
    uint64_t seen_dispatch = 0U;
    uint32_t base_center = PX * 16U;
    client::cpu_hold_upload = false;
    client::cpu_upserts = client::cpu_replacements = client::cpu_replacement_attempts = client::cpu_clears = 0U;
    client::cpu_scene_observer = [&](client::TerrainLodScene const& scene, bool accepted) {
        auto progress = runtime->progress();
        if (accepted && player->x == int32_t((PX + 4U) * 16U) && scene.center_x == base_center
            && client::cpuHasActual(scene, PX + 4U, PY) && progress.job_outstanding && !progress.job_ready) {
            local_before_far_ready = true;
            player->x = int32_t((PX + 9U) * 16U);
            source_rejected_before_advance = progress.source_rejected;
            auto const snapshot = client::TerrainLodRuntimeTestAccess::dispatchSnapshot(*runtime);
            for (auto const& input : snapshot.second) {
                auto tile = actual.authoritativeTile({int32_t(input->x), int32_t(input->y), 0});
                if (!tile) {
                    continue;
                }
                auto& column = tile->columns.front();
                column.top_material = column.top_material == shared::Block::Snow
                    ? shared::Block::Grass : shared::Block::Snow;
                if (actual.removeAuthoritativeSurface(tile->coordinate)) {
                    source_advanced_during_far_job = actual.publishAuthoritativeSurface(*tile);
                    break;
                }
            }
        }
        if (accepted && monitor_near_publication_with_far_ready
            && player->x == int32_t((PX + 9U) * 16U) && client::cpuHasActual(scene, PX + 9U, PY)
            && client::TerrainLodRuntimeTestAccess::farReady(*runtime)) {
            near_published_while_far_ready = true;
        }
        if (accepted && after_reset && (scene.session_join_epoch != session.join_epoch || scene.coverage_epoch != coverage)) {
            wrong_epoch = true;
        }
    };
    auto update = [&] {
        ++calls;
        runtime->update(terrain, &actual, player, renderer, std::chrono::steady_clock::now() + std::chrono::milliseconds{20},
            session, coverage, true);
        if (after_reset && runtime->progress().dispatched != seen_dispatch) {
            seen_dispatch = runtime->progress().dispatched;
            auto snapshot = client::TerrainLodRuntimeTestAccess::dispatchSnapshot(*runtime);
            EXPECT_EQ(snapshot.second.size(), snapshot.first.columns.size());
            reset_snapshots.push_back(std::move(snapshot));
        }
    };
    auto pump = [&](auto predicate, std::chrono::seconds timeout = std::chrono::seconds{12}) {
        auto until = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < until) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return predicate();
    };
    ASSERT_TRUE(pump([&] { return runtime->active(); })) << "initial source/worker/upsert never activated";
    auto first = renderer.runtimeInfo().terrain_lod_scene;
    ASSERT_TRUE(first);
    ASSERT_TRUE(client::cpuHasActual(*first, PX, PY));
    base_center = first->center_x;
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::installHeldPendingScene(*runtime));
    client::cpu_hold_upload = true;
    player->x = int32_t((PX + 1U) * 16U);
    update();
    bool const retained_while_pending_held = client::TerrainLodRuntimeTestAccess::selectionDemand(*runtime);
    client::cpu_hold_upload = false;
    EXPECT_TRUE(retained_while_pending_held)
        << "near selection demand must survive a held pending scene until work is admitted";
    RecordProperty("selection_demand_retained_while_pending_held", retained_while_pending_held);
    player->x = int32_t((PX + 4U) * 16U);
    ASSERT_TRUE(pump([&] { return local_before_far_ready; }))
        << "no observed near publication while the actual full worker was nonready";
    RecordProperty("near_published_while_real_far_nonready", true);
    ASSERT_TRUE(source_advanced_during_far_job);
    RecordProperty("source_revision_advanced_while_far_pending", true);
    ASSERT_TRUE(actual.authoritativeColumn((PX + 9U) * 16U, PY * 16U))
        << "adjacent current authority must exist before testing near admission";
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::holdFarReadyFromPresented(*runtime))
        << "expected a presented scene before holding a completed far candidate";
    ASSERT_FALSE(client::TerrainLodRuntimeTestAccess::presentedHasColumn(*runtime, PX + 9U, PY))
        << "priority fixture must begin with the new current column absent from the presented base";
    monitor_near_publication_with_far_ready = true;
    update();
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::selectionDemand(*runtime))
        << "moving to the adjacent authoritative column must create near demand";
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::farReady(*runtime));
    bool const near_admitted_with_far_ready = pump([&] {
        return !client::TerrainLodRuntimeTestAccess::selectionDemand(*runtime)
            && client::TerrainLodRuntimeTestAccess::localJobIsNearCurrentWithFarReady(*runtime)
            && client::TerrainLodRuntimeTestAccess::farReady(*runtime);
    });
    EXPECT_TRUE(near_admitted_with_far_ready)
        << "current near work must take the local slot after existing work clears while the completed far scene stays queued";
    RecordProperty("near_admitted_while_far_ready", near_admitted_with_far_ready);
    ASSERT_TRUE(near_admitted_with_far_ready);
    ASSERT_TRUE(pump([&] { return near_published_while_far_ready; }))
        << "current near scene must publish before the queued far result is rebased";
    RecordProperty("near_published_while_far_ready", near_published_while_far_ready);
    ASSERT_TRUE(pump([&] {
        return runtime->progress().source_rejected > source_rejected_before_advance;
    })) << "full work from the old source revision was not rejected";
    RecordProperty("stale_far_job_rejected_after_source_advance", true);
    player->x = int32_t((PX + 5U) * 16U);
    ASSERT_TRUE(pump([&] {
        auto scene = renderer.runtimeInfo().terrain_lod_scene;
        return scene && client::cpuHasActual(*scene, PX + 5U, PY);
    }));
    // Hold only the explicitly stubbed renderer boundary, never the production worker.
    auto attempts = client::cpu_upserts;
    client::cpu_hold_upload = true;
    ASSERT_TRUE(pump([&] { return client::cpu_upserts > attempts; }));
    auto rejected = runtime->progress().plan_rejected;
    player->x = int32_t((PX + 11U) * 16U);
    ASSERT_TRUE(pump([&] { return runtime->progress().plan_rejected > rejected; }));
    client::cpu_hold_upload = false;
    ASSERT_TRUE(pump([&] {
        auto scene = renderer.runtimeInfo().terrain_lod_scene;
        return scene && client::cpuHasActual(*scene, PX + 11U, PY);
    }));
    auto column = actual.authoritativeColumn((PX + 11U) * 16U, PY * 16U);
    ASSERT_TRUE(column);
    auto const scene_before_edit = client::TerrainLodRuntimeTestAccess::presentedScene(*runtime);
    ASSERT_TRUE(scene_before_edit);
    client::cpu_hold_upload = true;
    uint64_t const attempts_before_edit = client::cpu_replacement_attempts;
    ASSERT_TRUE(actual.applyEditDelta({int32_t((PX + 11U) * 16U), int32_t(PY * 16U), column->highest_solid_z + 1},
        shared::Block::Stone, 0U, 1U, 1U));
    ASSERT_TRUE(pump([&] { return client::cpu_replacement_attempts > attempts_before_edit; }))
        << "edited geometry was not submitted to the held renderer upload";
    EXPECT_EQ(renderer.runtimeInfo().terrain_lod_scene, scene_before_edit)
        << "a held edit upload must leave the renderer scene unchanged";
    EXPECT_EQ(client::TerrainLodRuntimeTestAccess::presentedScene(*runtime), scene_before_edit)
        << "a held edit upload must not advance Runtime's presented scene";
    client::cpu_hold_upload = false;
    auto edits = client::cpu_replacements;
    ASSERT_TRUE(pump([&] { return client::cpu_replacements > edits; })) << "actual edit lane never replaced groups";
    auto const published_edit_scene = renderer.runtimeInfo().terrain_lod_scene;
    ASSERT_TRUE(published_edit_scene);
    EXPECT_NE(published_edit_scene, scene_before_edit);
    EXPECT_EQ(client::TerrainLodRuntimeTestAccess::presentedScene(*runtime), published_edit_scene)
        << "Runtime must adopt the exact immutable scene returned after renderer publication";
    auto capture = client::captureTerrainLodColumn(actual, PX + 11U, PY);
    ASSERT_TRUE(capture);
    std::array const edited{std::make_shared<client::TerrainLodColumnInput const>(*capture)};
    auto canonical_patch = client::buildTerrainLodColumnPatch(terrain->identity(), edited);
    auto scene = renderer.runtimeInfo().terrain_lod_scene;
    auto group = std::ranges::find_if(scene->groups, [&](auto const& g) {
        return g.origin_x == (PX + 11U) * 16U && g.origin_y == PY * 16U && g.extent == 16U;
    });
    ASSERT_NE(group, scene->groups.end());
    ASSERT_EQ(canonical_patch->groups.size(), 1U);
    bool const canonical_edit = std::ranges::equal(client::terrainLodGroupFaces(*group),
        client::terrainLodGroupFaces(canonical_patch->groups.front()));
    EXPECT_TRUE(canonical_edit);
    RecordProperty("canonical_edit_faces_match", canonical_edit);
    RecordProperty("pending_center_repair_completed", true);
    RecordProperty("update_calls_before_reset", calls);
    RecordProperty("captured_columns_before_reset", runtime->progress().captured);
    RecordProperty("edit_replacements_before_reset", client::cpu_replacements);
    player->x = int32_t((PX + 12U) * 16U);
    ASSERT_TRUE(pump([&] { return runtime->progress().job_outstanding; }));
    runtime->reset(&renderer);
    ASSERT_FALSE(renderer.runtimeInfo().terrain_lod_scene);
    ++session.join_epoch;
    ++coverage;
    seen_dispatch = runtime->progress().dispatched;
    after_reset = true;
    bool const reset_recovered = pump([&] {
        auto current = renderer.runtimeInfo().terrain_lod_scene;
        return current && current->session_join_epoch == session.join_epoch && current->coverage_epoch == coverage
            && client::cpuHasActual(*current, PX + 12U, PY);
    });
    bool const all_captured = pump([&] {
        return client::TerrainLodRuntimeTestAccess::capturedInputs(*runtime).size() == 85U;
    });
    EXPECT_TRUE(all_captured);
    auto complete_inputs = client::TerrainLodRuntimeTestAccess::capturedInputs(*runtime);
    EXPECT_EQ(complete_inputs.size(), 85U);
    RecordProperty("reset_recovered", reset_recovered);
    RecordProperty("reset_failed_jobs", runtime->progress().failed);
    RecordProperty("reset_last_error", runtime->progress().last_error_text.data());
    RecordProperty("reset_dispatch_snapshots", reset_snapshots.size());
    EXPECT_FALSE(reset_snapshots.empty());
    EXPECT_FALSE(wrong_epoch);
    EXPECT_GT(runtime->progress().captured, 0U);
    EXPECT_GT(client::cpu_clears, 0U);
    RecordProperty("actual_update_calls", calls);
    RecordProperty("actual_captured_columns", runtime->progress().captured);
    RecordProperty("renderer_stub_upsert_attempts", client::cpu_upserts);
    RecordProperty("real_edit_replacement_calls", client::cpu_replacements);
    RecordProperty("plan_rejections_during_blocked_upsert", runtime->progress().plan_rejected - rejected);
    RecordProperty("stale_epoch_published", wrong_epoch);
    client::cpu_scene_observer = {};
    runtime.reset(); // Actual destructor cancels and joins every worker before diagnostic replay.
    if (!reset_snapshots.empty()) {
        auto const& ticket = reset_snapshots.back().first;
        auto const& inputs = complete_inputs;
        RecordProperty("direct_replay_column_count", inputs.size());
        char const* prefix = std::getenv("MC_RUNTIME_COLD_FAILURE_PREFIX");
        if (prefix != nullptr) {
            std::ofstream context(std::string(prefix) + ".captures.tsv");
            context << "model\tNativeV10\nseed\t" << terrain->identity().seed
                << "\nrevision\t" << terrain->identity().revision << "\nconfiguration\t"
                << terrain->identity().configuration_identity << "\ncenter\t" << ticket.center_x << '\t' << ticket.center_y
                << "\ngeneration\t" << ticket.generation
                << "\njoin\t" << session.join_epoch << "\ncoverage\t" << coverage << '\n';
            for (auto const& input : inputs) {
                context << "column\t" << input->x << '\t' << input->y << '\t' << input->chunks.size()
                    << '\t' << input->authority_profiles.size() << '\n';
                for (auto const& edge : input->edge_heights) {
                    context << "edge";
                    for (uint16_t height : edge) {
                        context << '\t' << height;
                    }
                    context << '\n';
                }
                for (auto const& version : input->authority_sources) {
                    context << "authority\t" << version.coordinate.x << '\t' << version.coordinate.y << '\t'
                        << version.coordinate.z << '\t' << version.revision << '\t' << version.content_hash
                        << '\t' << version.dense_content_hash << '\n';
                }
                for (auto const& version : input->sources) {
                    context << "chunk\t" << version.coordinate.x << '\t' << version.coordinate.y << '\t'
                        << version.coordinate.z << '\t' << version.identity.content_hash << '\t' << version.edit_revision << '\n';
                }
                for (auto const& version : input->edit_sources) {
                    context << "edit\t" << version.coordinate.x << '\t' << version.coordinate.y << '\t'
                        << version.coordinate.z << '\t' << version.revision << '\n';
                }
            }
            setenv("MC_RIVER_SCENE_EVIDENCE", prefix, 1);
            setenv("MC_RIVER_SCENE_FIRST_FAILURE_ONLY", "1", 1);
        }
        try {
            auto direct = client::buildTerrainLodSceneFromColumns(*terrain, ticket.center_x, ticket.center_y, inputs,
                {}, nullptr, {}, false, client::TerrainLodColumnCoordinate{ticket.center_x / 16U, ticket.center_y / 16U});
            RecordProperty("direct_same_capture_failed", false);
            static_cast<void>(direct);
        } catch (std::exception const& error) {
            RecordProperty("direct_same_capture_failed", true);
            RecordProperty("direct_same_capture_error", error.what());
            ADD_FAILURE() << "same complete captured input set must build: " << error.what();
        }
    }
    EXPECT_TRUE(reset_recovered) << "reset must recover; preserve the producer error rather than weaken this assertion";
}

enum class CurrentFirstInterruption {
    MoveHeldToRetained, MoveHeldToAdmitted, MoveHeldSettle, EditHeld,
    EscapeHeldSettle, EditThenEscapeHeldSettle, RepeatEscapeHeldSettle, EscapeRunningSettle,
};

static void verifyCurrentFirstRecenter(CurrentFirstInterruption const interruption)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    auto terrain = std::make_shared<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    shared::SparseWorld actual{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(actual.setTerrainIdentity(terrain->identity()));
    constexpr uint32_t BASE = 2'048U, FRONTIER = 2'097U;
    for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
            std::array{0, -1}, std::array{0, 1}}) {
        ASSERT_TRUE(actual.publishAuthoritativeSurface(terrain->sampleSurface(
            {int32_t(BASE) + offset[0], int32_t(BASE) + offset[1], 0})));
    }
    client::CpuUnusedShaderAssets assets;
    client::VulkanRenderer renderer{nullptr, assets};
    client::TerrainLodRuntime runtime;
    std::optional<shared::Player> player{shared::Player{.id = 1U, .x = int32_t(BASE * 16U),
        .y = int32_t(BASE * 16U), .z = 764, .ch = 'P'}};
    shared::TerrainSessionIdentity const session{
        .join_epoch = 7U, .terrain = terrain->identity(), .configuration = configuration};
    auto update = [&] {
        runtime.update(terrain, &actual, player, renderer, std::chrono::steady_clock::now() + std::chrono::milliseconds{20},
            session, 1U);
    };
    auto pump = [&](auto predicate, std::chrono::seconds timeout) {
        auto const until = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < until) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return predicate();
    };
    client::cpu_hold_upload = false;
    ASSERT_TRUE(pump([&] { return runtime.active(); }, std::chrono::seconds{60}));
    auto const base = renderer.runtimeInfo().terrain_lod_scene;
    ASSERT_TRUE(base);
    ASSERT_TRUE(client::cpuHasActual(*base, BASE, BASE));
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::holdFarReadyFromPresented(runtime));

    bool const repeat_escape = interruption == CurrentFirstInterruption::RepeatEscapeHeldSettle;
    bool const running_settle = interruption == CurrentFirstInterruption::EscapeRunningSettle;
    bool const escape_settle = interruption == CurrentFirstInterruption::EscapeHeldSettle
        || interruption == CurrentFirstInterruption::EditThenEscapeHeldSettle || repeat_escape || running_settle;
    bool const edit_before_escape = interruption == CurrentFirstInterruption::EditThenEscapeHeldSettle;
    uint32_t const escape_step = repeat_escape ? shared::FULL_DETAIL_RADIUS_CHUNKS + 4U : 4U;
    uint32_t const destinations = repeat_escape ? 3U : escape_settle ? 1U : 0U;
    for (uint32_t destination = 0U; destination <= destinations; ++destination) {
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                ASSERT_TRUE(actual.publishAuthoritativeSurface(terrain->sampleSurface(
                    {int32_t(FRONTIER + destination * escape_step) + dx, int32_t(BASE) + dy, 0})));
            }
        }
    }
    constexpr client::TerrainLodColumnCoordinate first_visual{FRONTIER, BASE};
    client::TerrainLodColumnCoordinate next_visual{FRONTIER + (escape_settle ? escape_step : 1U), BASE};
    bool const edit_held = interruption == CurrentFirstInterruption::EditHeld;
    bool const move_to_retained = interruption == CurrentFirstInterruption::MoveHeldToRetained;
    bool const move_during_settle = interruption == CurrentFirstInterruption::MoveHeldSettle || escape_settle;
    constexpr client::TerrainLodColumnCoordinate retained_visual{BASE, BASE};
    auto settled_visual = edit_held ? first_visual : move_to_retained ? retained_visual : next_visual;
    bool prepared_first = false, first_includes_next = false, published_settle = false;
    bool held_first_settle = false, escaped = false, repaired_escape_before_settle = false;
    auto held_visual = first_visual;
    uint32_t first_transient_columns = 0U;
    std::shared_ptr<client::TerrainLodScene const> expected_edit;
    client::TerrainLodScene const* mixed_candidate = nullptr;
    bool published_far = false;
    size_t mixed_attempts = 0U;
    size_t mixed_publications = 0U;
    client::cpu_scene_observer = [&](client::TerrainLodScene const& scene, bool accepted) {
        if (!scene.visual_center) {
            if (escape_settle && escaped) {
                EXPECT_TRUE(accepted);
                EXPECT_TRUE(client::cpuHasActual(scene, next_visual.x, next_visual.y));
                EXPECT_TRUE(client::cpuHasActual(scene, BASE, BASE));
                EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, actual));
                if (repeat_escape) {
                    EXPECT_LE(scene.actual_column_count, base->actual_column_count + first_transient_columns)
                        << "repeated current-first repairs replace rather than accumulate transient columns";
                }
                repaired_escape_before_settle = accepted
                    && client::cpuHasActual(scene, next_visual.x, next_visual.y);
                if (expected_edit) {
                    auto group = std::ranges::find_if(scene.groups, [&](auto const& entry) {
                        return entry.origin_x == FRONTIER * 16U && entry.origin_y == BASE * 16U
                            && entry.extent == 16U;
                    });
                    ASSERT_NE(group, scene.groups.end());
                    ASSERT_EQ(expected_edit->groups.size(), 1U);
                    EXPECT_TRUE(std::ranges::equal(client::terrainLodGroupFaces(*group),
                        client::terrainLodGroupFaces(expected_edit->groups.front())));
                } else {
                    EXPECT_EQ(client::cpuHasActual(scene, first_visual.x, first_visual.y),
                        client::terrainLodVisualColumn(first_visual.x, first_visual.y, next_visual.x, next_visual.y))
                        << "only superseded transient columns outside existing visual relevance may retire";
                }
                return;
            }
            ++mixed_attempts;
            if (mixed_candidate == nullptr) {
                mixed_candidate = &scene;
            }
            EXPECT_EQ(&scene, mixed_candidate);
            if (accepted && interruption == CurrentFirstInterruption::MoveHeldToAdmitted) {
                EXPECT_TRUE(client::cpuHasActual(scene, next_visual.x, next_visual.y));
            }
            mixed_publications += accepted ? 1U : 0U;
            prepared_first = true;
            first_transient_columns = scene.actual_column_count - base->actual_column_count;
            first_includes_next = client::cpuHasActual(scene, next_visual.x, next_visual.y);
            EXPECT_TRUE(client::cpuHasActual(scene, BASE, BASE));
            EXPECT_TRUE(client::cpuHasActual(scene, first_visual.x, first_visual.y));
            EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, actual));
            EXPECT_LE(scene.actual_column_count, base->actual_column_count + 9U);
            EXPECT_EQ(scene.center_x, base->center_x);
            EXPECT_EQ(scene.center_y, base->center_y);
            EXPECT_TRUE(client::TerrainLodRuntimeTestAccess::farReady(runtime));
        }
        if (accepted && published_settle && scene.center_x != base->center_x) {
            published_far = true;
            EXPECT_TRUE(client::cpuHasActual(scene, settled_visual.x, settled_visual.y));
            EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, actual));
        }
        if (move_during_settle && scene.visual_center == held_visual && scene.center_x == base->center_x) {
            EXPECT_FALSE(accepted);
            held_first_settle = true;
        }
        if (accepted && scene.visual_center == settled_visual && scene.center_x == base->center_x) {
            if (escape_settle) {
                EXPECT_TRUE(repaired_escape_before_settle)
                    << "latest exact current-first must publish before a replacement conventional disk";
            }
            published_settle = true;
            EXPECT_TRUE(client::cpuHasActual(scene, settled_visual.x, settled_visual.y));
            EXPECT_EQ(client::cpuHasActual(scene, BASE, BASE), move_to_retained);
            if (move_to_retained) {
                EXPECT_FALSE(client::cpuHasActual(scene, first_visual.x, first_visual.y));
            }
            EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, actual));
        }
    };
    client::cpu_hold_upload = true;
    player->x = int32_t(first_visual.x * 16U);
    ASSERT_TRUE(pump([&] { return prepared_first; }, std::chrono::seconds{30}));
    ASSERT_EQ(first_includes_next, !escape_settle)
        << "the fixture distinguishes an admitted neighbor from an escaped current";
    auto const attempts_before_edit = mixed_attempts;
    if (edit_held) {
        auto const edited = actual.authoritativeColumn(BASE * 16U, BASE * 16U);
        ASSERT_TRUE(edited);
        auto const edits = client::cpu_replacements;
        ASSERT_TRUE(actual.applyEditDelta({int32_t(BASE * 16U), int32_t(BASE * 16U), edited->highest_solid_z + 1},
            shared::Block::Stone, 0U, 1U, 1U));
        ASSERT_TRUE(pump([&] { return client::cpu_replacements > edits; }, std::chrono::seconds{10}));
        EXPECT_EQ(mixed_attempts, attempts_before_edit);
        auto const patched = renderer.runtimeInfo().terrain_lod_scene;
        ASSERT_TRUE(patched);
        EXPECT_NE(patched, base);
        EXPECT_TRUE(client::terrainLodSourcesCurrent(*patched, actual));
    }
    client::cpu_hold_upload = false;
    auto expected_attempts = mixed_attempts;
    if (move_during_settle) {
        update();
        ++expected_attempts;
        ASSERT_EQ(mixed_publications, 1U);
        if (!running_settle) {
            client::cpu_hold_upload = true;
            ASSERT_TRUE(pump([&] { return held_first_settle; }, std::chrono::seconds{30}));
        }
        if (edit_before_escape) {
            auto const edited = actual.authoritativeColumn(FRONTIER * 16U, BASE * 16U);
            ASSERT_TRUE(edited);
            auto const replacements = client::cpu_replacements;
            ASSERT_TRUE(actual.applyEditDelta({int32_t(FRONTIER * 16U), int32_t(BASE * 16U),
                edited->highest_solid_z + 1}, shared::Block::Stone, 0U, 1U, 1U));
            // Keep the full Settle upload held while accepting the independent edit patch.
            client::cpu_hold_scene_only = true;
            bool const edit_published = pump(
                [&] { return client::cpu_replacements > replacements; }, std::chrono::seconds{10});
            client::cpu_hold_scene_only = false;
            ASSERT_TRUE(edit_published);
            auto const edited_scene = renderer.runtimeInfo().terrain_lod_scene;
            ASSERT_TRUE(edited_scene);
            EXPECT_FALSE(edited_scene->visual_center);
            EXPECT_TRUE(client::terrainLodSourcesCurrent(*edited_scene, actual));
            auto column = client::captureTerrainLodColumn(actual, FRONTIER, BASE);
            ASSERT_TRUE(column);
            std::array const inputs{std::make_shared<client::TerrainLodColumnInput const>(*column)};
            expected_edit = client::buildTerrainLodColumnPatch(terrain->identity(), inputs);
        }
        escaped = escape_settle;
        client::cpu_hold_upload = false;
    }
    auto const presented_before_move = renderer.runtimeInfo().terrain_lod_scene;
    auto const settle_cancellation = client::TerrainLodRuntimeTestAccess::localCancellationToken(runtime);
    if (!edit_held) {
        player->x = int32_t(settled_visual.x * 16U);
        update();
        if (interruption == CurrentFirstInterruption::MoveHeldToAdmitted) {
            ++expected_attempts;
            ASSERT_EQ(mixed_publications, 1U);
            EXPECT_EQ(renderer.runtimeInfo().terrain_lod_scene.get(), mixed_candidate);
        } else {
            EXPECT_EQ(renderer.runtimeInfo().terrain_lod_scene, presented_before_move);
        }
        EXPECT_EQ(mixed_attempts, expected_attempts);
    }
    if (running_settle) {
        EXPECT_TRUE(settle_cancellation.stop_requested())
            << "a canonical missing current cancels the obsolete in-flight Settle";
    }
    if (repeat_escape) {
        for (uint32_t repeat = 0U; repeat < 2U; ++repeat) {
            ASSERT_TRUE(pump([&] { return repaired_escape_before_settle; }, std::chrono::seconds{30}));
            held_visual = next_visual;
            held_first_settle = false;
            client::cpu_hold_upload = true;
            ASSERT_TRUE(pump([&] { return held_first_settle; }, std::chrono::seconds{30}));
            repaired_escape_before_settle = false;
            next_visual.x += escape_step;
            settled_visual = next_visual;
            player->x = int32_t(next_visual.x * 16U);
            client::cpu_hold_upload = false;
            update();
        }
    }
    ASSERT_TRUE(pump([&] { return published_settle; }, std::chrono::seconds{30}));
    if (escape_settle) {
        EXPECT_TRUE(repaired_escape_before_settle);
        testing::Test::RecordProperty("escaped_current_published_before_settle", repaired_escape_before_settle);
    }
    if (edit_held) {
        EXPECT_TRUE(pump([&] { return published_far; }, std::chrono::seconds{30}))
            << "far handoff must proceed once the conventional disk retires the edited column";
    }
    EXPECT_EQ(mixed_attempts, expected_attempts);
    bool const publish_transient = move_during_settle || interruption == CurrentFirstInterruption::MoveHeldToAdmitted;
    EXPECT_EQ(mixed_publications, publish_transient ? 1U : 0U);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughCurrentFirstPublishesHeldUploadAtAdmittedNeighbor)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::MoveHeldToAdmitted);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughCurrentFirstRejectsHeldUploadAtRetainedOnlyColumn)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::MoveHeldToRetained);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughCurrentFirstRejectsHeldSettleAfterMovement)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::MoveHeldSettle);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughCurrentFirstCannotOverwritePublishedEdit)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::EditHeld);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughEscapedPublishedCurrentFirstRepairsBeforeSettle)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::EscapeHeldSettle);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughEscapedPublishedCurrentFirstRetainsAcceptedEdit)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::EditThenEscapeHeldSettle);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughRepeatedCurrentFirstReplacesTransientCoverage)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::RepeatEscapeHeldSettle);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughEscapedCurrentCancelsRunningSettle)
{
    verifyCurrentFirstRecenter(CurrentFirstInterruption::EscapeRunningSettle);
}

TEST(TerrainLodRuntimeTest, CpuCallthroughRejectedCurrentFirstRetriesCurrentBeforeSettle)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    auto terrain = std::make_shared<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    shared::SparseWorld actual{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(actual.setTerrainIdentity(terrain->identity()));
    constexpr uint32_t BASE = 2'048U, FIRST = 2'097U, NEXT = FIRST + 4U;
    auto publish_neighborhood = [&](uint32_t const x) {
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                EXPECT_TRUE(actual.publishAuthoritativeSurface(terrain->sampleSurface(
                    {int32_t(x) + dx, int32_t(BASE) + dy, 0})));
            }
        }
    };
    publish_neighborhood(BASE);
    client::CpuUnusedShaderAssets assets;
    client::VulkanRenderer renderer{nullptr, assets};
    client::TerrainLodRuntime runtime;
    std::optional<shared::Player> player{shared::Player{.id = 1U, .x = int32_t(BASE * 16U),
        .y = int32_t(BASE * 16U), .z = 764, .ch = 'P'}};
    shared::TerrainSessionIdentity const session{
        .join_epoch = 7U, .terrain = terrain->identity(), .configuration = configuration};
    auto update = [&] {
        runtime.update(terrain, &actual, player, renderer,
            std::chrono::steady_clock::now() + std::chrono::milliseconds{20}, session, 1U);
    };
    auto pump = [&](auto predicate) {
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        while (!predicate() && std::chrono::steady_clock::now() < until) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return predicate();
    };
    client::cpu_hold_upload = false;
    ASSERT_TRUE(pump([&] { return runtime.active(); }));
    auto const base = renderer.runtimeInfo().terrain_lod_scene;
    ASSERT_TRUE(base);
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::holdFarReadyFromPresented(runtime));
    publish_neighborhood(FIRST);
    publish_neighborhood(NEXT);
    bool first_held = false, moved = false, retry_observed = false, retry_current_published = false;
    client::cpu_scene_observer = [&](client::TerrainLodScene const& scene, bool const accepted) {
        if (!moved) {
            EXPECT_FALSE(accepted);
            EXPECT_FALSE(scene.visual_center.has_value());
            EXPECT_TRUE(client::cpuHasActual(scene, FIRST, BASE));
            EXPECT_FALSE(client::cpuHasActual(scene, NEXT, BASE));
            first_held = true;
            return;
        }
        if (!retry_observed) {
            retry_observed = true;
            EXPECT_FALSE(scene.visual_center.has_value())
                << "an unpublished current-first attempt cannot require a full settle before retry";
            retry_current_published = accepted && !scene.visual_center
                && client::cpuHasActual(scene, NEXT, BASE);
            EXPECT_TRUE(retry_current_published);
            EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, actual));
        }
    };
    client::cpu_hold_upload = true;
    player->x = int32_t(FIRST * 16U);
    ASSERT_TRUE(pump([&] { return first_held; }));
    EXPECT_EQ(renderer.runtimeInfo().terrain_lod_scene, base);
    moved = true;
    player->x = int32_t(NEXT * 16U);
    client::cpu_hold_upload = false;
    update();
    EXPECT_EQ(renderer.runtimeInfo().terrain_lod_scene, base)
        << "moving beyond the admitted footprint must reject the held old scene";
    ASSERT_TRUE(pump([&] { return retry_observed; }));
    EXPECT_TRUE(retry_current_published);
    RecordProperty("latest_current_published_before_settle", retry_current_published);
    client::cpu_scene_observer = {};
}

TEST(TerrainLodRuntimeTest, CpuCallthroughCurrentFirstPublishesWithStaleOffscreenAuthorityRetired)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    auto terrain = std::make_shared<shared::TerrainWorld const>(generation::prepareAtlas(configuration));
    shared::SparseWorld actual{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(actual.setTerrainIdentity(terrain->identity()));
    constexpr uint32_t BASE = 2'048U, CURRENT = BASE + shared::FULL_DETAIL_RADIUS_CHUNKS + 9U;
    auto publish_neighborhood = [&](uint32_t const x) {
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                ASSERT_TRUE(actual.publishAuthoritativeSurface(terrain->sampleSurface(
                    {int32_t(x) + dx, int32_t(BASE) + dy, 0})));
            }
        }
    };
    publish_neighborhood(BASE);
    client::CpuUnusedShaderAssets assets;
    client::VulkanRenderer renderer{nullptr, assets};
    client::TerrainLodRuntime runtime;
    std::optional<shared::Player> player{shared::Player{.id = 1U, .x = int32_t(BASE * 16U),
        .y = int32_t(BASE * 16U), .z = 764, .ch = 'P'}};
    shared::TerrainSessionIdentity const session{
        .join_epoch = 7U, .terrain = terrain->identity(), .configuration = configuration};
    auto update = [&] {
        runtime.update(terrain, &actual, player,
            renderer, std::chrono::steady_clock::now() + std::chrono::milliseconds{20}, session, 1U);
    };
    auto pump = [&](auto predicate) {
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        while (!predicate() && std::chrono::steady_clock::now() < until) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return predicate();
    };
    client::cpu_hold_upload = false;
    ASSERT_TRUE(pump([&] { return runtime.active(); }));
    auto const base = renderer.runtimeInfo().terrain_lod_scene;
    ASSERT_TRUE(base);
    ASSERT_TRUE(client::cpuHasActual(*base, BASE, BASE));
    ASSERT_TRUE(client::TerrainLodRuntimeTestAccess::holdFarReadyFromPresented(runtime));

    ASSERT_TRUE(actual.removeAuthoritativeSurface({int32_t(BASE), int32_t(BASE), 0}));
    EXPECT_FALSE(client::terrainLodSourcesCurrent(*base, actual));
    publish_neighborhood(CURRENT);
    bool published = false;
    client::cpu_scene_observer = [&](client::TerrainLodScene const& scene, bool const accepted) {
        if (scene.visual_center) {
            return;
        }
        EXPECT_TRUE(accepted);
        EXPECT_TRUE(client::cpuHasActual(scene, CURRENT, BASE));
        EXPECT_TRUE(client::cpuHasActual(scene, CURRENT - 1U, BASE));
        EXPECT_TRUE(client::cpuHasActual(scene, CURRENT + 1U, BASE));
        EXPECT_TRUE(client::terrainLodSourcesCurrent(scene, actual));
        published = accepted && client::cpuHasActual(scene, CURRENT, BASE)
            && client::terrainLodSourcesCurrent(scene, actual);
    };
    player->x = int32_t(CURRENT * 16U);
    bool const completed = pump([&] { return published; });
    client::cpu_scene_observer = {};
    ASSERT_TRUE(completed)
        << "a stale authority capture more than the 40-column visual radius away must not block the current footprint";
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*renderer.runtimeInfo().terrain_lod_scene, actual));
    EXPECT_FALSE(client::cpuHasActual(*renderer.runtimeInfo().terrain_lod_scene, BASE, BASE));
}

#endif
