#include <client/Camera.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <glm/vec4.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>

namespace {

[[nodiscard]] bool cornersIntersect(
    glm::vec3 const minimum,
    glm::vec3 const maximum,
    glm::mat4 const& projection_view
) {
    std::array<bool, 6> outside{ true, true, true, true, true, true };
    for (uint32_t corner_index = 0U; corner_index < 8U; ++corner_index) {
        glm::vec4 const corner{
            (corner_index & 1U) != 0U ? maximum.x : minimum.x,
            (corner_index & 2U) != 0U ? maximum.y : minimum.y,
            (corner_index & 4U) != 0U ? maximum.z : minimum.z,
            1.0F,
        };
        glm::vec4 const clip = projection_view * corner;
        outside[0] = outside[0] && clip.x < -clip.w;
        outside[1] = outside[1] && clip.x > clip.w;
        outside[2] = outside[2] && clip.y < -clip.w;
        outside[3] = outside[3] && clip.y > clip.w;
        outside[4] = outside[4] && clip.z < 0.0F;
        outside[5] = outside[5] && clip.z > clip.w;
    }
    return std::ranges::none_of(outside, [](bool const is_outside) { return is_outside; });
}

TEST(VulkanFrustumTest, UsesAllSixZeroToOneClipPlanesAndRetainsTouchingBounds)
{
    static constexpr float OUTSIDE_OFFSET = 0.01F;
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    EXPECT_TRUE(frustum.intersects({ -1.0F, -1.0F, 0.0F }, { 1.0F, 1.0F, 1.0F }));
    std::array<glm::vec3, 6> const boundary_points{
        glm::vec3{ -1.0F, 0.0F, 0.5F },
        glm::vec3{ 1.0F, 0.0F, 0.5F },
        glm::vec3{ 0.0F, -1.0F, 0.5F },
        glm::vec3{ 0.0F, 1.0F, 0.5F },
        glm::vec3{ 0.0F, 0.0F, 0.0F },
        glm::vec3{ 0.0F, 0.0F, 1.0F },
    };
    std::array<glm::vec3, 6> const outside_offsets{
        glm::vec3{ -OUTSIDE_OFFSET, 0.0F, 0.0F },
        glm::vec3{ OUTSIDE_OFFSET, 0.0F, 0.0F },
        glm::vec3{ 0.0F, -OUTSIDE_OFFSET, 0.0F },
        glm::vec3{ 0.0F, OUTSIDE_OFFSET, 0.0F },
        glm::vec3{ 0.0F, 0.0F, -OUTSIDE_OFFSET },
        glm::vec3{ 0.0F, 0.0F, OUTSIDE_OFFSET },
    };
    for (uint32_t index = 0U; index < boundary_points.size(); ++index) {
        SCOPED_TRACE(index);
        glm::vec3 const boundary = boundary_points[index];
        glm::vec3 const outside = boundary + outside_offsets[index];
        EXPECT_TRUE(frustum.intersects(boundary, boundary));
        EXPECT_FALSE(frustum.intersects(outside, outside));
    }
}

TEST(VulkanFrustumTest, PreservesNearPlaneCrossingsCameraInsideAndBehindCameraRejection)
{
    static constexpr uint32_t WIDTH = 1'920U;
    static constexpr uint32_t HEIGHT = 1'080U;
    client::Camera const camera{};
    glm::mat4 const projection_view = *camera.projectionMatrix(WIDTH, HEIGHT) * camera.viewMatrix();
    client::VulkanFrustum const frustum{ projection_view };
    EXPECT_TRUE(frustum.intersects({ -1.0F, -1.0F, -1.0F }, { 1.0F, 1.0F, 1.0F }));
    EXPECT_TRUE(frustum.intersects({ -0.01F, 0.05F, -0.01F }, { 0.01F, 0.15F, 0.01F }));
    EXPECT_FALSE(frustum.intersects({ -0.01F, 0.01F, -0.01F }, { 0.01F, 0.05F, 0.01F }));
    EXPECT_FALSE(frustum.intersects({ -1.0F, -10.0F, -1.0F }, { 1.0F, -2.0F, 1.0F }));
}

TEST(VulkanFrustumTest, RetainsBoxesWhoseFaceIntersectsWithoutAnInsideCorner)
{
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    EXPECT_TRUE(frustum.intersects({ -2.0F, -2.0F, 0.4F }, { 2.0F, 2.0F, 0.6F }));
    EXPECT_TRUE(frustum.intersects({ -2.0F, -0.2F, -1.0F }, { 2.0F, 0.2F, 2.0F }));
}

TEST(VulkanFrustumTest, WrapsBoundsToTheSameNearestWorldImageAcrossTheSeam)
{
    static constexpr uint32_t WIDTH = 1'920U;
    static constexpr uint32_t HEIGHT = 1'080U;
    static constexpr double WORLD_PERIOD = 65'536.0;
    client::Camera const camera{ { .position = { WORLD_PERIOD - 8.0, WORLD_PERIOD - 8.0, 16.0 } } };
    client::WrappedBounds const wrapped = client::boundsNearestToCamera(
        { 0.0F, 0.0F, 0.0F }, { 16.0F, 16.0F, 32.0F }, camera.pose().position
    );
    EXPECT_EQ(wrapped.minimum, (glm::vec3{ WORLD_PERIOD, WORLD_PERIOD, 0.0 }));
    EXPECT_EQ(wrapped.maximum, (glm::vec3{ WORLD_PERIOD + 16.0, WORLD_PERIOD + 16.0, 32.0 }));
    glm::mat4 const projection_view = *camera.projectionMatrix(WIDTH, HEIGHT) * camera.viewMatrix();
    EXPECT_EQ(
        client::VulkanFrustum{ projection_view }.intersects(wrapped.minimum, wrapped.maximum),
        cornersIntersect(wrapped.minimum, wrapped.maximum, projection_view)
    );
    client::WrappedBounds const reverse = client::boundsNearestToCamera(
        { 65'520.0F, 65'520.0F, 0.0F }, { 65'536.0F, 65'536.0F, 32.0F }, { 8.0, 8.0, 16.0 }
    );
    EXPECT_EQ(reverse.minimum, (glm::vec3{ -16.0F, -16.0F, 0.0F }));
    EXPECT_EQ(reverse.maximum, (glm::vec3{ 0.0F, 0.0F, 32.0F }));
}

TEST(VulkanFrustumTest, MatchesCornerCullingAcrossFovFramebufferAndWideTerrainDistances)
{
    static constexpr uint32_t SEED = 27'100U;
    static constexpr uint32_t BOX_COUNT = 10'000U;
    static constexpr std::array<uint32_t, 2> RADII{ 256U, 1'024U };
    static constexpr std::array<double, 3> FOVS{ 35.0, 70.0, 110.0 };
    static constexpr std::array<std::array<uint32_t, 2>, 3> EXTENTS{
        std::array<uint32_t, 2>{ 640U, 480U },
        std::array<uint32_t, 2>{ 1'920U, 1'080U },
        std::array<uint32_t, 2>{ 480U, 1'920U },
    };
    std::minstd_rand generator{ SEED };
    std::uniform_real_distribution<float> position{ -1.0F, 1.0F };
    std::uniform_real_distribution<float> size{ 0.1F, 32.0F };
    for (uint32_t const radius : RADII) {
        for (double const fov : FOVS) {
            for (std::array<uint32_t, 2> const extent : EXTENTS) {
                client::Camera const camera{
                    {
                        .position = { 32'763.0, -32'761.0, 450.0 },
                        .angles = { .yaw_degrees = 37.2, .pitch_degrees = -35.0, .roll_degrees = 11.0 },
                    },
                    { .vertical_fov_degrees = fov, .far_plane = static_cast<double>(radius) * 32.0 },
                };
                glm::mat4 const projection_view = *camera.projectionMatrix(extent[0], extent[1])
                    * camera.viewMatrix();
                client::VulkanFrustum const frustum{ projection_view };
                uint32_t conservative_extra_count = 0U;
                for (uint32_t index = 0U; index < BOX_COUNT; ++index) {
                    glm::vec3 const minimum = glm::vec3{ camera.pose().position } + glm::vec3{
                        position(generator) * radius * 16.0F,
                        position(generator) * radius * 16.0F,
                        position(generator) * 512.0F,
                    };
                    glm::vec3 const maximum = minimum + glm::vec3{ size(generator), size(generator), size(generator) };
                    bool const corners_visible = cornersIntersect(minimum, maximum, projection_view);
                    bool const planes_visible = frustum.intersects(minimum, maximum);
                    ASSERT_TRUE(planes_visible || !corners_visible)
                        << "radius=" << radius << " fov=" << fov << " extent=" << extent[0] << 'x' << extent[1]
                        << " box=" << index;
                    conservative_extra_count += planes_visible && !corners_visible ? 1U : 0U;
                }
                EXPECT_LE(conservative_extra_count, 2U)
                    << "radius=" << radius << " fov=" << fov << " extent=" << extent[0] << 'x' << extent[1];
            }
        }
    }
}

} // namespace
