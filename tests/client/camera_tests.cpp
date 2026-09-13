#include <client/Camera.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>

namespace {

void expectNear(glm::dvec3 const actual, glm::dvec3 const expected) {
    EXPECT_NEAR(actual.x, expected.x, 1e-12);
    EXPECT_NEAR(actual.y, expected.y, 1e-12);
    EXPECT_NEAR(actual.z, expected.z, 1e-12);
}

} // namespace

TEST(CameraTest, DefaultAxesFollowRightHandedZUpContract) {
    client::Camera const camera;

    expectNear(camera.forward(), { 0.0, 1.0, 0.0 });
    expectNear(camera.right(), { 1.0, 0.0, 0.0 });
    expectNear(camera.up(), { 0.0, 0.0, 1.0 });
}

TEST(CameraTest, ViewMatrixPlacesForwardDirectionOnNegativeViewZ) {
    static constexpr float CAMERA_X = 3.0f;
    static constexpr float CAMERA_Y = 4.0f;
    static constexpr float CAMERA_Z = 5.0f;

    client::Camera camera;
    ASSERT_TRUE(camera.setPosition({ CAMERA_X, CAMERA_Y, CAMERA_Z }));

    glm::mat4 const view = camera.viewMatrix();
    glm::vec4 const camera_position = view * glm::vec4{ CAMERA_X, CAMERA_Y, CAMERA_Z, 1.0f };
    glm::vec4 const forward_position = view * glm::vec4{ CAMERA_X, CAMERA_Y + 2.0f, CAMERA_Z, 1.0f };
    EXPECT_NEAR(camera_position.x, 0.0f, 1e-6f);
    EXPECT_NEAR(camera_position.y, 0.0f, 1e-6f);
    EXPECT_NEAR(camera_position.z, 0.0f, 1e-6f);
    EXPECT_NEAR(forward_position.z, -2.0f, 1e-6f);
}

TEST(CameraTest, CardinalYawKeepsForwardAndRightOrthogonal) {
    client::Camera camera;
    ASSERT_TRUE(camera.setAngles({ .yaw_degrees = 90.0, .pitch_degrees = 0.0 }));

    expectNear(camera.forward(), { 1.0, 0.0, 0.0 });
    expectNear(camera.right(), { 0.0, -1.0, 0.0 });
    EXPECT_NEAR(glm::dot(camera.forward(), camera.right()), 0.0, 1e-12);
}

TEST(CameraTest, RollIsMeasuredInDegreesAroundTheForwardAxis) {
    client::Camera camera;

    ASSERT_TRUE(camera.setAngles({ .roll_degrees = 90.0 }));
    EXPECT_DOUBLE_EQ(camera.pose().angles.roll_degrees, 90.0);
    expectNear(camera.up(), { 1.0, 0.0, 0.0 });

    ASSERT_TRUE(camera.rotate(0.0, 0.0, 360.0));
    EXPECT_DOUBLE_EQ(camera.pose().angles.roll_degrees, 90.0);
}

TEST(CameraTest, YawIsNormalizedToTheCanonicalFullTurnRange) {
    client::Camera camera;

    ASSERT_TRUE(camera.setAngles({ .yaw_degrees = -360.0, .pitch_degrees = 0.0 }));
    EXPECT_DOUBLE_EQ(camera.pose().angles.yaw_degrees, 0.0);

    ASSERT_TRUE(camera.rotate(450.0, 0.0));
    EXPECT_DOUBLE_EQ(camera.pose().angles.yaw_degrees, 90.0);
    expectNear(camera.forward(), { 1.0, 0.0, 0.0 });
}

TEST(CameraTest, PitchIsClampedAwayFromSingularity) {
    client::Camera camera;

    ASSERT_TRUE(camera.setAngles({ .yaw_degrees = 0.0, .pitch_degrees = 180.0 }));
    EXPECT_DOUBLE_EQ(camera.pose().angles.pitch_degrees, client::Camera::MAX_PITCH_DEGREES);
    EXPECT_LT(camera.forward().y, 1.0);
    EXPECT_GT(camera.forward().y, 0.0);

    ASSERT_TRUE(camera.setAngles({ .yaw_degrees = 0.0, .pitch_degrees = -180.0 }));
    EXPECT_DOUBLE_EQ(camera.pose().angles.pitch_degrees, client::Camera::MIN_PITCH_DEGREES);
    EXPECT_GT(camera.forward().y, 0.0);
}

