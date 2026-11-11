#include <client/TerrainLodPresentation.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/TerrainLodDrawIndex.hpp>
#include <client/render/TerrainLodUploadPlan.hpp>

#include <gtest/gtest.h>

#include <algorithm>
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

namespace {

void bindSceneFaces(client::TerrainLodScene& scene, std::vector<client::StoneFaceInstance> faces)
{
    scene.logical_face_count = faces.size();
    for (client::TerrainLodDrawGroup& group : scene.groups) {
        if (group.range.instance_count == 0U) {
            continue;
        }
        if (group.range.first_instance > faces.size()
            || group.range.instance_count > faces.size() - group.range.first_instance) {
            group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(
                group.range.instance_count);
            group.face_storage_first_instance = 0U;
            continue;
        }
        auto const first = faces.begin() + group.range.first_instance;
        group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(
            first, first + group.range.instance_count);
        group.face_storage_first_instance = 0U;
    }
}

std::vector<client::StoneFaceInstance> sceneFaces(client::TerrainLodScene const& scene)
{
    std::vector<client::StoneFaceInstance> faces(client::terrainLodSceneFaceCount(scene));
    for (client::TerrainLodDrawGroup const& group : scene.groups) {
        auto const group_faces = client::terrainLodGroupFaces(group);
        std::copy(group_faces.begin(), group_faces.end(), faces.begin() + group.range.first_instance);
    }
    return faces;
}

std::shared_ptr<client::TerrainLodScene> makeUploadPlanBaseScene(int32_t first_face_x, int32_t second_face_x)
{
    auto scene = std::make_shared<client::TerrainLodScene>();
    scene->identity.seed = 42U;
    scene->session_join_epoch = 3U;
    scene->coverage_epoch = 4U;
    scene->groups = {
        { .range = { 0U, 1U }, .extent = 16U },
        { .range = { 1U, 1U }, .origin_x = 16U, .extent = 16U },
    };
    std::vector<client::StoneFaceInstance> faces(2U);
    faces[0].x = first_face_x;
    faces[1].x = second_face_x;
    bindSceneFaces(*scene, std::move(faces));
    return scene;
}

std::shared_ptr<client::TerrainLodScene> makeUploadPlanPatchScene(int32_t face_x)
{
    auto scene = std::make_shared<client::TerrainLodScene>();
    scene->identity.seed = 42U;
    scene->session_join_epoch = 3U;
    scene->coverage_epoch = 4U;
    scene->groups = { { .range = { 0U, 1U }, .extent = 16U } };
    std::vector<client::StoneFaceInstance> faces(1U);
    faces[0].x = face_x;
    bindSceneFaces(*scene, std::move(faces));
    return scene;
}


} // namespace

TEST(TerrainLodUploadPlanTest, ReusesExactGroupsAcrossReorderAndWaitsForChangedGroup)
{
    client::TerrainLodScene active;
    std::vector<client::StoneFaceInstance> active_faces(3U);
    active_faces[1].x = 16;
    active_faces[2].x = 32;
    active.groups = {
        { .range = { 0U, 1U }, .origin_x = 0U, .extent = 16U },
        { .range = { 1U, 1U }, .origin_x = 16U, .extent = 16U },
        { .range = { 2U, 1U }, .origin_x = 32U, .extent = 16U },
    };
    bindSceneFaces(active, std::move(active_faces));
    EXPECT_EQ(client::terrainLodSceneFaceCount(active), 3U);
    auto const unchanged = client::detail::terrainLodReusableGroups(&active, active);
    ASSERT_EQ(unchanged.size(), 3U);
    EXPECT_EQ(unchanged[0], 0U);
    EXPECT_EQ(unchanged[1], 1U);
    EXPECT_EQ(unchanged[2], 2U);
    auto candidate = active;
    std::swap(candidate.groups[0], candidate.groups[2]);
    std::vector<client::StoneFaceInstance> candidate_faces = sceneFaces(candidate);
    candidate_faces[1].z = 1;
    bindSceneFaces(candidate, std::move(candidate_faces));
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
    EXPECT_LT(uploaded_faces, client::terrainLodSceneFaceCount(candidate));
    std::array<uint32_t, 3> progress{ 1U, 0U, 1U };
    EXPECT_FALSE(client::detail::terrainLodUploadsComplete(candidate, progress));
    progress[1] = 1U;
    EXPECT_TRUE(client::detail::terrainLodUploadsComplete(candidate, progress));
    candidate.groups[2].extent = 32U;
    EXPECT_FALSE(client::detail::terrainLodReusableGroups(&active, candidate)[2].has_value());
}

