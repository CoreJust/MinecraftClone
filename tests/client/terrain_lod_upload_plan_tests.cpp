#include <client/TerrainLodPresentation.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/TerrainLodUploadPlan.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

TEST(TerrainLodUploadPlanTest, ReusesExactGroupsAcrossReorderAndWaitsForChangedGroup)
{
    client::TerrainLodScene active;
    active.faces.resize(3U);
    active.faces[1].x = 16;
    active.faces[2].x = 32;
    active.groups = {
        { .range = { 0U, 1U }, .origin_x = 0U, .extent = 16U },
        { .range = { 1U, 1U }, .origin_x = 16U, .extent = 16U },
        { .range = { 2U, 1U }, .origin_x = 32U, .extent = 16U },
    };
    auto const unchanged = client::detail::terrainLodReusableGroups(&active, active);
    ASSERT_EQ(unchanged.size(), 3U);
    EXPECT_EQ(unchanged[0], 0U);
    EXPECT_EQ(unchanged[1], 1U);
    EXPECT_EQ(unchanged[2], 2U);
    auto candidate = active;
    std::swap(candidate.groups[0], candidate.groups[2]);
    candidate.faces[1].z = 1;
    auto const reusable = client::detail::terrainLodReusableGroups(&active, candidate);
    ASSERT_EQ(reusable.size(), 3U);
    EXPECT_EQ(reusable[0], 2U);
    EXPECT_FALSE(reusable[1].has_value());
    EXPECT_EQ(reusable[2], 0U);
    uint32_t uploaded_faces = 0U;
    for (uint32_t index = 0U; index < reusable.size(); ++index) {
        if (!reusable[index]) {
            uploaded_faces += candidate.groups[index].range.instance_count;
        }
    }
    EXPECT_EQ(uploaded_faces, 1U);
    EXPECT_LT(uploaded_faces, candidate.faces.size());
    std::array<uint32_t, 3> progress{ 1U, 0U, 1U };
    EXPECT_FALSE(client::detail::terrainLodUploadsComplete(candidate, progress));
    progress[1] = 1U;
    EXPECT_TRUE(client::detail::terrainLodUploadsComplete(candidate, progress));
    candidate.groups[2].extent = 32U;
    EXPECT_FALSE(client::detail::terrainLodReusableGroups(&active, candidate)[2].has_value());
}

TEST(TerrainLodUploadPlanTest, RejectsDuplicateKeysAndInvalidFaceRanges)
{
    client::TerrainLodScene scene;
    scene.faces.resize(2U);
    scene.groups = {
        { .range = { 0U, 1U }, .extent = 16U },
        { .range = { 1U, 1U }, .extent = 16U },
    };
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    scene.groups[1].origin_x = 16U;
    scene.groups[1].range.first_instance = 2U;
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    scene.groups[1].range = { 1U, 0U };
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    scene.faces.resize(1U);
    EXPECT_NO_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)));
}

TEST(TerrainLodUploadPlanTest, RejectsOverlappingRangesEvenWhenTheirTotalMatches)
{
    client::TerrainLodScene scene;
    scene.faces.resize(2U);
    scene.groups = {
        { .range = { 0U, 1U }, .extent = 16U },
        { .range = { 0U, 1U }, .origin_x = 16U, .extent = 16U },
    };
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    EXPECT_FALSE(client::detail::terrainLodUploadsComplete(scene, std::array<uint32_t, 1>{ 1U }));
}

TEST(TerrainLodUploadPlanTest, AsyncPlannerKeepsOnlyLatestGenerationAfterInvalidation)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 2 };
    auto first = std::make_shared<client::TerrainLodScene>();
    first->faces.resize(1U);
    first->groups = { { .range = { 0U, 1U }, .extent = 16U } };
    auto second = std::make_shared<client::TerrainLodScene>(*first);
    second->faces[0].z = 7;
    client::detail::TerrainLodUploadPlanner planner;
    EXPECT_FALSE(planner.poll(nullptr, {}, first).has_value());
    EXPECT_TRUE(planner.pending());
    planner.invalidate();
    EXPECT_FALSE(planner.pending());
    std::optional<client::detail::TerrainLodUploadPlan> result;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = planner.poll(nullptr, {}, second);
        std::this_thread::yield();
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->scene, second);
    EXPECT_EQ(result->uploaded, (std::vector<uint32_t>{ 0U }));
    EXPECT_EQ(result->ranges.size(), 1U);
}

