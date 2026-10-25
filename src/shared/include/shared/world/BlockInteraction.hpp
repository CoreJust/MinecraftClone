#pragma once

#include <shared/world/SparseWorld.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace shared {

enum class GameMode : uint8_t {
    Spectator = 0,
    Creative = 1,
    Survival = 2,
};

[[nodiscard]]
constexpr bool isValidGameMode(GameMode mode) noexcept
{
    switch (mode) {
    case GameMode::Spectator:
    case GameMode::Creative:
    case GameMode::Survival:
        return true;
    }
    return false;
}

enum class InteractionPerspective : uint8_t {
    FirstPerson = 0,
    ThirdPersonRear = 1,
    ThirdPersonFront = 2,
};

[[nodiscard]]
constexpr bool isValidInteractionPerspective(InteractionPerspective perspective) noexcept
{
    switch (perspective) {
    case InteractionPerspective::FirstPerson:
    case InteractionPerspective::ThirdPersonRear:
    case InteractionPerspective::ThirdPersonFront:
        return true;
    }
    return false;
}

inline constexpr double MAX_INTERACTION_CAMERA_DISTANCE = 6.0;
inline constexpr double DEFAULT_BLOCK_REACH = 6.0;
inline constexpr double INTERACTION_CAMERA_OBSTRUCTION_MARGIN = 0.03;
inline constexpr double INTERACTION_PLAYER_EYE_HEIGHT = 1.625;
inline constexpr double INTERACTION_WORLD_CURVATURE_RADIUS = 131'072.0;

enum class InteractionAction : uint8_t {
    Break,
    Place,
    Flight,
    CollisionBypass,
    QuickBreak,
    ChangeMode,
    Count,
};

inline constexpr uint8_t INTERACTION_ACTION_COUNT = static_cast<uint8_t>(InteractionAction::Count);

struct InteractionActionGrants final {
    uint8_t bits = 0U;

    [[nodiscard]]
    bool allows(InteractionAction action) const noexcept;

    void set(InteractionAction action, bool enabled = true) noexcept;
};

struct InteractionActionOverrides final {
    std::array<std::optional<bool>, INTERACTION_ACTION_COUNT> values{};

    void set(InteractionAction action, bool enabled) noexcept;
};

struct MaterialFilter final {
    // No whitelist means unrestricted; an explicitly empty whitelist denies every material.
    std::optional<std::vector<Block>> whitelist;
    std::vector<Block> blacklist;

    [[nodiscard]]
    bool allows(Block material) const noexcept;
};

struct InteractionPolicy final {
    InteractionActionOverrides soft_actions{};
    InteractionActionGrants hard_vetoes{};
    MaterialFilter break_materials{};
    MaterialFilter place_materials{};
};

struct EffectiveInteractionRights final {
    GameMode mode = GameMode::Spectator;
    InteractionActionGrants actions{};
    MaterialFilter break_materials{};
    MaterialFilter place_materials{};

    [[nodiscard]]
    bool allows(InteractionAction action) const noexcept;
    [[nodiscard]]
    bool canBreak(Block material) const noexcept;
    [[nodiscard]]
    bool canPlace(Block material) const noexcept;
};

[[nodiscard]]
EffectiveInteractionRights effectiveInteractionRights(
    GameMode mode,
    InteractionPolicy const& policy = {}
);

struct BreakTiming final {
    // Count complete 100 ms intervals after the authorized start tick. Quick break completes
    // when checked on the first authorized server tick and earns no timed interval.
    uint16_t required_full_intervals_after_start = 0U;
    bool quick = false;
};

[[nodiscard]]
std::optional<BreakTiming> breakTiming(EffectiveInteractionRights const& rights, Block material) noexcept;

[[nodiscard]]
bool isBreakComplete(BreakTiming timing, uint16_t full_intervals_after_start) noexcept;

enum class BlockFace : uint8_t {
    None,
    NegativeX,
    PositiveX,
    NegativeY,
    PositiveY,
    NegativeZ,
    PositiveZ,
};

struct VoxelRay final {
    double origin_x = 0.0;
    double origin_y = 0.0;
    double origin_z = 0.0;
    double direction_x = 0.0;
    double direction_y = 0.0;
    double direction_z = 0.0;
};

enum class BlockTargetStatus : uint8_t {
    Hit,
    Miss,
    Unknown,
    Preview,
    OutOfBounds,
    InvalidRay,
};

struct BlockTarget final {
    WorldCoordinate cell{};
    Block block = Block::Air;
    BlockFace entered_face = BlockFace::None;
    double distance = 0.0;
};

struct BlockTargetResult final {
    BlockTargetStatus status = BlockTargetStatus::Miss;
    std::optional<BlockTarget> target;
};

// `curved_world` applies the renderer's camera-centered vertical curvature to the ray.
[[nodiscard]]
BlockTargetResult traceBlockTarget(
    SparseWorld const& world,
    VoxelRay const& ray,
    double max_distance = DEFAULT_BLOCK_REACH,
    bool curved_world = false
) noexcept;

[[nodiscard]]
std::optional<std::array<double, 3>> interactionLookDirection(
    double yaw_degrees,
    double pitch_degrees
) noexcept;

[[nodiscard]]
std::optional<VoxelRay> reconstructInteractionCameraRay(
    VoxelRay eye_ray,
    InteractionPerspective perspective,
    double maximum_unobstructed_distance = MAX_INTERACTION_CAMERA_DISTANCE
) noexcept;

// Reconstructs the visible center ray, then enforces eye reach and line of sight.
[[nodiscard]]
BlockTargetResult resolveInteractionTarget(
    SparseWorld const& world,
    VoxelRay const& accepted_eye_ray,
    InteractionPerspective perspective,
    bool curved_world = false
) noexcept;

} // namespace shared