TEST(TerrainLodUploadPlanTest, UploadCursorResumesIncompleteWorkAndResetsForNewPlan)
{
    client::TerrainLodScene scene;
    static constexpr size_t PREFIX_SIZE = 65'536U;
    scene.groups.resize(PREFIX_SIZE + 3U);
    for (auto& group : scene.groups) {
        group.range.instance_count = 1U;
    }
    scene.groups[PREFIX_SIZE].range.instance_count = 3U;
    scene.groups[PREFIX_SIZE + 1U].range.instance_count = 0U;
    std::vector<uint32_t> uploaded(PREFIX_SIZE + 3U, 1U);
    uploaded[PREFIX_SIZE] = 1U;
    uploaded[PREFIX_SIZE + 1U] = 0U;
    uploaded[PREFIX_SIZE + 2U] = 0U;
    size_t cursor = 0U;
    client::detail::advanceTerrainLodUploadCursor(scene, uploaded, cursor);
    EXPECT_EQ(cursor, PREFIX_SIZE);
    // A retry with no upload keeps the partial group without revisiting the prefix.
    uploaded[0] = 0U;
    client::detail::advanceTerrainLodUploadCursor(scene, uploaded, cursor);
    EXPECT_EQ(cursor, PREFIX_SIZE);
    uploaded[PREFIX_SIZE] = 3U;
    client::detail::advanceTerrainLodUploadCursor(scene, uploaded, cursor);
    EXPECT_EQ(cursor, PREFIX_SIZE + 2U);
    uploaded[PREFIX_SIZE + 2U] = 1U;
    client::detail::advanceTerrainLodUploadCursor(scene, uploaded, cursor);
    EXPECT_EQ(cursor, scene.groups.size());
    // Cancellation/new plan resets the cursor, so the first group is selected again.
    cursor = 0U;
    client::detail::advanceTerrainLodUploadCursor(scene, uploaded, cursor);
    EXPECT_EQ(cursor, 0U);
    EXPECT_THROW(client::detail::advanceTerrainLodUploadCursor(scene, std::span<uint32_t const>{}, cursor),
        std::invalid_argument);
}

TEST(TerrainLodUploadPlanTest, RejectsDuplicateKeysAndInvalidFaceRanges)
{
    client::TerrainLodScene scene;
    std::vector<client::StoneFaceInstance> faces(2U);
    scene.groups = {
        { .range = { 0U, 1U }, .extent = 16U },
        { .range = { 1U, 1U }, .extent = 16U },
    };
    bindSceneFaces(scene, faces);
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    scene.groups[1].origin_x = 16U;
    scene.groups[1].range.first_instance = 2U;
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    scene.groups[1].range = { 1U, 0U };
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    scene.logical_face_count = 1U;
    EXPECT_NO_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)));
}

TEST(TerrainLodUploadPlanTest, RejectsOverlappingRangesEvenWhenTheirTotalMatches)
{
    client::TerrainLodScene scene;
    std::vector<client::StoneFaceInstance> faces(2U);
    scene.groups = {
        { .range = { 0U, 1U }, .extent = 16U },
        { .range = { 0U, 1U }, .origin_x = 16U, .extent = 16U },
    };
    bindSceneFaces(scene, std::move(faces));
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
    EXPECT_FALSE(client::detail::terrainLodUploadsComplete(scene, std::array<uint32_t, 1>{ 1U }));
}

TEST(TerrainLodUploadPlanTest, RejectsGroupSliceOutsideItsImmutableOwner)
{
    client::TerrainLodScene scene;
    scene.logical_face_count = 2U;
    client::TerrainLodDrawGroup group{.range = {0U, 2U}, .extent = 16U};
    group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(1U);
    scene.groups.push_back(std::move(group));
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReusableGroups(nullptr, scene)), std::invalid_argument);
}

