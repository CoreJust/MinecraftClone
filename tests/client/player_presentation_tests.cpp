#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>

#include <gtest/gtest.h>

#include <chrono>

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

TEST(PlayerPresentationTest, FirstPersonCameraAndInterpolationFollowFlightHeight)
{
    static constexpr shared::Player INITIAL{
        .id = 7U,
        .x = 12,
        .y = 20,
        .z = 12,
        .ch = '@',
    };
    static constexpr shared::Player TARGET{
        .id = 7U,
        .x = 12,
        .y = 20,
        .z = 13,
        .z_subcell = 5'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const STARTED_AT{};
    client::PlayerPresentation presentation;

    presentation.update(INITIAL, STARTED_AT);
    presentation.update(TARGET, STARTED_AT + std::chrono::milliseconds{ 100 });

    ASSERT_TRUE(presentation.sample('@', STARTED_AT + std::chrono::milliseconds{ 150 }).has_value());
    client::CameraPose const pose = client::localPlayerFirstPersonPose(
        *presentation.sample('@', STARTED_AT + std::chrono::milliseconds{ 150 }),
        { .yaw_degrees = 90.0 }
    );
    EXPECT_DOUBLE_EQ(pose.position.x, 13.0);
    EXPECT_DOUBLE_EQ(pose.position.y, 21.0);
    EXPECT_DOUBLE_EQ(pose.position.z, 13.75);
    EXPECT_DOUBLE_EQ(pose.angles.yaw_degrees, 90.0);
}

TEST(PlayerPresentationTest, InitialAuthoritativePositionSnapsAndSubsequentPositionsInterpolate)
{
    static constexpr shared::Player INITIAL{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .ch = '@',
    };
    static constexpr shared::Player TARGET{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .x_subcell = 4'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(INITIAL, started_at);
    ASSERT_TRUE(presentation.sample('@', started_at).has_value());
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at)->x, 0.0);

    presentation.update(TARGET, started_at + std::chrono::milliseconds{ 100 });
    EXPECT_DOUBLE_EQ(shared::playerPositionX(TARGET), 0.4);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 125 })->x, 0.1);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 150 })->x, 0.2);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 175 })->x, 0.3);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 200 })->x, 0.4);
}

TEST(PlayerPresentationTest, HorizontalInterpolationCrossesWorldSeamByShortestPath)
{
    static constexpr shared::Player BEFORE_SEAM{
        .id = 1U,
        .x = 65'535U,
        .y = 0U,
        .x_subcell = 9'000U,
        .y_subcell = 1'000U,
        .ch = '@',
    };
    static constexpr shared::Player AFTER_SEAM{
        .id = 1U,
        .x = 0U,
        .y = 65'535U,
        .x_subcell = 1'000U,
        .y_subcell = 9'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(BEFORE_SEAM, started_at);
    presentation.update(AFTER_SEAM, started_at + std::chrono::milliseconds{ 100 });

    auto const quarter = presentation.sample('@', started_at + std::chrono::milliseconds{ 125 });
    auto const midpoint = presentation.sample('@', started_at + std::chrono::milliseconds{ 150 });
    auto const three_quarters = presentation.sample('@', started_at + std::chrono::milliseconds{ 175 });
    ASSERT_TRUE(quarter.has_value());
    ASSERT_TRUE(midpoint.has_value());
    ASSERT_TRUE(three_quarters.has_value());
    EXPECT_NEAR(quarter->x, 65'535.95, 1e-9);
    EXPECT_NEAR(midpoint->x, 0.0, 1e-9);
    EXPECT_NEAR(three_quarters->x, 0.05, 1e-9);
    EXPECT_NEAR(quarter->y, 0.05, 1e-9);
    EXPECT_NEAR(midpoint->y, 0.0, 1e-9);
    EXPECT_NEAR(three_quarters->y, 65'535.95, 1e-9);
}

TEST(PlayerPresentationTest, HorizontalInterpolationCrossesWorldSeamInReverse)
{
    static constexpr shared::Player AFTER_SEAM{
        .id = 1U,
        .x = 0U,
        .x_subcell = 1'000U,
        .ch = '@',
    };
    static constexpr shared::Player BEFORE_SEAM{
        .id = 1U,
        .x = 65'535U,
        .x_subcell = 9'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(AFTER_SEAM, started_at);
    presentation.update(BEFORE_SEAM, started_at + std::chrono::milliseconds{ 100 });

    auto const midpoint = presentation.sample('@', started_at + std::chrono::milliseconds{ 150 });
    ASSERT_TRUE(midpoint.has_value());
    EXPECT_NEAR(midpoint->x, 0.0, 1e-9);
}

TEST(PlayerPresentationTest, SamplingDoesNotDependOnPriorRenderFrames)
{
    static constexpr shared::Player INITIAL{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .ch = '@',
    };
    static constexpr shared::Player TARGET{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .x_subcell = 4'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation sampled_each_frame;
    client::PlayerPresentation sampled_once;

    sampled_each_frame.update(INITIAL, started_at);
    sampled_once.update(INITIAL, started_at);
    sampled_each_frame.update(TARGET, started_at + std::chrono::milliseconds{ 100 });
    sampled_once.update(TARGET, started_at + std::chrono::milliseconds{ 100 });
    static_cast<void>(sampled_each_frame.sample('@', started_at + std::chrono::milliseconds{ 125 }));
    static_cast<void>(sampled_each_frame.sample('@', started_at + std::chrono::milliseconds{ 150 }));
    static_cast<void>(sampled_each_frame.sample('@', started_at + std::chrono::milliseconds{ 175 }));

    ASSERT_TRUE(sampled_each_frame.sample('@', started_at + std::chrono::milliseconds{ 175 }).has_value());
    ASSERT_TRUE(sampled_once.sample('@', started_at + std::chrono::milliseconds{ 175 }).has_value());
    EXPECT_DOUBLE_EQ(
        sampled_each_frame.sample('@', started_at + std::chrono::milliseconds{ 175 })->x,
        sampled_once.sample('@', started_at + std::chrono::milliseconds{ 175 })->x
    );
}

TEST(PlayerPresentationTest, RetargetStartsFromTheCurrentPresentationPosition)
{
    static constexpr shared::Player INITIAL{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .ch = '@',
    };
    static constexpr shared::Player FIRST_TARGET{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .x_subcell = 4'000U,
        .ch = '@',
    };
    static constexpr shared::Player SECOND_TARGET{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .x_subcell = 8'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(INITIAL, started_at);
    presentation.update(FIRST_TARGET, started_at + std::chrono::milliseconds{ 100 });
    presentation.update(SECOND_TARGET, started_at + std::chrono::milliseconds{ 150 });

    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 150 })->x, 0.2);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 175 })->x, 0.35);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 250 })->x, 0.8);
}

