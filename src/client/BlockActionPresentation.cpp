#include <client/BlockActionPresentation.hpp>

#include <algorithm>
#include <array>

namespace client::detail {

bool updateBlockPlaceInput(
    BlockPlaceInputState& state,
    bool const held,
    bool const fresh_press,
    bool const permitted,
    std::chrono::steady_clock::time_point const now
) noexcept
{
    if (!held) {
        state = {};
        return false;
    }
    if (fresh_press) {
        state.active = true;
        state.next_placement = now;
    }
    if (!state.active || !permitted || now < state.next_placement) {
        return false;
    }
    state.next_placement = now + std::chrono::milliseconds{200};
    return true;
}

BlockBreakInputActions updateBlockBreakInput(
    BlockBreakInputState& state,
    BlockBreakInputSample const& input
) noexcept
{
    BlockBreakInputActions actions;
    bool const eligible = input.held && input.permitted && input.target.has_value();
    bool const target_changed = state.active && eligible && state.target != input.target;
    if (state.active && (!eligible || target_changed)) {
        actions.submit_release = true;
        state.active = false;
        state.target.reset();
    }
    if (!eligible) {
        return actions;
    }
    if (!state.active) {
        if (!input.fresh_press && !target_changed) {
            return actions;
        }
        state.active = true;
        state.target = input.target;
        state.next_renewal = input.now + std::chrono::milliseconds{100};
        actions.submit_hold = true;
    } else if (input.now >= state.next_renewal) {
        state.next_renewal = input.now + std::chrono::milliseconds{100};
        actions.submit_hold = true;
    }
    return actions;
}

std::optional<shared::Block> nextPlaceableBlock(
    uint8_t const allowed_materials,
    shared::Block const selected
) noexcept
{
    uint8_t constexpr FIRST_SOLID = static_cast<uint8_t>(shared::Block::Stone);
    uint8_t constexpr LAST_SOLID = static_cast<uint8_t>(shared::Block::Sand);
    uint8_t const selected_value = static_cast<uint8_t>(selected);
    bool const selected_is_allowed = selected_value >= FIRST_SOLID && selected_value <= LAST_SOLID
        && (allowed_materials & static_cast<uint8_t>(1U << selected_value)) != 0U;
    constexpr uint8_t SOLID_COUNT = LAST_SOLID - FIRST_SOLID + 1U;
    uint8_t const start_offset = selected_is_allowed
        ? static_cast<uint8_t>((selected_value - FIRST_SOLID + 1U) % SOLID_COUNT)
        : 0U;
    for (uint8_t offset = 0U; offset < SOLID_COUNT; ++offset) {
        uint8_t const value = static_cast<uint8_t>(FIRST_SOLID + (start_offset + offset) % SOLID_COUNT);
        if ((allowed_materials & static_cast<uint8_t>(1U << value)) != 0U) {
            return static_cast<shared::Block>(value);
        }
    }
    return std::nullopt;
}

float authoritativeBreakProgress(shared::ServerBlockActionResultMessage const& status) noexcept
{
    if (status.status != shared::BlockActionStatus::Progress || status.required_intervals == 0U) {
        return 0.0F;
    }
    return std::clamp(
        static_cast<float>(status.completed_intervals) / static_cast<float>(status.required_intervals),
        0.0F,
        1.0F
    );
}

bool hasCustomBlockActionGrants(shared::ServerBlockActionResultMessage const& status)
{
    if (!shared::isValidGameMode(status.mode)) {
        return false;
    }

    static std::array<shared::EffectiveInteractionRights, 3U> const BASE_RIGHTS = [] {
        return std::array{
            shared::effectiveInteractionRights(shared::GameMode::Spectator),
            shared::effectiveInteractionRights(shared::GameMode::Creative),
            shared::effectiveInteractionRights(shared::GameMode::Survival),
        };
    }();
    shared::EffectiveInteractionRights const& base = BASE_RIGHTS[static_cast<size_t>(status.mode)];
    uint8_t const change_mode_bit = static_cast<uint8_t>(
        1U << static_cast<uint8_t>(shared::InteractionAction::ChangeMode)
    );
    if ((status.effective_actions & static_cast<uint8_t>(~base.actions.bits) & static_cast<uint8_t>(~change_mode_bit))
        != 0U) {
        return true;
    }

    uint8_t base_break_materials = 0U;
    uint8_t base_place_materials = 0U;
    for (shared::Block const material : {
        shared::Block::Stone,
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
        shared::Block::Sand,
    }) {
        uint8_t const bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(material));
        if (base.canBreak(material)) {
            base_break_materials |= bit;
        }
        if (base.canPlace(material)) {
            base_place_materials |= bit;
        }
    }
    bool const break_grant = interactionActionAllowed(status.effective_actions, shared::InteractionAction::Break)
        && (status.break_materials & static_cast<uint8_t>(~base_break_materials)) != 0U;
    bool const place_grant = interactionActionAllowed(status.effective_actions, shared::InteractionAction::Place)
        && (status.place_materials & static_cast<uint8_t>(~base_place_materials)) != 0U;
    return break_grant || place_grant;
}

} // namespace client::detail
