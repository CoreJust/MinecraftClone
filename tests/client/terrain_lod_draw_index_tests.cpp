#include <client/Camera.hpp>
#include <client/render/TerrainLodDrawIndex.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <vector>

namespace {

TEST(TerrainLodDrawIndexTest, MatchesFlatTraversalAcrossMovingWrappedCurvedViews)
{
    std::vector<client::TerrainLodDrawGroup> groups;
    std::minstd_rand random{ 564U };
    for (uint32_t i = 0U; i < 73'756U; ++i) {
        uint32_t const extent = 16U << (random() % 5U);
        groups.push_back({ .range = { i * 3U, 3U }, .origin_x = (random() % 65'536U) / extent * extent,
            .origin_y = (random() % 65'536U) / extent * extent, .extent = extent,
            .minimum_z = static_cast<int32_t>(random() % 512U), .maximum_z = 1'024 });
    }
    client::TerrainLodDrawIndex index;
    index.reset(groups);
    std::vector<uint32_t> actual;
    uint64_t tested = 0U;
    uint64_t flat_ns = 0U;
    uint64_t indexed_ns = 0U;
    for (uint32_t sample = 0U; sample < 48U; ++sample) {
        glm::dvec3 const position{ sample % 3U == 0U ? -16.0 : sample % 3U == 1U ? 65'520.0 : 32'768.0,
            static_cast<double>((sample * 7'919U) % 65'536U), 32.0 + sample * 31.0 };
        client::Camera const camera{ { .position = position,
            .angles = { .yaw_degrees = sample * 37.0, .pitch_degrees = -17.0 } },
            { .vertical_fov_degrees = 40.0 + (sample % 3U) * 55.0, .far_plane = 65'536.0 } };
        auto const projection = camera.projectionMatrix(1'600U, 900U);
        ASSERT_TRUE(projection);
        client::VulkanFrustum const frustum{ *projection * glm::lookAtRH(glm::vec3(0.0F),
            glm::vec3(camera.forward()), glm::vec3(camera.up())) };
        auto const start = std::chrono::steady_clock::now();
        std::vector<uint32_t> expected;
        for (uint32_t ordinal = 0U; ordinal < groups.size(); ++ordinal) {
            auto const& group = groups[ordinal];
            client::WrappedBounds const bounds = client::curvedBoundsNearestToCamera(
                { group.origin_x, group.origin_y, group.minimum_z },
                { group.origin_x + group.extent, group.origin_y + group.extent, group.maximum_z }, position);
            if (client::insideWorldVisibility(bounds, client::worldVisibilityRadius(position.z))
                && frustum.intersects(bounds.minimum, bounds.maximum)) {
                expected.push_back(ordinal);
            }
        }
        auto const midpoint = std::chrono::steady_clock::now();
        auto const work = index.collect(frustum, position, actual);
        auto const finish = std::chrono::steady_clock::now();
        flat_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(midpoint - start).count());
        indexed_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - midpoint).count());
        tested += work.groups_tested;
        EXPECT_EQ(actual, expected) << "sample " << sample;
    }
    EXPECT_LT(tested, groups.size() * 48U / 2U);
    RecordProperty("flat_duration_ns", flat_ns);
    RecordProperty("indexed_duration_ns", indexed_ns);
    RecordProperty("groups_tested", tested);
    RecordProperty("flat_groups", groups.size() * 48U);
}

TEST(TerrainLodDrawIndexTest, ExactNearestImageCutsAndMixedExtentsKeepFlatMembershipAndOrder)
{
    std::vector<client::TerrainLodDrawGroup> groups;
    for (uint32_t i = 0U; i < 64U; ++i) {
        uint32_t const extent = 16U << (i % 5U);
        groups.push_back({ .range = { i, 1U }, .origin_x = i % 2U == 0U ? 0U : 65'536U - extent,
            .origin_y = (i * 1'024U) % 65'536U, .extent = extent, .minimum_z = -1'024, .maximum_z = 2'048 });
    }
    groups.push_back({ .range = { 64U, 1U }, .extent = 65'536U,
        .minimum_z = -2'048, .maximum_z = 4'096 });
    client::TerrainLodDrawIndex index;
    index.reset(groups);
    for (double const x : { -32'768.001, -32'768.0, -32'767.999, 0.0,
            32'767.999, 32'768.0, 32'768.001, 65'536.0, 98'304.0 }) {
        for (float const scale : { 1.0F, 0.0001F, -0.0001F }) {
            client::VulkanFrustum const frustum{ glm::scale(glm::mat4{ 1.0F }, glm::vec3(scale)) };
            glm::dvec3 const camera{ x, x, 1'024.0 };
            std::vector<uint32_t> expected;
            for (uint32_t i = 0U; i < groups.size(); ++i) {
                auto const& group = groups[i];
                auto const bounds = client::curvedBoundsNearestToCamera(
                    { group.origin_x, group.origin_y, group.minimum_z },
                    { group.origin_x + group.extent, group.origin_y + group.extent, group.maximum_z }, camera);
                if (client::insideWorldVisibility(bounds, client::worldVisibilityRadius(camera.z))
                    && frustum.intersects(bounds.minimum, bounds.maximum)) {
                    expected.push_back(i);
                }
            }
            std::vector<uint32_t> actual;
            static_cast<void>(index.collect(frustum, camera, actual));
            EXPECT_EQ(actual, expected) << "camera " << x << " scale " << scale;
        }
    }
}

TEST(TerrainLodDrawIndexTest, ResetReplacesTheSceneAndEmptySceneProducesNoGroups)
{
    client::TerrainLodDrawIndex index;
    std::vector<client::TerrainLodDrawGroup> groups{ { .origin_x = 0U, .origin_y = 0U, .extent = 16U,
        .minimum_z = 0, .maximum_z = 16 } };
    index.reset(groups);
    std::vector<uint32_t> actual;
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    static_cast<void>(index.collect(frustum, {}, actual));
    ASSERT_EQ(actual, (std::vector<uint32_t>{ 0U }));
    index.reset({});
    static_cast<void>(index.collect(frustum, {}, actual));
    EXPECT_TRUE(actual.empty());
}

TEST(TerrainLodDrawIndexTest, RefitMatchesFreshResetAcrossHeightEditsAndWrappedViews)
{
    static constexpr uint32_t GRID_GROUP_COUNT = 16U;
    static constexpr uint32_t GROUP_EXTENT = 16U;
    static constexpr uint32_t WORLD_PERIOD = 65'536U;
    std::vector<client::TerrainLodDrawGroup> original_groups;
    for (uint32_t index = 0U; index < GRID_GROUP_COUNT; ++index) {
        original_groups.push_back({
            .range = { index, 1U },
            .origin_x = (index % 4U) * GROUP_EXTENT,
            .origin_y = (index / 4U) * GROUP_EXTENT,
            .extent = GROUP_EXTENT,
            .minimum_z = -8,
            .maximum_z = 8,
        });
    }
    original_groups[0].minimum_z = -4;
    original_groups[0].maximum_z = 2;
    original_groups[1].minimum_z = -2;
    original_groups[1].maximum_z = 4;
    original_groups.push_back({
        .range = { GRID_GROUP_COUNT, 1U }, .origin_x = WORLD_PERIOD - GROUP_EXTENT,
        .origin_y = 0U, .extent = GROUP_EXTENT,
        .minimum_z = -8, .maximum_z = 8,
    });
    original_groups.push_back({
        .range = { GRID_GROUP_COUNT + 1U, 1U }, .origin_x = 0U,
        .origin_y = WORLD_PERIOD - GROUP_EXTENT, .extent = GROUP_EXTENT,
        .minimum_z = -8, .maximum_z = 8,
    });

    auto updated_groups = original_groups;
    updated_groups[0].maximum_z = -2;
    updated_groups[1].minimum_z = 2;
    client::TerrainLodDrawIndex original_index;
    original_index.reset(original_groups);
    client::TerrainLodDrawIndex refitted_index = original_index;
    ASSERT_TRUE(refitted_index.refit(updated_groups));
    client::TerrainLodDrawIndex original_reset;
    original_reset.reset(original_groups);
    client::TerrainLodDrawIndex updated_reset;
    updated_reset.reset(updated_groups);

    client::VulkanFrustum const edit_frustum{
        glm::scale(glm::mat4{ 1.0F }, glm::vec3{ 0.01F, 0.01F, 1.0F }) };
    std::vector<uint32_t> before_edit;
    std::vector<uint32_t> after_edit;
    static_cast<void>(original_index.collect(edit_frustum, {}, before_edit));
    static_cast<void>(refitted_index.collect(edit_frustum, {}, after_edit));
    EXPECT_EQ(before_edit, (std::vector<uint32_t>{ 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U }));
    EXPECT_EQ(after_edit, (std::vector<uint32_t>{ 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U }));

    std::array const positions{
        glm::dvec3{ 0.0, 0.0, 0.0 },
        glm::dvec3{ 65'536.0, 0.0, 64.0 },
        glm::dvec3{ 0.0, 65'536.0, 128.0 },
        glm::dvec3{ 65'536.0, 65'536.0, 256.0 },
        glm::dvec3{ -16.0, -16.0, 0.0 },
    };
    for (uint32_t sample = 0U; sample < positions.size(); ++sample) {
        SCOPED_TRACE(sample);
        client::Camera const camera{
            { .position = positions[sample], .angles = { .yaw_degrees = sample * 90.0, .pitch_degrees = -17.0 } },
            { .vertical_fov_degrees = 90.0, .far_plane = 65'536.0 },
        };
        auto const projection = camera.projectionMatrix(1'600U, 900U);
        ASSERT_TRUE(projection);
        client::VulkanFrustum const frustum{ *projection * glm::lookAtRH(glm::vec3(0.0F),
            glm::vec3(camera.forward()), glm::vec3(camera.up())) };
        std::vector<uint32_t> old_actual;
        std::vector<uint32_t> old_expected;
        std::vector<uint32_t> new_actual;
        std::vector<uint32_t> new_expected;
        static_cast<void>(original_index.collect(frustum, positions[sample], old_actual));
        static_cast<void>(original_reset.collect(frustum, positions[sample], old_expected));
        static_cast<void>(refitted_index.collect(frustum, positions[sample], new_actual));
        static_cast<void>(updated_reset.collect(frustum, positions[sample], new_expected));
        EXPECT_EQ(old_actual, old_expected);
        EXPECT_EQ(new_actual, new_expected);
    }
}

TEST(TerrainLodDrawIndexTest, RefitRejectsChangedGroupCountAndSpatialTopology)
{
    static constexpr uint32_t TOPOLOGY_GROUP_COUNT = 9U;
    static constexpr uint32_t GROUP_EXTENT = 16U;
    std::vector<client::TerrainLodDrawGroup> groups;
    for (uint32_t index = 0U; index < TOPOLOGY_GROUP_COUNT; ++index) {
        groups.push_back({
            .range = { index, 1U }, .origin_x = (index % 3U) * GROUP_EXTENT,
            .origin_y = (index / 3U) * GROUP_EXTENT, .extent = GROUP_EXTENT,
            .minimum_z = -4, .maximum_z = 4,
        });
    }
    client::TerrainLodDrawIndex published_index;
    published_index.reset(groups);
    auto expect_rejected = [&](std::vector<client::TerrainLodDrawGroup> changed_groups) {
        client::TerrainLodDrawIndex candidate = published_index;
        EXPECT_FALSE(candidate.refit(changed_groups));
        client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
        std::vector<uint32_t> published;
        std::vector<uint32_t> candidate_groups;
        static_cast<void>(published_index.collect(frustum, {}, published));
        static_cast<void>(candidate.collect(frustum, {}, candidate_groups));
        EXPECT_EQ(candidate_groups, published);
    };

    auto changed_groups = groups;
    changed_groups[0].origin_x += GROUP_EXTENT;
    expect_rejected(changed_groups);
    changed_groups = groups;
    changed_groups[0].extent += GROUP_EXTENT;
    expect_rejected(changed_groups);
    changed_groups = groups;
    std::swap(changed_groups[0], changed_groups[1]);
    expect_rejected(changed_groups);
    changed_groups = groups;
    changed_groups.pop_back();
    expect_rejected(changed_groups);
}

} // namespace