TEST(TerrainLodUploadPlanTest, AsyncPlannerKeepsOnlyLatestGenerationAfterInvalidation)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 2 };
    auto first = std::make_shared<client::TerrainLodScene>();
    std::vector<client::StoneFaceInstance> faces(1U);
    first->groups = { { .range = { 0U, 1U }, .extent = 16U } };
    bindSceneFaces(*first, std::move(faces));
    auto second = std::make_shared<client::TerrainLodScene>(*first);
    auto second_faces = sceneFaces(*second);
    second_faces[0].z = 7;
    bindSceneFaces(*second, std::move(second_faces));
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
    std::vector<client::StoneFaceInstance> faces(1U);
    active->groups = { { .range = { 0U, 1U }, .extent = 16U } };
    bindSceneFaces(*active, std::move(faces));
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
    active.groups = {{.range = {0U, 1U}, .extent = 16U}};
    bindSceneFaces(active, std::vector<client::StoneFaceInstance>(1U));
    auto active_draw_index = std::make_shared<client::TerrainLodDrawIndex>();
    active_draw_index->reset(active.groups);
    active.draw_index = std::move(active_draw_index);
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
    client::TerrainLodEditSource const old_edit{{0, 0, 10}, 1U};
    client::TerrainLodEditSource const updated_edit{{0, 0, 10}, 2U};
    client::TerrainLodEditSource const introduced_edit{{2, 0, 9}, 3U};
    client::TerrainLodEditSource const unrelated_edit{{9, 0, 0}, 4U};
    // Shared boundary dependencies can occur repeatedly in captured columns.
    active.chunk_sources = {unrelated_chunk, old_chunk, old_chunk};
    active.authority_sources = {unrelated_authority, old_authority, old_authority};
    active.edit_sources = {unrelated_edit, old_edit, old_edit};
    client::TerrainLodScene patch;
    patch.groups = active.groups;
    patch.groups.front().maximum_z = 2;
    std::vector<client::StoneFaceInstance> patch_faces(1U);
    patch_faces.front().z = 1;
    bindSceneFaces(patch, std::move(patch_faces));
    patch.chunk_sources = {introduced_chunk, updated_chunk, updated_chunk};
    patch.authority_sources = {introduced_authority, updated_authority, updated_authority};
    patch.edit_sources = {introduced_edit, updated_edit, updated_edit};
    std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U}};
    auto const plan = client::detail::terrainLodReplacementPlan(active, ranges, patch);
    ASSERT_NE(plan.scene, nullptr);
    ASSERT_NE(plan.scene->draw_index, nullptr);
    EXPECT_NE(plan.scene->draw_index, active.draw_index);
    EXPECT_EQ(plan.scene->chunk_sources,
        (std::vector<client::TerrainLodChunkSource>{updated_chunk, introduced_chunk, unrelated_chunk}));
    EXPECT_EQ(plan.scene->authority_sources,
        (std::vector<client::TerrainLodAuthoritySource>{updated_authority, introduced_authority, unrelated_authority}));
    EXPECT_EQ(plan.scene->edit_sources,
        (std::vector<client::TerrainLodEditSource>{updated_edit, introduced_edit, unrelated_edit}));
    EXPECT_EQ(active.chunk_sources, (std::vector<client::TerrainLodChunkSource>{unrelated_chunk, old_chunk, old_chunk}));
    EXPECT_EQ(active.edit_sources, (std::vector<client::TerrainLodEditSource>{unrelated_edit, old_edit, old_edit}));
    EXPECT_EQ(sceneFaces(*plan.scene), sceneFaces(patch));
    EXPECT_EQ(plan.scene->logical_face_count, 1U);
    EXPECT_EQ(plan.scene->groups[0].face_storage, patch.groups[0].face_storage);
    EXPECT_NE(active.groups[0].face_storage, patch.groups[0].face_storage);
}

