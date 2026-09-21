#pragma once

#include "Camera.hpp"

#include <shared/world/World.hpp>

#include <glm/vec3.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <vector>

namespace client {

struct PlayerPresentationPosition final {
    double x;
    double y;
    double z;

    bool operator==(PlayerPresentationPosition const&) const noexcept = default;
};

enum class CameraPerspective : uint8_t {
    FirstPerson,
    ThirdPersonRear,
    ThirdPersonFront,
};

struct PlayerCameraView final {
    CameraPose pose;
    bool renders_local_body = false;
};

constexpr double MAX_LOCAL_PLAYER_CAMERA_DISTANCE = 6.0;
inline constexpr glm::vec3 PLAYER_BODY_DIMENSIONS{
    static_cast<float>(shared::World::PLAYER_WIDTH_SUBCELLS) / static_cast<float>(shared::SUBCELLS_PER_CELL),
    static_cast<float>(shared::World::PLAYER_WIDTH_SUBCELLS) / static_cast<float>(shared::SUBCELLS_PER_CELL),
    static_cast<float>(shared::World::PLAYER_HEIGHT_SUBCELLS) / static_cast<float>(shared::SUBCELLS_PER_CELL),
};
inline constexpr double PLAYER_EYE_HEIGHT = 1.625;

class PlayerPresentation final {
public:
    void update(shared::Player const& player, std::chrono::steady_clock::time_point received_at) noexcept;
    void remove(char character) noexcept;

    [[nodiscard]]
    std::optional<PlayerPresentationPosition> sample(
        char character,
        std::chrono::steady_clock::time_point now
    ) const noexcept;

private:
    struct Sample final {
        char character;
        PlayerPresentationPosition from;
        PlayerPresentationPosition to;
        double from_vertical_velocity = 0.0;
        double to_vertical_velocity = 0.0;
        bool has_transition = false;
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point updated_at;
    };

    [[nodiscard]]
    static PlayerPresentationPosition position(shared::Player const& player) noexcept;
    [[nodiscard]]
    static PlayerPresentationPosition sample(
        Sample const& sample,
        std::chrono::steady_clock::time_point now
    ) noexcept;
    [[nodiscard]]
    static double verticalVelocity(
        Sample const& sample,
        std::chrono::steady_clock::time_point now
    ) noexcept;

    std::vector<Sample> m_samples;
};

[[nodiscard]]
glm::dvec3 localPlayerCenterPosition(PlayerPresentationPosition position) noexcept;

[[nodiscard]]
glm::dvec3 localPlayerCenterPosition(shared::Player const& player) noexcept;

[[nodiscard]]
glm::dvec3 localPlayerEyePosition(PlayerPresentationPosition position) noexcept;

[[nodiscard]]
glm::dvec3 localPlayerEyePosition(shared::Player const& player) noexcept;

[[nodiscard]]
CameraPose localPlayerFirstPersonPose(
    PlayerPresentationPosition position,
    CameraAngles angles
) noexcept;

[[nodiscard]]
CameraPose localPlayerFirstPersonPose(
    shared::Player const& player,
    CameraAngles angles
) noexcept;

[[nodiscard]]
CameraPose localPlayerThirdPersonPose(
    PlayerPresentationPosition position,
    CameraAngles angles
) noexcept;

[[nodiscard]]
PlayerCameraView resolveLocalPlayerCamera(
    PlayerPresentationPosition position,
    CameraAngles look_angles,
    CameraPerspective perspective,
    double maximum_unobstructed_distance
) noexcept;

[[nodiscard]]
PlayerCameraView resolveLocalPlayerCamera(
    shared::Player const& player,
    CameraAngles look_angles,
    CameraPerspective perspective,
    double maximum_unobstructed_distance
) noexcept;

[[nodiscard]]
CameraPerspective nextCameraPerspective(CameraPerspective perspective) noexcept;

[[nodiscard]]
bool shouldRenderPlayerBody(
    shared::Player const& player,
    char local_character,
    CameraPerspective perspective
) noexcept;

[[nodiscard]]
std::array<float, 4> playerPaletteColor(shared::PlayerPaletteIndex palette_index) noexcept;

[[nodiscard]]
CameraPose localPlayerThirdPersonPose(
    shared::Player const& player,
    CameraAngles angles
) noexcept;

[[nodiscard]]
bool shouldRenderRemotePlayer(shared::Player const& player, char local_character) noexcept;

} // namespace client
