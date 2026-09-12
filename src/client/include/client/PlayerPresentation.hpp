#pragma once

#include "Camera.hpp"

#include <shared/world/World.hpp>

#include <glm/vec3.hpp>

#include <chrono>
#include <optional>
#include <vector>

namespace client {

struct PlayerPresentationPosition final {
    double x;
    double y;
};

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
        std::chrono::steady_clock::time_point started_at;
    };

    [[nodiscard]]
    static PlayerPresentationPosition position(shared::Player const& player) noexcept;
    [[nodiscard]]
    static PlayerPresentationPosition sample(
        Sample const& sample,
        std::chrono::steady_clock::time_point now
    ) noexcept;

    std::vector<Sample> m_samples;
};

[[nodiscard]]
glm::dvec3 localPlayerCenterPosition(shared::Player const& player) noexcept;

[[nodiscard]]
glm::dvec3 localPlayerCenterPosition(PlayerPresentationPosition position) noexcept;

[[nodiscard]]
CameraPose localPlayerThirdPersonPose(
    shared::Player const& player,
    CameraAngles angles
) noexcept;

[[nodiscard]]
CameraPose localPlayerThirdPersonPose(
    PlayerPresentationPosition position,
    CameraAngles angles
) noexcept;

[[nodiscard]]
bool shouldRenderRemotePlayer(shared::Player const& player, char local_character) noexcept;

} // namespace client