TEST(CameraTest, NonFinitePoseAndRotationAreRejectedWithoutMutation) {
    client::Camera camera;
    ASSERT_TRUE(camera.setPosition({ 2.0, 3.0, 4.0 }));
    ASSERT_TRUE(camera.setAngles({ .yaw_degrees = 35.0, .pitch_degrees = 12.0, .roll_degrees = 4.0 }));
    client::CameraPose const before = camera.pose();

    EXPECT_FALSE(camera.setPosition({ NAN, 3.0, 4.0 }));
    EXPECT_FALSE(camera.setAngles({ .yaw_degrees = INFINITY, .pitch_degrees = 12.0 }));
    EXPECT_FALSE(camera.rotate(1.0, NAN, 0.0));
    EXPECT_EQ(camera.pose(), before);

    ASSERT_TRUE(camera.rotate(std::numeric_limits<double>::max(), std::numeric_limits<double>::max()));
    EXPECT_TRUE(std::isfinite(camera.pose().angles.yaw_degrees));
    EXPECT_DOUBLE_EQ(camera.pose().angles.pitch_degrees, client::Camera::MAX_PITCH_DEGREES);
}

TEST(CameraTest, ProjectionRejectsZeroExtentAndUsesFiniteAspect) {
    client::Camera const camera;

    EXPECT_FALSE(camera.projectionMatrix(0, 1080).has_value());
    EXPECT_FALSE(camera.projectionMatrix(1920, 0).has_value());

    std::optional<glm::mat4> const projection = camera.projectionMatrix(1920, 1080);
    ASSERT_TRUE(projection.has_value());
    EXPECT_TRUE(std::isfinite((*projection)[0][0]));
    EXPECT_TRUE(std::isfinite((*projection)[1][1]));
    EXPECT_GT((*projection)[0][0], 0.0f);
    EXPECT_LT((*projection)[1][1], 0.0f);
}

TEST(CameraTest, ProjectionUsesVulkanZeroToOneDepthRange) {
    static constexpr uint32_t WIDTH = 1920;
    static constexpr uint32_t HEIGHT = 1080;
    static constexpr float NEAR_VIEW_DEPTH = -0.1f;
    static constexpr float FAR_VIEW_DEPTH = -1'000.0f;

    client::Camera const camera;
    std::optional<glm::mat4> const projection = camera.projectionMatrix(WIDTH, HEIGHT);
    ASSERT_TRUE(projection.has_value());

    glm::vec4 const near_clip = *projection * glm::vec4{ 0.0f, 0.0f, NEAR_VIEW_DEPTH, 1.0f };
    glm::vec4 const far_clip = *projection * glm::vec4{ 0.0f, 0.0f, FAR_VIEW_DEPTH, 1.0f };
    EXPECT_NEAR(near_clip.z / near_clip.w, 0.0f, 1e-6f);
    EXPECT_NEAR(far_clip.z / far_clip.w, 1.0f, 1e-6f);
}

TEST(CameraTest, ProjectionMapsWorldUpTowardPositiveViewportTop)
{
    static constexpr float WORLD_DISTANCE = 5.0F;
    static constexpr float WORLD_HEIGHT = 1.0F;

    client::Camera const camera;
    std::optional<glm::mat4> const projection = camera.projectionMatrix(1920U, 1080U);
    ASSERT_TRUE(projection.has_value());

    glm::mat4 const projection_view = *projection * camera.viewMatrix();
    glm::vec4 const above_clip = projection_view * glm::vec4{
        0.0F,
        WORLD_DISTANCE,
        WORLD_HEIGHT,
        1.0F,
    };
    glm::vec4 const below_clip = projection_view * glm::vec4{
        0.0F,
        WORLD_DISTANCE,
        -WORLD_HEIGHT,
        1.0F,
    };

    EXPECT_LT(above_clip.y / above_clip.w, 0.0F);
    EXPECT_GT(below_clip.y / below_clip.w, 0.0F);
}

TEST(CameraTest, InvalidProjectionIsRejectedWithoutMutation) {
    client::Camera camera;
    client::CameraProjection const before = camera.projection();

    EXPECT_FALSE(camera.setProjection({ .vertical_fov_degrees = 0.0 }));
    EXPECT_FALSE(camera.setProjection({ .vertical_fov_degrees = 70.0, .near_plane = INFINITY }));
    EXPECT_EQ(camera.projection(), before);
}
