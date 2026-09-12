#include <client/PlayerPresentation.hpp>

#include <cmath>

namespace client {

namespace {

constexpr double PLAYER_CENTER_OFFSET = 1.0;
constexpr double PLAYER_CENTER_HEIGHT = 1.0;
constexpr double THIRD_PERSON_DISTANCE = 6.0;
constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;

} // namespace

glm::dvec3 localPlayerCenterPosition(shared::Player const& player) noexcept
{
    return {
        shared::playerPositionX(player) + PLAYER_CENTER_OFFSET,
        shared::playerPositionY(player) + PLAYER_CENTER_OFFSET,
        PLAYER_CENTER_HEIGHT,
    };
}

CameraPose localPlayerThirdPersonPose(
    shared::Player const& player,
    CameraAngles const angles
) noexcept
{
    double const yaw = angles.yaw_degrees * DEGREES_TO_RADIANS;
    double const pitch = angles.pitch_degrees * DEGREES_TO_RADIANS;
    double const cosine_pitch = std::cos(pitch);
    glm::dvec3 const forward{
        std::sin(yaw) * cosine_pitch,
        std::cos(yaw) * cosine_pitch,
        std::sin(pitch),
    };
    return {
        .position = localPlayerCenterPosition(player) - forward * THIRD_PERSON_DISTANCE,
        .angles = angles,
    };
}

bool shouldRenderRemotePlayer(shared::Player const& player, char const local_character) noexcept
{
    return player.ch != local_character;
}

} // namespace client
