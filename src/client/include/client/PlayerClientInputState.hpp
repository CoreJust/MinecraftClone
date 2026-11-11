#pragma once

#include <shared/world/World.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace client {

class PlayerClientInputState final {
  public:
    void observeCycle(bool const pressed)
    {
        if (pressed && !m_cycle_held) {
            if (m_pending_cycles == std::numeric_limits<uint64_t>::max()) {
                throw std::overflow_error("Too many pending movement capability changes");
            }
            ++m_pending_cycles;
        }
        m_cycle_held = pressed;
    }

    void observeJump(bool const pressed, bool const descending = false) noexcept
    {
        if (descending) {
            m_jump_pending = false;
        } else {
            m_jump_pending = m_jump_pending || pressed;
        }
    }

    [[nodiscard]] bool cycleRequested() const noexcept { return m_pending_cycles != 0U; }
    [[nodiscard]] uint64_t pendingCycles() const noexcept { return m_pending_cycles; }
    [[nodiscard]] bool jumpRequested(bool const held) const noexcept { return m_jump_pending || held; }

    [[nodiscard]] bool observeBreakTarget(bool const held, bool const target_available) noexcept
    {
        if (!held) {
            m_break_target_lost = false;
            return false;
        }
        bool const resumed = m_break_target_lost && target_available;
        m_break_target_lost = !target_available;
        return resumed;
    }

    void cancelBreakTargetIntent() noexcept
    {
        m_break_target_lost = false;
    }

    void submitted(shared::Direction const& direction) noexcept
    {
        if (direction.cycle_movement_capabilities && m_pending_cycles != 0U) {
            --m_pending_cycles;
        }
        if (static_cast<int8_t>(direction.z) > 0) {
            m_jump_pending = false;
        }
    }

    void reset() noexcept
    {
        m_cycle_held = false;
        m_pending_cycles = 0U;
        m_jump_pending = false;
        m_break_target_lost = false;
    }

  private:
    bool m_cycle_held = false;
    uint64_t m_pending_cycles = 0U;
    bool m_jump_pending = false;
    bool m_break_target_lost = false;
};

enum class PlayerClientExitInputAction : uint8_t
{
    None,
    CloseMap,
    OpenConfirmation,
    CancelConfirmation,
    ConfirmExit,
};

class PlayerClientExitConfirmationState final
{
  public:
    [[nodiscard]] PlayerClientExitInputAction observe(
        bool const map_open,
        bool const escape_edge,
        bool const confirm_edge
    ) noexcept
    {
        if (map_open) {
            return escape_edge ? PlayerClientExitInputAction::CloseMap : PlayerClientExitInputAction::None;
        }
        if (m_pending) {
            if (confirm_edge) {
                m_pending = false;
                return PlayerClientExitInputAction::ConfirmExit;
            }
            if (escape_edge) {
                m_pending = false;
                return PlayerClientExitInputAction::CancelConfirmation;
            }
            return PlayerClientExitInputAction::None;
        }
        if (escape_edge) {
            m_pending = true;
            return PlayerClientExitInputAction::OpenConfirmation;
        }
        return PlayerClientExitInputAction::None;
    }

    void request() noexcept { m_pending = true; }
    void cancel() noexcept { m_pending = false; }
    [[nodiscard]] bool pending() const noexcept { return m_pending; }

  private:
    bool m_pending = false;
};

} // namespace client
