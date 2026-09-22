#include <client/PlayerPresentation.hpp>

#include <shared/world/SparseWorld.hpp>

#include <algorithm>
#include <cmath>

namespace client {

namespace {

constexpr double PLAYER_BODY_CENTER_OFFSET = static_cast<double>(shared::World::PLAYER_WIDTH_SUBCELLS)
    / static_cast<double>(shared::SUBCELLS_PER_CELL) / 2.0;
constexpr double PLAYER_BODY_CENTER_HEIGHT = static_cast<double>(shared::World::PLAYER_HEIGHT_SUBCELLS)
    / static_cast<double>(shared::SUBCELLS_PER_CELL) / 2.0;
constexpr double THIRD_PERSON_DISTANCE = MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;
constexpr double WORLD_WRAP_PERIOD = static_cast<double>(shared::WorldExtent::WIDTH);
constexpr double PRESENTATION_TICK_SECONDS = 0.1;

constexpr std::array<std::array<float, 4>, shared::PLAYER_PALETTE_COUNT> PLAYER_PALETTE{{
    { 0.91F, 0.24F, 0.24F, 1.0F }, { 0.95F, 0.53F, 0.17F, 1.0F },
    { 0.94F, 0.82F, 0.22F, 1.0F }, { 0.55F, 0.80F, 0.24F, 1.0F },
    { 0.18F, 0.75F, 0.42F, 1.0F }, { 0.15F, 0.77F, 0.70F, 1.0F },
    { 0.19F, 0.62F, 0.92F, 1.0F }, { 0.32F, 0.45F, 0.93F, 1.0F },
    { 0.53F, 0.34F, 0.91F, 1.0F }, { 0.75F, 0.30F, 0.87F, 1.0F },
    { 0.91F, 0.27F, 0.64F, 1.0F }, { 0.87F, 0.34F, 0.43F, 1.0F },
    { 0.55F, 0.30F, 0.18F, 1.0F }, { 0.45F, 0.48F, 0.55F, 1.0F },
    { 0.75F, 0.78F, 0.82F, 1.0F }, { 0.94F, 0.94F, 0.94F, 1.0F },
}};

[[nodiscard]] glm::dvec3 forward(CameraAngles const angles) noexcept
{
    double const yaw = angles.yaw_degrees * DEGREES_TO_RADIANS;
    double const pitch = angles.pitch_degrees * DEGREES_TO_RADIANS;
    double const cosine_pitch = std::cos(pitch);
    return {
        std::sin(yaw) * cosine_pitch,
        std::cos(yaw) * cosine_pitch,
        std::sin(pitch),
    };
}

[[nodiscard]] double clippedThirdPersonDistance(double const maximum_unobstructed_distance) noexcept
{
    if (!std::isfinite(maximum_unobstructed_distance) || maximum_unobstructed_distance <= 0.0) {
        return 0.0;
    }
    return std::min(THIRD_PERSON_DISTANCE, maximum_unobstructed_distance);
}

[[nodiscard]] double interpolateWrappedHorizontal(
    double const from,
    double const to,
    double const alpha
) noexcept
{
    double delta = to - from;
    if (delta > WORLD_WRAP_PERIOD * 0.5) {
        delta -= WORLD_WRAP_PERIOD;
    } else if (delta < -WORLD_WRAP_PERIOD * 0.5) {
        delta += WORLD_WRAP_PERIOD;
    }
    double result = std::fmod(from + delta * alpha, WORLD_WRAP_PERIOD);
    if (result < 0.0) {
        result += WORLD_WRAP_PERIOD;
    }
    return result;
}

[[nodiscard]] double simulationVerticalVelocity(shared::Player const& player) noexcept
{
    if (player.movement_capabilities.allows(shared::MovementCapability::Flight)) {
        return 0.0;
    }
    return static_cast<double>(player.vertical_velocity_subcells)
        / static_cast<double>(shared::SUBCELLS_PER_CELL)
        / PRESENTATION_TICK_SECONDS;
}

} // namespace

void PlayerPresentation::update(
    shared::Player const& player,
    std::chrono::steady_clock::time_point const received_at
) noexcept
{
    PlayerPresentationPosition const target = position(player);
    double const target_vertical_velocity = simulationVerticalVelocity(player);
    for (Sample& sample : m_samples) {
        if (sample.character == player.ch) {
            if (sample.to == target) {
                if (sample.to_vertical_velocity == target_vertical_velocity) {
                    return;
                }
                if (!sample.has_transition || received_at >= sample.started_at + shared::TICK) {
                    sample.from = target;
                    sample.to = target;
                    sample.from_vertical_velocity = target_vertical_velocity;
                    sample.to_vertical_velocity = target_vertical_velocity;
                    sample.has_transition = false;
                    sample.started_at = received_at;
                    return;
                }
                PlayerPresentationPosition const current = PlayerPresentation::sample(sample, received_at);
                double const current_vertical_velocity = verticalVelocity(sample, received_at);
                sample = {
                    .character = player.ch,
                    .from = current,
                    .to = target,
                    .from_vertical_velocity = current_vertical_velocity,
                    .to_vertical_velocity = target_vertical_velocity,
                    .has_transition = true,
                    .started_at = received_at,
                };
                return;
            }
            double const from_vertical_velocity = sample.has_transition
                ? verticalVelocity(sample, received_at)
                : sample.to_vertical_velocity;
            sample = {
                .character = player.ch,
                .from = PlayerPresentation::sample(sample, received_at),
                .to = target,
                .from_vertical_velocity = from_vertical_velocity,
                .to_vertical_velocity = target_vertical_velocity,
                .has_transition = true,
                .started_at = received_at,
            };
            return;
        }
    }
    m_samples.push_back({
        .character = player.ch,
        .from = target,
        .to = target,
        .to_vertical_velocity = target_vertical_velocity,
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
        .z = shared::playerPositionZ(player),
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
    double const h00 = 2.0 * alpha * alpha * alpha - 3.0 * alpha * alpha + 1.0;
    double const h10 = alpha * alpha * alpha - 2.0 * alpha * alpha + alpha;
    double const h01 = -2.0 * alpha * alpha * alpha + 3.0 * alpha * alpha;
    double const h11 = alpha * alpha * alpha - alpha * alpha;
    return {
        .x = interpolateWrappedHorizontal(sample.from.x, sample.to.x, alpha),
        .y = interpolateWrappedHorizontal(sample.from.y, sample.to.y, alpha),
        .z = h00 * sample.from.z
            + h10 * sample.from_vertical_velocity * PRESENTATION_TICK_SECONDS
            + h01 * sample.to.z
            + h11 * sample.to_vertical_velocity * PRESENTATION_TICK_SECONDS,
    };
}

double PlayerPresentation::verticalVelocity(
    Sample const& sample,
    std::chrono::steady_clock::time_point const now
) noexcept
{
    if (!sample.has_transition) {
        return 0.0;
    }
    if (now <= sample.started_at) {
        return sample.from_vertical_velocity;
    }
    double const alpha = std::min(
        1.0,
        std::chrono::duration<double>(now - sample.started_at).count() / PRESENTATION_TICK_SECONDS
    );
    double const dh00 = 6.0 * alpha * alpha - 6.0 * alpha;
    double const dh10 = 3.0 * alpha * alpha - 4.0 * alpha + 1.0;
    double const dh01 = -6.0 * alpha * alpha + 6.0 * alpha;
    double const dh11 = 3.0 * alpha * alpha - 2.0 * alpha;
    return (
        dh00 * sample.from.z
        + dh10 * sample.from_vertical_velocity * PRESENTATION_TICK_SECONDS
        + dh01 * sample.to.z
        + dh11 * sample.to_vertical_velocity * PRESENTATION_TICK_SECONDS
    ) / PRESENTATION_TICK_SECONDS;
}

glm::dvec3 localPlayerCenterPosition(PlayerPresentationPosition const position) noexcept
{
    return {
        position.x + PLAYER_BODY_CENTER_OFFSET,
        position.y + PLAYER_BODY_CENTER_OFFSET,
        position.z + PLAYER_BODY_CENTER_HEIGHT,
    };
}

glm::dvec3 localPlayerCenterPosition(shared::Player const& player) noexcept
{
    return localPlayerCenterPosition(PlayerPresentationPosition{
        .x = shared::playerPositionX(player),
        .y = shared::playerPositionY(player),
        .z = shared::playerPositionZ(player),
    });
}

glm::dvec3 localPlayerEyePosition(PlayerPresentationPosition const position) noexcept
{
    return {
        position.x + PLAYER_BODY_CENTER_OFFSET,
        position.y + PLAYER_BODY_CENTER_OFFSET,
        position.z + PLAYER_EYE_HEIGHT,
    };
}

glm::dvec3 localPlayerEyePosition(shared::Player const& player) noexcept
{
    return localPlayerEyePosition(PlayerPresentationPosition{
        .x = shared::playerPositionX(player),
        .y = shared::playerPositionY(player),
        .z = shared::playerPositionZ(player),
    });
}

CameraPose localPlayerFirstPersonPose(
    PlayerPresentationPosition const position,
    CameraAngles const angles
) noexcept
{
    return {
        .position = localPlayerEyePosition(position),
        .angles = angles,
    };
}

CameraPose localPlayerFirstPersonPose(
    shared::Player const& player,
    CameraAngles const angles
) noexcept
{
    return localPlayerFirstPersonPose(
        PlayerPresentationPosition{
            .x = shared::playerPositionX(player),
            .y = shared::playerPositionY(player),
            .z = shared::playerPositionZ(player),
        },
        angles
    );
}

CameraPose localPlayerThirdPersonPose(
    PlayerPresentationPosition const position,
    CameraAngles const angles
) noexcept
{
    return {
        .position = localPlayerEyePosition(position) - forward(angles) * THIRD_PERSON_DISTANCE,
        .angles = angles,
    };
}

PlayerCameraView resolveLocalPlayerCamera(
    PlayerPresentationPosition const position,
    CameraAngles const look_angles,
    CameraPerspective const perspective,
    double const maximum_unobstructed_distance
) noexcept
{
    glm::dvec3 const eye = localPlayerEyePosition(position);
    double const distance = clippedThirdPersonDistance(maximum_unobstructed_distance);
    switch (perspective) {
    case CameraPerspective::FirstPerson:
        return { .pose = { .position = eye, .angles = look_angles }, .renders_local_body = false };
    case CameraPerspective::ThirdPersonRear:
        return {
            .pose = { .position = eye - forward(look_angles) * distance, .angles = look_angles },
            .renders_local_body = true,
        };
    case CameraPerspective::ThirdPersonFront:
        return {
            .pose = {
                .position = eye + forward(look_angles) * distance,
                .angles = {
                    .yaw_degrees = look_angles.yaw_degrees + 180.0,
                    .pitch_degrees = -look_angles.pitch_degrees,
                    .roll_degrees = -look_angles.roll_degrees,
                },
            },
            .renders_local_body = true,
        };
    }
    return { .pose = { .position = eye, .angles = look_angles }, .renders_local_body = false };
}

PlayerCameraView resolveLocalPlayerCamera(
    shared::Player const& player,
    CameraAngles const look_angles,
    CameraPerspective const perspective,
    double const maximum_unobstructed_distance
) noexcept
{
    return resolveLocalPlayerCamera(
        PlayerPresentationPosition{
            .x = shared::playerPositionX(player),
            .y = shared::playerPositionY(player),
            .z = shared::playerPositionZ(player),
        },
        look_angles,
        perspective,
        maximum_unobstructed_distance
    );
}

CameraPerspective nextCameraPerspective(CameraPerspective const perspective) noexcept
{
    switch (perspective) {
    case CameraPerspective::FirstPerson:
        return CameraPerspective::ThirdPersonRear;
    case CameraPerspective::ThirdPersonRear:
        return CameraPerspective::ThirdPersonFront;
    case CameraPerspective::ThirdPersonFront:
        return CameraPerspective::FirstPerson;
    }
    return CameraPerspective::FirstPerson;
}

bool shouldRenderPlayerBody(
    shared::Player const& player,
    char const local_character,
    CameraPerspective const perspective
) noexcept
{
    return player.ch != local_character || perspective != CameraPerspective::FirstPerson;
}

std::array<float, 4> playerPaletteColor(shared::PlayerPaletteIndex const palette_index) noexcept
{
    return PLAYER_PALETTE[shared::isValidPlayerPaletteIndex(palette_index) ? palette_index : 0U];
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
            .z = shared::playerPositionZ(player),
        },
        angles
    );
}

bool shouldRenderRemotePlayer(shared::Player const& player, char const local_character) noexcept
{
    return player.ch != local_character;
}

} // namespace client