TEST(PlayerPresentationTest, PresentationHoldsLastAuthoritativePositionUntilTheNextUpdate)
{
    static constexpr shared::Player INITIAL{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .ch = '@',
    };
    static constexpr shared::Player TARGET{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .x_subcell = 4'000U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(INITIAL, started_at);
    presentation.update(TARGET, started_at + std::chrono::milliseconds{ 100 });

    ASSERT_TRUE(presentation.sample('@', started_at + std::chrono::seconds{ 1 }).has_value());
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::seconds{ 1 })->x, 0.4);
}

TEST(PlayerPresentationTest, RemovalDropsTheCharacterPresentation)
{
    static constexpr shared::Player PLAYER{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .ch = '@',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(PLAYER, started_at);
    presentation.remove('@');

    EXPECT_FALSE(presentation.sample('@', started_at).has_value());
}

TEST(PlayerPresentationTest, SampledPresentationDrivesCameraAndRemotePlayersWhileWorldStaysAuthoritative)
{
    static constexpr shared::Player LOCAL_INITIAL{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .ch = '@',
    };
    static constexpr shared::Player LOCAL_AUTHORITATIVE{
        .id = 1U,
        .x = 0U,
        .y = 0U,
        .x_subcell = 4'000U,
        .ch = '@',
    };
    static constexpr shared::Player REMOTE_INITIAL{
        .id = 2U,
        .x = 10U,
        .y = 0U,
        .ch = '#',
    };
    static constexpr shared::Player REMOTE_AUTHORITATIVE{
        .id = 2U,
        .x = 10U,
        .y = 0U,
        .x_subcell = 4'000U,
        .ch = '#',
    };
    std::chrono::steady_clock::time_point const started_at{};
    client::PlayerPresentation presentation;

    presentation.update(LOCAL_INITIAL, started_at);
    presentation.update(REMOTE_INITIAL, started_at);
    presentation.update(LOCAL_AUTHORITATIVE, started_at + std::chrono::milliseconds{ 100 });
    presentation.update(REMOTE_AUTHORITATIVE, started_at + std::chrono::milliseconds{ 100 });

    ASSERT_TRUE(presentation.sample('@', started_at + std::chrono::milliseconds{ 125 }).has_value());
    ASSERT_TRUE(presentation.sample('#', started_at + std::chrono::milliseconds{ 125 }).has_value());
    EXPECT_DOUBLE_EQ(shared::playerPositionX(LOCAL_AUTHORITATIVE), 0.4);
    EXPECT_DOUBLE_EQ(shared::playerPositionX(REMOTE_AUTHORITATIVE), 10.4);
    EXPECT_DOUBLE_EQ(presentation.sample('@', started_at + std::chrono::milliseconds{ 125 })->x, 0.1);
    EXPECT_DOUBLE_EQ(presentation.sample('#', started_at + std::chrono::milliseconds{ 125 })->x, 10.1);
    EXPECT_DOUBLE_EQ(
        client::localPlayerThirdPersonPose(
            *presentation.sample('@', started_at + std::chrono::milliseconds{ 125 }),
            { }
        ).position.x,
        1.1
    );
}

} // namespace
