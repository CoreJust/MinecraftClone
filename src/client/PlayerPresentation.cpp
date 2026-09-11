#include <client/PlayerPresentation.hpp>

namespace client {

namespace {

constexpr double PLAYER_CENTER_OFFSET = 1.0;
constexpr double FIRST_PERSON_EYE_HEIGHT = 1.6;

} // namespace

glm::dvec3 localPlayerEyePosition(shared::Player const& player) noexcept
{
    return {
        static_cast<double>(player.x) + PLAYER_CENTER_OFFSET,
        static_cast<double>(player.y) + PLAYER_CENTER_OFFSET,
        FIRST_PERSON_EYE_HEIGHT,
    };
}

bool shouldRenderRemotePlayer(shared::Player const& player, char const local_character) noexcept
{
    return player.ch != local_character;
}

} // namespace client