TEST(TerrainLodUploadPlanTest, AsyncPlannerSnapshotsActiveRangesWithoutOwningRenderer)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 2 };
    auto active = std::make_shared<client::TerrainLodScene>();
    active->faces.resize(1U);
    active->groups = { { .range = { 0U, 1U }, .extent = 16U } };
    auto candidate = std::make_shared<client::TerrainLodScene>(*active);
    std::vector<client::StoneIndirectDraws::Range> ranges{ { 100U, 1U } };
    client::detail::TerrainLodUploadPlanner planner;
    EXPECT_FALSE(planner.poll(active, ranges, candidate).has_value());
    ranges[0].first_instance = 200U;
    active.reset();
    std::optional<client::detail::TerrainLodUploadPlan> result;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = planner.poll(nullptr, ranges, candidate);
        std::this_thread::yield();
    }
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->ranges.size(), 1U);
    EXPECT_EQ(result->ranges[0].first_instance, 100U);
    EXPECT_EQ(result->uploaded, (std::vector<uint32_t>{ 1U }));
}

TEST(TerrainLodUploadPlanTest, ExactReplacementPublishesCurrentDependencyProvenance)
{
    client::TerrainLodScene active;
    active.faces.resize(1U);
    active.groups = {{.range = {0U, 1U}, .extent = 16U}};
    client::TerrainLodChunkSource const old_chunk{{0, 0, 0}, {10U, 20U}, 0U};
    client::TerrainLodChunkSource const updated_chunk{{0, 0, 0}, {11U, 21U}, 1U};
    client::TerrainLodChunkSource const introduced_chunk{{2, 0, 9}, {12U, 22U}, 3U};
    client::TerrainLodChunkSource const unrelated_chunk{{9, 0, 0}, {13U, 23U}, 4U};
    client::TerrainLodAuthoritySource const old_authority{
        {0, 0, 0}, {.revision = 1U, .seed = 7U, .configuration_identity = "before"}, 10U, 20U};
    client::TerrainLodAuthoritySource const updated_authority{
        {0, 0, 0}, {.revision = 2U, .seed = 8U, .configuration_identity = "after"}, 11U, 21U};
    client::TerrainLodAuthoritySource const introduced_authority{
        {2, 0, 0}, {.revision = 3U, .seed = 9U, .configuration_identity = "introduced"}, 12U, 22U};
    client::TerrainLodAuthoritySource const unrelated_authority{
        {9, 0, 0}, {.revision = 4U, .seed = 10U, .configuration_identity = "retained"}, 13U, 23U};
    // Shared boundary dependencies can occur repeatedly in captured columns.
    active.chunk_sources = {unrelated_chunk, old_chunk, old_chunk};
    active.authority_sources = {unrelated_authority, old_authority, old_authority};
    client::TerrainLodScene patch;
    patch.faces.resize(1U);
    patch.faces.front().z = 1;
    patch.groups = active.groups;
    patch.chunk_sources = {introduced_chunk, updated_chunk, updated_chunk};
    patch.authority_sources = {introduced_authority, updated_authority, updated_authority};
    std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U}};
    auto const plan = client::detail::terrainLodReplacementPlan(active, ranges, patch);
    ASSERT_NE(plan.scene, nullptr);
    EXPECT_EQ(plan.scene->chunk_sources,
        (std::vector<client::TerrainLodChunkSource>{updated_chunk, introduced_chunk, unrelated_chunk}));
    EXPECT_EQ(plan.scene->authority_sources,
        (std::vector<client::TerrainLodAuthoritySource>{updated_authority, introduced_authority, unrelated_authority}));
    EXPECT_EQ(active.chunk_sources, (std::vector<client::TerrainLodChunkSource>{unrelated_chunk, old_chunk, old_chunk}));
    EXPECT_EQ(plan.scene->faces, patch.faces);
}
