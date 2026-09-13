#include <client/PlayerPresentation.hpp>

#include <algorithm>
#include <cmath>

namespace client {

namespace {

constexpr double PLAYER_CENTER_OFFSET = 1.0;
constexpr double PLAYER_CENTER_HEIGHT = 1.0;
constexpr double THIRD_PERSON_DISTANCE = 6.0;
constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;

} // namespace

void PlayerPresentation::update(
    shared::Player const& player,
    std::chrono::steady_clock::time_point const received_at
) noexcept
{
    PlayerPresentationPosition const target = position(player);
    for (Sample& sample : m_samples) {
        if (sample.character == player.ch) {
            if (sample.to == target) {
                return;
            }
            sample = {
                .character = player.ch,
                .from = PlayerPresentation::sample(sample, received_at),
                .to = target,
                .started_at = received_at,
            };
            return;
        }
    }
    m_samples.push_back({
        .character = player.ch,
        .from = target,
        .to = target,
        .started_at = received_at,
    });
}

void PlayerPresentation::remove(char const character) noexcept
{
    auto const found = std::find_if(m_samples.begin(), m_samples.end(), [character](Sample const& sample) {
        return sample.character == character;
    });
    if (found != m_samples.end()) {
        m_samples.erase(found);
    }
}

std::optional<PlayerPresentationPosition> PlayerPresentation::sample(
    char const character,
    std::chrono::steady_clock::time_point const now
) const noexcept
{
    for (Sample const& sample : m_samples) {
        if (sample.character == character) {
            return PlayerPresentation::sample(sample, now);
        }
    }
    return std::nullopt;
}

PlayerPresentationPosition PlayerPresentation::position(shared::Player const& player) noexcept
{
    return {
        .x = shared::playerPositionX(player),
        .y = shared::playerPositionY(player),
    };
}

PlayerPresentationPosition PlayerPresentation::sample(
    Sample const& sample,
    std::chrono::steady_clock::time_point const now
) noexcept
{
    if (now <= sample.started_at) {
        return sample.from;
    }
    std::chrono::steady_clock::duration const elapsed = now - sample.started_at;
    double const alpha = std::min(
        1.0,
        static_cast<double>(elapsed.count())
            / static_cast<double>(std::chrono::duration_cast<std::chrono::steady_clock::duration>(shared::TICK).count())
    );
    return {
        .x = sample.from.x + (sample.to.x - sample.from.x) * alpha,
        .y = sample.from.y + (sample.to.y - sample.from.y) * alpha,
    };
}

glm::dvec3 localPlayerCenterPosition(PlayerPresentationPosition const position) noexcept
{
    return {
        position.x + PLAYER_CENTER_OFFSET,
        position.y + PLAYER_CENTER_OFFSET,
        PLAYER_CENTER_HEIGHT,
    };
}

glm::dvec3 localPlayerCenterPosition(shared::Player const& player) noexcept
{
    return localPlayerCenterPosition(PlayerPresentationPosition{
        .x = shared::playerPositionX(player),
        .y = shared::playerPositionY(player),
    });
}

CameraPose localPlayerThirdPersonPose(
    PlayerPresentationPosition const position,
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
        .position = localPlayerCenterPosition(position) - forward * THIRD_PERSON_DISTANCE,
        .angles = angles,
    };
}

CameraPose localPlayerThirdPersonPose(
    shared::Player const& player,
    CameraAngles const angles
) noexcept
{
    return localPlayerThirdPersonPose(
        PlayerPresentationPosition{
            .x = shared::playerPositionX(player),
            .y = shared::playerPositionY(player),
        },
        angles
    );
}

bool shouldRenderRemotePlayer(shared::Player const& player, char const local_character) noexcept
{
    return player.ch != local_character;
}

} // namespace client
