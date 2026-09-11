#pragma once

#include "AndroidInputState.hpp"

#include <client/render/DebugHud.hpp>

#include <cstdint>

struct AInputEvent;

namespace game_android {

class AndroidInput final {
public:
    void setSurfaceWidth(uint32_t width) noexcept;
    void setDensity(float density) noexcept;
    void clear() noexcept;

    [[nodiscard]]
    int32_t handle(AInputEvent const* event) noexcept;
    [[nodiscard]]
    shared::Direction direction() const noexcept;
    [[nodiscard]]
    bool consumeStopRequest() noexcept;
    [[nodiscard]]
    bool consumeReloadRequest() noexcept;
    [[nodiscard]]
    bool consumeDebugHudToggleRequest() noexcept;
    [[nodiscard]]
    bool consumeLookDelta(float& horizontal, float& vertical) noexcept;
private:
    [[nodiscard]]
    int32_t handleKey(AInputEvent const* event) noexcept;
    [[nodiscard]]
    int32_t handleMotion(AInputEvent const* event) noexcept;
private:
    uint32_t m_surface_width = 0;
    AndroidInputState m_state;

    bool m_reload_pressed = false;
    client::DebugHudToggleLatch m_debug_hud_toggle;
    bool m_stop_requested = false;
    bool m_reload_requested = false;
    bool m_debug_hud_toggle_requested = false;
};

} // namespace game_android
