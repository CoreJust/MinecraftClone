#include <client/TerrainLodRuntime.hpp>

#include <gtest/gtest.h>
#include <testsupport/TerrainFixtures.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <string>
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

TEST(TerrainLodRuntimeTest, IncrementalCrossDependenciesMatchFullValidationForRemovalAndReadiness)
{
    IncrementalSources fixture;
    fixture.cross(0U, 0U);
    fixture.cross(10U, 10U);
    fixture.capture(0U, 0U);
    fixture.capture(10U, 10U);
    fixture.compare();
    EXPECT_TRUE(fixture.world.removeAuthoritativeSurface({4'095, 0, 0}));
    fixture.compare();
    ASSERT_EQ(fixture.selection.columns.size(), 1U);
    ASSERT_EQ(fixture.columns.size(), 1U);
    fixture.publish(4'095U, 0U);
    fixture.compare();
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
    plan.updateInterest(63U, 32U);
    for (auto const column : COLUMNS) {
        plan.remember(column);
    }
    plan.presented(COLUMNS);
    plan.updateInterest(64U, 32U);
    EXPECT_EQ(plan.ticket().columns.size(), COLUMNS.size());
    EXPECT_FALSE(plan.canPublish(plan.ticket(), std::span{COLUMNS}.subspan(1U, 1U)));
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
    plan.updateInterest(640U, 0U);
    EXPECT_EQ(plan.ticket().columns.size(), 1U);
    plan.updateInterest(672U, 0U);
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
} // namespace client
