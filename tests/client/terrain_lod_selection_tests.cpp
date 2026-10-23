#include <client/TerrainLodSelection.hpp>
#include <shared/world/TerrainLod.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace {

using client::TerrainLodReadiness;
using client::TerrainLodRegion;
using client::TerrainLodSelection;
using shared::TerrainLodLevel;

TEST(TerrainLodSelectionTest, RetainsParentUntilEveryChildHasUploaded)
{
    static constexpr uint64_t GENERATION = 7U;
    static constexpr TerrainLodRegion ROOT{};
    TerrainLodSelection selection{ GENERATION };
    ASSERT_TRUE(selection.markReady(ROOT, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(ROOT, GENERATION));
    auto const children = client::terrainLodChildren(ROOT);
    for (uint32_t index = 0U; index < 15U; ++index) {
        ASSERT_TRUE(selection.markReady(children[index], GENERATION, TerrainLodReadiness::Uploaded));
        EXPECT_FALSE(selection.refine(ROOT, GENERATION));
        EXPECT_TRUE(selection.active(ROOT));
        EXPECT_EQ(selection.leaves().size(), 1U);
    }
    ASSERT_TRUE(selection.markReady(children.back(), GENERATION, TerrainLodReadiness::Mesh));
    EXPECT_FALSE(selection.refine(ROOT, GENERATION));
    ASSERT_TRUE(selection.markReady(children.back(), GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.refine(ROOT, GENERATION));
    EXPECT_FALSE(selection.active(ROOT));
    EXPECT_EQ(selection.leaves().size(), 16U);
    for (TerrainLodRegion const child : children) {
        EXPECT_TRUE(selection.active(child));
    }
}

TEST(TerrainLodSelectionTest, StaleGenerationCannotActivateOrRefineCurrentWorld)
{
    static constexpr uint64_t OLD_GENERATION = 4U;
    static constexpr uint64_t CURRENT_GENERATION = 5U;
    static constexpr TerrainLodRegion ROOT{};
    TerrainLodSelection selection{ OLD_GENERATION };
    ASSERT_TRUE(selection.markReady(ROOT, OLD_GENERATION, TerrainLodReadiness::Uploaded));
    selection.reset(CURRENT_GENERATION);
    EXPECT_FALSE(selection.uploaded(ROOT));
    EXPECT_TRUE(selection.leaves().empty());
    EXPECT_FALSE(selection.markReady(ROOT, OLD_GENERATION, TerrainLodReadiness::Uploaded));
    EXPECT_FALSE(selection.activateRoot(ROOT, OLD_GENERATION));
    EXPECT_FALSE(selection.activateRoot(ROOT, CURRENT_GENERATION));
    ASSERT_TRUE(selection.markReady(ROOT, CURRENT_GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(ROOT, CURRENT_GENERATION));
    for (TerrainLodRegion const child : client::terrainLodChildren(ROOT)) {
        ASSERT_TRUE(selection.markReady(child, CURRENT_GENERATION, TerrainLodReadiness::Uploaded));
    }
    EXPECT_FALSE(selection.refine(ROOT, OLD_GENERATION));
    EXPECT_TRUE(selection.active(ROOT));
}

TEST(TerrainLodSelectionTest, CoarseningAtomicallyReplacesAllDescendantLevels)
{
    static constexpr uint64_t GENERATION = 8U;
    static constexpr TerrainLodRegion ROOT{};
    TerrainLodSelection selection{ GENERATION };
    ASSERT_TRUE(selection.markReady(ROOT, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(ROOT, GENERATION));
    auto const children = client::terrainLodChildren(ROOT);
    for (TerrainLodRegion const child : children) {
        ASSERT_TRUE(selection.markReady(child, GENERATION, TerrainLodReadiness::Uploaded));
    }
    ASSERT_TRUE(selection.refine(ROOT, GENERATION));
    for (TerrainLodRegion const grandchild : client::terrainLodChildren(children.front())) {
        ASSERT_TRUE(selection.markReady(grandchild, GENERATION, TerrainLodReadiness::Uploaded));
    }
    ASSERT_TRUE(selection.refine(children.front(), GENERATION));
    EXPECT_EQ(selection.leaves().size(), 31U);
    ASSERT_TRUE(selection.coarsen(ROOT, GENERATION));
    EXPECT_EQ(selection.leaves().size(), 1U);
    EXPECT_TRUE(selection.active(ROOT));
    EXPECT_FALSE(selection.active(children.front()));
}

TEST(TerrainLodSelectionTest, PeriodicKeysResolveToTheSameRegionWithoutDuplicateCoverage)
{
    static constexpr uint64_t GENERATION = 9U;
    static constexpr TerrainLodRegion ROOT{};
    static constexpr TerrainLodRegion WRAPPED_ROOT{ .level = TerrainLodLevel::L0, .x = 256U, .y = 512U };
    TerrainLodSelection selection{ GENERATION };
    ASSERT_TRUE(selection.markReady(WRAPPED_ROOT, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(ROOT, GENERATION));
    EXPECT_TRUE(selection.active(ROOT));
    EXPECT_TRUE(selection.active(WRAPPED_ROOT));
    EXPECT_FALSE(selection.activateRoot(ROOT, GENERATION));
    EXPECT_EQ(selection.leaves().size(), 1U);
    EXPECT_EQ(client::terrainLodRegionExtent(TerrainLodLevel::L0), 256U);
    EXPECT_EQ(client::terrainLodRegionExtent(TerrainLodLevel::L1), 64U);
    EXPECT_EQ(client::terrainLodRegionExtent(TerrainLodLevel::L2), 16U);
}

TEST(TerrainLodSelectionTest, ReadyEvidenceNeverRegressesAndActualChunksAreTerminal)
{
    static constexpr uint64_t GENERATION = 10U;
    static constexpr TerrainLodRegion CHUNK_REGION{ .level = TerrainLodLevel::L3, .x = 1U, .y = 2U };
    TerrainLodSelection selection{ GENERATION };
    ASSERT_TRUE(selection.markReady(CHUNK_REGION, GENERATION, TerrainLodReadiness::Uploaded));
    EXPECT_FALSE(selection.markReady(CHUNK_REGION, GENERATION, TerrainLodReadiness::Source));
    EXPECT_TRUE(selection.uploaded(CHUNK_REGION));
    EXPECT_FALSE(selection.refine(CHUNK_REGION, GENERATION));
    EXPECT_THROW(static_cast<void>(client::terrainLodChildren(CHUNK_REGION)), std::invalid_argument);
}

TEST(TerrainLodSelectionTest, ActualChunkReplacesFourComplete512AtlasCellsTogether)
{
    static constexpr uint64_t GENERATION = 11U;
    static constexpr TerrainLodRegion ROOT{};
    static constexpr TerrainLodRegion CHUNK{ .level = TerrainLodLevel::L3, .x = 0U, .y = 0U };
    TerrainLodSelection selection{ GENERATION, 128U };
    ASSERT_TRUE(selection.markReady(ROOT, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(ROOT, GENERATION));
    auto const children = client::terrainLodChildren(ROOT, 128U);
    for (TerrainLodRegion const child : children) {
        ASSERT_TRUE(selection.markReady(child, GENERATION, TerrainLodReadiness::Uploaded));
    }
    ASSERT_TRUE(selection.refine(ROOT, GENERATION));
    auto const cells = client::terrainLodChildren(children.front(), 128U);
    for (TerrainLodRegion const cell : cells) {
        ASSERT_TRUE(selection.markReady(cell, GENERATION, TerrainLodReadiness::Uploaded));
    }
    ASSERT_TRUE(selection.refine(children.front(), GENERATION));
    EXPECT_FALSE(selection.activateActualChunk(CHUNK, GENERATION));
    EXPECT_EQ(selection.leaves().size(), 31U);
    ASSERT_TRUE(selection.markReady(CHUNK, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateActualChunk(CHUNK, GENERATION));
    EXPECT_EQ(selection.leaves().size(), 28U);
    EXPECT_TRUE(selection.active(CHUNK));
    for (TerrainLodRegion const cell : cells) {
        EXPECT_EQ(selection.active(cell), cell.x >= 2U || cell.y >= 2U);
    }
}

TEST(TerrainLodSelectionTest, IndependentRootsPublishWithoutArrivalOrderConnectivity)
{
    static constexpr uint64_t GENERATION = 12U;
    static constexpr TerrainLodRegion FIRST{ .x = 1U, .y = 1U };
    static constexpr TerrainLodRegion FAR{ .x = 128U, .y = 128U };
    TerrainLodSelection selection{ GENERATION };
    ASSERT_TRUE(selection.markReady(FAR, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(FAR, GENERATION));
    ASSERT_TRUE(selection.markReady(FIRST, GENERATION, TerrainLodReadiness::Uploaded));
    ASSERT_TRUE(selection.activateRoot(FIRST, GENERATION));
    EXPECT_EQ(selection.leaves().size(), 2U);
    EXPECT_TRUE(selection.active(FIRST));
    EXPECT_TRUE(selection.active(FAR));
}

} // namespace
