#pragma once

#include "AndroidInputState.hpp"

#include <client/render/DebugHud.hpp>

#include <cmath>
#include <cstdint>
#include <optional>

struct AInputEvent;

namespace game_android {

class AndroidFlightTouchControls final {
public:
    [[nodiscard]]
    static std::optional<int8_t> direction(
        float const x,
        float const y,
        uint32_t const surface_width,
        uint32_t const surface_height
    ) noexcept
    {
        static constexpr float FLIGHT_CONTROL_EDGE_FRACTION = 0.8F;
        static constexpr float FLIGHT_CONTROL_BAND_FRACTION = 0.25F;
        if (
            surface_width == 0U
            || surface_height == 0U
            || !std::isfinite(x)
            || !std::isfinite(y)
            || x < static_cast<float>(surface_width) * FLIGHT_CONTROL_EDGE_FRACTION
            || x >= static_cast<float>(surface_width)
            || y < 0.0F
            || y >= static_cast<float>(surface_height)
        ) {
            return std::nullopt;
        }
        if (y < static_cast<float>(surface_height) * FLIGHT_CONTROL_BAND_FRACTION) {
            return 1;
        }
        if (y >= static_cast<float>(surface_height) * (1.0F - FLIGHT_CONTROL_BAND_FRACTION)) {
            return -1;
        }
        return std::nullopt;
    }
};

class AndroidInput final {
public:
    void setSurfaceWidth(uint32_t width) noexcept;
    void setSurfaceHeight(uint32_t height) noexcept;
    void setDensity(float density) noexcept;
    void clear() noexcept;

    [[nodiscard]]
    int32_t handle(AInputEvent const* event) noexcept;
    [[nodiscard]]
    shared::Direction direction() const noexcept;
    [[nodiscard]]
    int8_t flightDirection() const noexcept;
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
    [[nodiscard]]
    bool beginFlightTouch(int32_t pointer_id, float x, float y) noexcept;
    [[nodiscard]]
    bool isFlightTouch(int32_t pointer_id) const noexcept;
    [[nodiscard]]
    bool endFlightTouch(int32_t pointer_id) noexcept;
    void cancelFlightTouches() noexcept;
private:
    uint32_t m_surface_width = 0;
    uint32_t m_surface_height = 0;
    AndroidInputState m_state;

    bool m_reload_pressed = false;
    client::DebugHudToggleLatch m_debug_hud_toggle;
    bool m_stop_requested = false;
    bool m_reload_requested = false;
    bool m_debug_hud_toggle_requested = false;
    int32_t m_ascending_touch_pointer_id = -1;
    int32_t m_descending_touch_pointer_id = -1;
};

} // namespace game_android
