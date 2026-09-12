#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>

#include <gtest/gtest.h>

namespace {

TEST(PlayerPresentationTest, LocalPlayerThirdPersonPoseOrbitsAroundPlayerCenter)
{
    static constexpr shared::Player PLAYER{
        .id = 7U,
        .x = 12U,
        .y = 20U,
        .ch = '@',
    };
    client::CameraAngles const angles{ .yaw_degrees = 0.0, .pitch_degrees = 0.0, .roll_degrees = 10.0 };
    client::CameraPose const pose = client::localPlayerThirdPersonPose(PLAYER, angles);

    EXPECT_EQ(client::localPlayerCenterPosition(PLAYER), (glm::dvec3{ 13.0, 21.0, 1.0 }));
    EXPECT_EQ(pose.position, (glm::dvec3{ 13.0, 15.0, 1.0 }));
    EXPECT_EQ(pose.angles, angles);

    client::CameraPose const side_pose = client::localPlayerThirdPersonPose(
        PLAYER,
        { .yaw_degrees = 90.0, .pitch_degrees = 0.0 }
    );
    EXPECT_EQ(side_pose.position, (glm::dvec3{ 7.0, 21.0, 1.0 }));

    client::CameraPose const elevated_pose = client::localPlayerThirdPersonPose(
        PLAYER,
        { .yaw_degrees = 0.0, .pitch_degrees = -30.0 }
    );
    EXPECT_NEAR(elevated_pose.position.y, 15.803'847'577'3, 1e-9);
    EXPECT_NEAR(elevated_pose.position.z, 4.0, 1e-9);

    client::Camera const camera{ elevated_pose };
    EXPECT_NEAR(camera.forward().x, 0.0, 1e-12);
    EXPECT_NEAR(camera.forward().y, 0.866'025'403'8, 1e-9);
    EXPECT_NEAR(camera.forward().z, -0.5, 1e-9);
}

TEST(PlayerPresentationTest, RemoteFilterRemainsAvailableForPresentationMetadata)
{
    static constexpr shared::Player LOCAL{
        .id = 1U,
        .x = 1U,
        .y = 2U,
        .ch = '@',
    };
    static constexpr shared::Player REMOTE{
        .id = 2U,
        .x = 3U,
        .y = 4U,
        .ch = '#',
    };

    EXPECT_FALSE(client::shouldRenderRemotePlayer(LOCAL, '@'));
    EXPECT_TRUE(client::shouldRenderRemotePlayer(REMOTE, '@'));
}

TEST(PlayerPresentationTest, CameraFollowsAuthoritativeSubcellPosition)
{
    static constexpr shared::Player PLAYER{
        .id = 7U,
        .x = 12U,
        .y = 20U,
        .x_subcell = 5'000U,
        .y_subcell = 2'500U,
        .ch = '@',
    };

    client::CameraPose const pose = client::localPlayerThirdPersonPose(PLAYER, { });
    EXPECT_DOUBLE_EQ(pose.position.x, 13.5);
    EXPECT_DOUBLE_EQ(pose.position.y, 15.25);
}

} // namespace