TEST(TerrainLodUploadPlanTest, ReplacementRejectsConflictingEditRevisionsWithinPatch)
{
    auto const active = makeUploadPlanBaseScene(10, 20);
    auto patch = makeUploadPlanPatchScene(30);
    patch->edit_sources = {{{0, 0, 10}, 1U}, {{0, 0, 10}, 2U}};
    std::array const ranges{
        client::StoneIndirectDraws::Range{100U, 1U}, client::StoneIndirectDraws::Range{200U, 1U},
    };

    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReplacementPlan(*active, ranges, *patch)),
        std::invalid_argument);
}

TEST(TerrainLodUploadPlanTest, ReplacementRejectsEditRevisionRollback)
{
    auto active = makeUploadPlanBaseScene(10, 20);
    auto patch = makeUploadPlanPatchScene(30);
    active->edit_sources = {{{0, 0, 10}, 2U}};
    patch->edit_sources = {{{0, 0, 10}, 1U}};
    std::array const ranges{
        client::StoneIndirectDraws::Range{100U, 1U}, client::StoneIndirectDraws::Range{200U, 1U},
    };

    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReplacementPlan(*active, ranges, *patch)),
        std::invalid_argument);
}

TEST(TerrainLodUploadPlanTest, SameBoundsReplacementRetainsIndexAndExactSceneContracts)
{
    client::TerrainLodScene active;
    active.identity.seed = 42U;
    active.session_join_epoch = 3U;
    active.coverage_epoch = 4U;
    active.center_x = 32U;
    active.center_y = 48U;
    active.root_extent = 512U;
    active.block_sea_level = 384U;
    active.actual_column_count = 2U;
    active.leaves = {{.level = shared::TerrainLodLevel::L3, .x = 0U, .y = 0U}};
    active.groups = {
        {.range = {0U, 1U}, .extent = 16U},
        {.range = {1U, 1U}, .origin_x = 16U, .extent = 16U},
    };
    std::vector<client::StoneFaceInstance> active_faces(2U);
    active_faces[1].x = 16;
    bindSceneFaces(active, std::move(active_faces));
    auto index = std::make_shared<client::TerrainLodDrawIndex>();
    index->reset(active.groups);
    active.draw_index = index;
    auto patch = active;
    patch.groups.resize(1U);
    patch.groups[0].range.instance_count = 2U;
    std::vector<client::StoneFaceInstance> patch_faces(2U);
    patch_faces[0].material_id = 2U;
    patch_faces[1].material_id = 3U;
    bindSceneFaces(patch, std::move(patch_faces));
    std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U},
        client::StoneIndirectDraws::Range{200U, 1U}};
    auto const plan = client::detail::terrainLodReplacementPlan(active, ranges, patch);
    ASSERT_NE(plan.scene, nullptr);
    EXPECT_EQ(plan.scene->draw_index, active.draw_index);
    EXPECT_EQ(sceneFaces(*plan.scene), (std::vector<client::StoneFaceInstance>{
        sceneFaces(patch)[0], sceneFaces(patch)[1], sceneFaces(active)[1]}));
    EXPECT_EQ(plan.scene->logical_face_count, 3U);
    EXPECT_EQ(plan.scene->groups[0].face_storage, patch.groups[0].face_storage);
    EXPECT_EQ(plan.scene->groups[1].face_storage, active.groups[1].face_storage);
    EXPECT_EQ(plan.scene->groups[0].range.first_instance, 0U);
    EXPECT_EQ(plan.scene->groups[0].range.instance_count, 2U);
    EXPECT_EQ(plan.scene->groups[1].range.first_instance, 2U);
    EXPECT_EQ(plan.scene->groups[1].origin_x, 16U);
    EXPECT_FALSE(plan.reusable[0]);
    EXPECT_EQ(plan.reusable[1], 1U);
    EXPECT_EQ(plan.ranges[1].first_instance, 200U);
    EXPECT_EQ(plan.uploaded, (std::vector<uint32_t>{0U, 1U}));
    EXPECT_EQ(plan.scene->identity, active.identity);
    EXPECT_EQ(plan.scene->session_join_epoch, active.session_join_epoch);
    EXPECT_EQ(plan.scene->coverage_epoch, active.coverage_epoch);
    EXPECT_EQ(plan.scene->center_x, active.center_x);
    EXPECT_EQ(plan.scene->center_y, active.center_y);
    EXPECT_EQ(plan.scene->root_extent, active.root_extent);
    EXPECT_EQ(plan.scene->block_sea_level, active.block_sea_level);
    EXPECT_EQ(plan.scene->shading_atlas, active.shading_atlas);
    ASSERT_EQ(plan.scene->leaves.size(), active.leaves.size());
    EXPECT_EQ(plan.scene->leaves[0].level, active.leaves[0].level);
    EXPECT_EQ(plan.scene->leaves[0].x, active.leaves[0].x);
    EXPECT_EQ(plan.scene->actual_column_count, active.actual_column_count);
    client::VulkanFrustum const frustum{glm::mat4{1.0F}};
    std::vector<uint32_t> before, after;
    static_cast<void>(active.draw_index->collect(frustum, {}, before));
    static_cast<void>(plan.scene->draw_index->collect(frustum, {}, after));
    EXPECT_EQ(after, before);
    active.draw_index.reset();
    EXPECT_EQ(client::detail::terrainLodReplacementPlan(active, ranges, patch).scene->draw_index, nullptr);
}

