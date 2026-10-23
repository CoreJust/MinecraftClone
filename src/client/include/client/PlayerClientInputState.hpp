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

    void observeJump(bool const pressed) noexcept
    {
        m_jump_pending = m_jump_pending || pressed;
    }

    [[nodiscard]] bool cycleRequested() const noexcept { return m_pending_cycles != 0U; }
    [[nodiscard]] uint64_t pendingCycles() const noexcept { return m_pending_cycles; }
    [[nodiscard]] bool jumpRequested(bool const held) const noexcept { return m_jump_pending || held; }

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
    }

  private:
    bool m_cycle_held = false;
    uint64_t m_pending_cycles = 0U;
    bool m_jump_pending = false;
};

} // namespace client
