#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>

#include <gtest/gtest.h>

namespace {

TEST(PlayerPresentationTest, LocalPlayerUsesCenteredFirstPersonEyeWithoutChangingAngles)
{
    static constexpr shared::Player PLAYER{
        .id = 7U,
        .x = 12U,
        .y = 20U,
        .ch = '@',
    };
    client::Camera camera{
        {
            .position = { 0.0, 0.0, 0.0 },
            .angles = {
                .yaw_degrees = 135.0,
                .pitch_degrees = -20.0,
                .roll_degrees = 10.0,
            },
        },
    };
    client::CameraAngles const angles_before = camera.pose().angles;

    ASSERT_TRUE(camera.setPosition(client::localPlayerEyePosition(PLAYER)));

    EXPECT_EQ(camera.pose().position, (glm::dvec3{ 13.0, 21.0, 1.6 }));
    EXPECT_EQ(camera.pose().angles, angles_before);
}

TEST(PlayerPresentationTest, OnlyRemotePlayersArePresented)
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

} // namespace