TEST(TerrainLodUploadPlanTest, ChangedVerticalBoundsRefitIndexAndRefreshCulling)
{
    for (bool const change_upper : { false, true }) {
        SCOPED_TRACE(change_upper);
        client::TerrainLodScene active;
        active.groups = {{
            .range = { 0U, 1U }, .extent = 16U,
            .minimum_z = change_upper ? -4 : -2, .maximum_z = change_upper ? 2 : 4,
        }};
        bindSceneFaces(active, std::vector<client::StoneFaceInstance>(1U));
        auto index = std::make_shared<client::TerrainLodDrawIndex>();
        index->reset(active.groups);
        active.draw_index = index;
        auto patch = active;
        patch.groups[0].minimum_z = change_upper ? -4 : 2;
        patch.groups[0].maximum_z = change_upper ? -2 : 4;
        std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U}};
        auto const plan = client::detail::terrainLodReplacementPlan(active, ranges, patch);
        ASSERT_NE(plan.scene->draw_index, nullptr);
        EXPECT_NE(plan.scene->draw_index, active.draw_index);
        client::TerrainLodDrawIndex fresh_reset;
        fresh_reset.reset(plan.scene->groups);
        client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
        std::vector<uint32_t> before, after;
        static_cast<void>(active.draw_index->collect(frustum, {}, before));
        static_cast<void>(plan.scene->draw_index->collect(frustum, {}, after));
        std::vector<uint32_t> expected;
        static_cast<void>(fresh_reset.collect(frustum, {}, expected));
        EXPECT_EQ(before, (std::vector<uint32_t>{ 0U }));
        EXPECT_TRUE(after.empty());
        EXPECT_EQ(after, expected);
    }
}

TEST(TerrainLodUploadPlanTest, RepeatedReplacementReleasesOldGroupStorageAndKeepsOtherGroups)
{
    client::TerrainLodScene active;
    active.groups = {
        {.range = {0U, 2U}, .extent = 16U},
        {.range = {2U, 1U}, .origin_x = 16U, .extent = 16U},
    };
    std::vector<client::StoneFaceInstance> active_faces(3U);
    active_faces[0].x = 10;
    active_faces[1].x = 11;
    active_faces[2].x = 12;
    bindSceneFaces(active, std::move(active_faces));
    std::array const ranges{
        client::StoneIndirectDraws::Range{100U, 2U}, client::StoneIndirectDraws::Range{200U, 1U},
    };
    std::weak_ptr<std::vector<client::StoneFaceInstance> const> obsolete_owner;
    std::shared_ptr<client::TerrainLodScene const> second_scene;
    {
        client::TerrainLodScene first_patch;
        first_patch.groups = {{.range = {0U, 2U}, .extent = 16U}};
        std::vector<client::StoneFaceInstance> first_faces(2U);
        first_faces[0].x = 20;
        first_faces[1].x = 21;
        bindSceneFaces(first_patch, std::move(first_faces));
        obsolete_owner = first_patch.groups[0].face_storage;
        auto first_plan = client::detail::terrainLodReplacementPlan(active, ranges, first_patch);
        {
            client::TerrainLodScene second_patch;
            second_patch.groups = {{.range = {0U, 1U}, .extent = 16U}};
            std::vector<client::StoneFaceInstance> second_faces(1U);
            second_faces[0].x = 30;
            bindSceneFaces(second_patch, std::move(second_faces));
            auto second_plan = client::detail::terrainLodReplacementPlan(
                *first_plan.scene, first_plan.ranges, second_patch);
            second_scene = std::move(second_plan.scene);
        }
        first_plan.scene.reset();
    }
    EXPECT_TRUE(obsolete_owner.expired());
    ASSERT_NE(second_scene, nullptr);
    EXPECT_EQ(second_scene->logical_face_count, 2U);
    EXPECT_EQ(second_scene->groups[1].face_storage, active.groups[1].face_storage);
    EXPECT_EQ(sceneFaces(*second_scene), (std::vector<client::StoneFaceInstance>{
        client::StoneFaceInstance{.x = 30}, client::StoneFaceInstance{.x = 12}}));
    EXPECT_EQ(sceneFaces(active), (std::vector<client::StoneFaceInstance>{
        client::StoneFaceInstance{.x = 10}, client::StoneFaceInstance{.x = 11}, client::StoneFaceInstance{.x = 12}}));
}

