#pragma once

#include <shared/net/Message.hpp>
#include <shared/world/BlockInteraction.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace client::detail {

struct BlockPlaceInputState final {
    bool active = false;
    std::chrono::steady_clock::time_point next_placement{};
};

[[nodiscard]] bool updateBlockPlaceInput(
    BlockPlaceInputState& state,
    bool held,
    bool fresh_press,
    bool permitted,
    std::chrono::steady_clock::time_point now
) noexcept;

struct BlockBreakInputState final {
    bool active = false;
    std::optional<shared::WorldCoordinate> target;
    std::chrono::steady_clock::time_point next_renewal{};
};

struct BlockBreakInputSample final {
    bool held = false;
    bool fresh_press = false;
    bool permitted = false;
    std::optional<shared::WorldCoordinate> target;
    std::chrono::steady_clock::time_point now{};
};

struct BlockBreakInputActions final {
    bool submit_hold = false;
    bool submit_release = false;
};

[[nodiscard]] BlockBreakInputActions updateBlockBreakInput(
    BlockBreakInputState& state,
    BlockBreakInputSample const& input
) noexcept;

[[nodiscard]] std::optional<shared::Block> nextPlaceableBlock(
    uint8_t allowed_materials,
    shared::Block selected
) noexcept;

[[nodiscard]] float authoritativeBreakProgress(
    shared::ServerBlockActionResultMessage const& status
) noexcept;

[[nodiscard]] constexpr bool interactionActionAllowed(
    uint8_t bits,
    shared::InteractionAction action
) noexcept
{
    return (bits & static_cast<uint8_t>(1U << static_cast<uint8_t>(action))) != 0U;
}

[[nodiscard]] constexpr bool interactionMaterialAllowed(uint8_t bits, shared::Block block) noexcept
{
    uint8_t const value = static_cast<uint8_t>(block);
    return value < 8U && (bits & static_cast<uint8_t>(1U << value)) != 0U;
}

[[nodiscard]] bool hasCustomBlockActionGrants(
    shared::ServerBlockActionResultMessage const& status
);

} // namespace client::detail