TEST(TerrainLodUploadPlanTest, ReplacementRejectsUnmatchedGroupWithoutMutatingActive)
{
    client::TerrainLodScene active;
    active.groups = {{.range = {0U, 1U}, .extent = 16U}};
    bindSceneFaces(active, std::vector<client::StoneFaceInstance>(1U));
    auto patch = active;
    patch.groups[0].origin_x = 16U;
    std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U}};
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReplacementPlan(active, ranges, patch)),
        std::invalid_argument);
    EXPECT_EQ(active.groups[0].origin_x, 0U);
    EXPECT_EQ(sceneFaces(active).size(), 1U);
}

TEST(TerrainLodUploadPlanTest, ReplacementPlannerPublishesOnlyLatestBasePatchAndRangeSnapshot)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 2 };
    auto first_base = makeUploadPlanBaseScene(10, 20);
    auto first_patch = makeUploadPlanPatchScene(11);
    auto latest_patch = makeUploadPlanPatchScene(30);
    auto latest_base = makeUploadPlanBaseScene(100, 200);
    latest_base->chunk_sources = {
        {{0, 0, 0}, {10U, 20U}, 0U},
        {{1, 0, 0}, {13U, 23U}, 4U},
    };
    latest_patch->chunk_sources = { {{0, 0, 0}, {11U, 21U}, 1U} };

    std::weak_ptr<client::TerrainLodScene const> stale_base = first_base;
    std::weak_ptr<client::TerrainLodScene const> stale_patch = first_patch;
    std::vector<client::StoneIndirectDraws::Range> first_ranges{
        { 100U, 1U }, { 200U, 1U },
    };
    std::vector<client::StoneIndirectDraws::Range> latest_ranges{
        { 300U, 1U }, { 400U, 1U },
    };
    client::detail::TerrainLodReplacementPlanner planner;
    EXPECT_FALSE(planner.poll(first_base, first_ranges, first_patch).has_value());
    EXPECT_TRUE(planner.pending());
    EXPECT_FALSE(planner.poll(first_base, first_ranges, latest_patch).has_value());
    EXPECT_FALSE(planner.poll(latest_base, latest_ranges, latest_patch).has_value());
    latest_ranges[1].first_instance = 900U;
    first_base.reset();
    first_patch.reset();

    std::optional<client::detail::TerrainLodUploadPlan> result;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = planner.poll(latest_base, latest_ranges, latest_patch);
        std::this_thread::yield();
    }
    ASSERT_TRUE(result.has_value());
    ASSERT_NE(result->scene, nullptr);
    EXPECT_GT(result->generation, 1U);
    ASSERT_EQ(result->ranges.size(), 2U);
    EXPECT_EQ(result->ranges[1].first_instance, 400U);
    EXPECT_EQ(sceneFaces(*result->scene), (std::vector<client::StoneFaceInstance>{
        client::StoneFaceInstance{ .x = 30 }, client::StoneFaceInstance{ .x = 200 },
    }));
    EXPECT_EQ(result->scene->groups[0].face_storage, latest_patch->groups[0].face_storage);
    EXPECT_EQ(result->scene->groups[1].face_storage, latest_base->groups[1].face_storage);
    EXPECT_EQ(result->scene->chunk_sources, (std::vector<client::TerrainLodChunkSource>{
        {{0, 0, 0}, {11U, 21U}, 1U}, {{1, 0, 0}, {13U, 23U}, 4U},
    }));
    EXPECT_TRUE(stale_base.expired());
    EXPECT_TRUE(stale_patch.expired());
    EXPECT_FALSE(planner.pending());
}

TEST(TerrainLodUploadPlanTest, ReplacementPlannerResetRetainsOwnersUntilStaleWorkDrains)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 2 };
    auto stale_base = makeUploadPlanBaseScene(10, 20);
    auto stale_patch = makeUploadPlanPatchScene(11);
    std::weak_ptr<client::TerrainLodScene const> stale_base_owner = stale_base;
    std::weak_ptr<client::TerrainLodScene const> stale_patch_owner = stale_patch;
    std::array const ranges{
        client::StoneIndirectDraws::Range{ 100U, 1U }, client::StoneIndirectDraws::Range{ 200U, 1U },
    };
    client::detail::TerrainLodReplacementPlanner planner;
    EXPECT_FALSE(planner.poll(stale_base, ranges, stale_patch).has_value());
    planner.invalidate();
    EXPECT_FALSE(planner.pending());
    stale_base.reset();
    stale_patch.reset();
    EXPECT_FALSE(stale_base_owner.expired());
    EXPECT_FALSE(stale_patch_owner.expired());

    auto latest_base = makeUploadPlanBaseScene(100, 200);
    auto latest_patch = makeUploadPlanPatchScene(30);
    std::optional<client::detail::TerrainLodUploadPlan> result;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = planner.poll(latest_base, ranges, latest_patch);
        std::this_thread::yield();
    }
    ASSERT_TRUE(result.has_value());
    ASSERT_NE(result->scene, nullptr);
    EXPECT_EQ(sceneFaces(*result->scene), (std::vector<client::StoneFaceInstance>{
        client::StoneFaceInstance{ .x = 30 }, client::StoneFaceInstance{ .x = 200 },
    }));
    EXPECT_TRUE(stale_base_owner.expired());
    EXPECT_TRUE(stale_patch_owner.expired());
}

TEST(TerrainLodUploadPlanTest, ReplacementPlannerPropagatesWorkerValidationFailure)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 2 };
    auto active = makeUploadPlanBaseScene(10, 20);
    auto invalid_patch = makeUploadPlanPatchScene(30);
    invalid_patch->groups[0].extent = 32U;
    std::array const ranges{
        client::StoneIndirectDraws::Range{ 100U, 1U }, client::StoneIndirectDraws::Range{ 200U, 1U },
    };
    client::detail::TerrainLodReplacementPlanner planner;
    EXPECT_FALSE(planner.poll(active, ranges, invalid_patch).has_value());
    bool failed = false;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!failed && std::chrono::steady_clock::now() < deadline) {
        try {
            static_cast<void>(planner.poll(active, ranges, invalid_patch));
        } catch (std::invalid_argument const&) {
            failed = true;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(failed);
    EXPECT_FALSE(planner.pending());
}

TEST(TerrainLodUploadPlanTest, ExactReplacementPreservesCurrentVisualDiskCenterBeyondCoarseCenter)
{
    auto active = makeUploadPlanBaseScene(10, 20);
    active->center_x = 0U;
    active->center_y = 0U;
    active->visual_center = client::TerrainLodColumnCoordinate{100U, 101U};
    auto patch = makeUploadPlanPatchScene(30);
    std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U},
        client::StoneIndirectDraws::Range{200U, 1U}};
    auto const result = client::detail::terrainLodReplacementPlan(*active, ranges, *patch);
    ASSERT_TRUE(result.scene);
    EXPECT_EQ(result.scene->visual_center, active->visual_center);
    EXPECT_EQ(result.scene->center_x, active->center_x);
    EXPECT_EQ(result.scene->center_y, active->center_y);
    EXPECT_EQ(sceneFaces(*result.scene), (std::vector<client::StoneFaceInstance>{
        client::StoneFaceInstance{.x = 30}, client::StoneFaceInstance{.x = 20}}));
}

TEST(TerrainLodUploadPlanTest, ExactReplacementRejectsConflictingExplicitVisualDiskCenter)
{
    auto active = makeUploadPlanBaseScene(10, 20);
    active->visual_center = client::TerrainLodColumnCoordinate{100U, 101U};
    auto patch = makeUploadPlanPatchScene(30);
    patch->visual_center = client::TerrainLodColumnCoordinate{101U, 101U};
    std::array const ranges{client::StoneIndirectDraws::Range{100U, 1U},
        client::StoneIndirectDraws::Range{200U, 1U}};
    EXPECT_THROW(static_cast<void>(client::detail::terrainLodReplacementPlan(*active, ranges, *patch)),
        std::invalid_argument);
    patch->visual_center = active->visual_center;
    EXPECT_NO_THROW(static_cast<void>(client::detail::terrainLodReplacementPlan(*active, ranges, *patch)));
}

TEST(TerrainLodUploadPlanTest, ExactReplacementRetainsV10SceneCachesForNextBuild)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    shared::TerrainWorld const world{generation::prepareAtlas(configuration)};
    auto const active = client::buildTerrainLodScene(world, 34'816U, 30'208U);
    ASSERT_NE(active->root_artifact, nullptr);
    ASSERT_NE(active->river_constraints, nullptr);
    auto const exact_group = std::ranges::find_if(active->groups, [](auto const& group) {
        return group.extent == 16U && group.range.instance_count != 0U;
    });
    ASSERT_NE(exact_group, active->groups.end());
    client::TerrainLodScene patch;
    patch.identity = active->identity;
    patch.session_join_epoch = active->session_join_epoch;
    patch.coverage_epoch = active->coverage_epoch;
    patch.groups.push_back(*exact_group);
    patch.groups.front().range.first_instance = 0U;
    patch.logical_face_count = patch.groups.front().range.instance_count;
    std::vector<client::StoneIndirectDraws::Range> ranges(active->groups.size());
    auto const merged = client::detail::terrainLodReplacementPlan(*active, ranges, patch).scene;
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->root_artifact, active->root_artifact);
    ASSERT_NE(active->local_layout, nullptr);
    EXPECT_EQ(merged->local_layout, active->local_layout);
    EXPECT_EQ(merged->river_constraints, active->river_constraints);
    EXPECT_EQ(sceneFaces(*merged), sceneFaces(*active));
    auto const next = client::buildTerrainLodScene(world, active->center_x, active->center_y, {}, {}, merged.get());
    EXPECT_EQ(next->root_artifact, active->root_artifact);
    size_t const l0_root_side = shared::WorldExtent::WIDTH / next->root_extent;
    EXPECT_EQ(next->selection_audit.reused_l0_root_count, l0_root_side * l0_root_side);
    EXPECT_EQ(next->selection_audit.rebuilt_l0_root_count, 0U);
    EXPECT_EQ(sceneFaces(*next), sceneFaces(*active));
}

TEST(TerrainLodUploadPlanTest, NormalOnlyPayloadChangeRequiresUploadWithoutChangingGeometry)
{
    auto active = makeUploadPlanBaseScene(0, 16);
    auto candidate = *active;
    auto faces = std::make_shared<std::vector<client::StoneFaceInstance>>(*candidate.groups[0].face_storage);
    faces->front().terrain_normals = {1U, 2U, 3U, 1U};
    candidate.groups[0].face_storage = faces;
    auto const reusable = client::detail::terrainLodReusableGroups(active.get(), candidate);
    ASSERT_EQ(reusable.size(), 2U);
    EXPECT_FALSE(reusable[0]);
    EXPECT_EQ(reusable[1], 1U);
    EXPECT_EQ(candidate.groups[0].minimum_z, active->groups[0].minimum_z);
    EXPECT_EQ(candidate.groups[0].maximum_z, active->groups[0].maximum_z);
    EXPECT_EQ(active->groups[0].face_storage->front().terrain_normals, (std::array<uint32_t, 4>{}));
}
