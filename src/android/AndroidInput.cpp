#include "AndroidInput.hpp"

#include <android/input.h>

#include <cstddef>

namespace game_android {

void AndroidInput::setSurfaceWidth(uint32_t const width) noexcept
{
    m_surface_width = width;
}

void AndroidInput::setSurfaceHeight(uint32_t const height) noexcept
{
    m_surface_height = height;
}

void AndroidInput::setDensity(float const density) noexcept
{
    m_state.setDensity(density);
}

void AndroidInput::clear() noexcept
{
    m_state.clear();
    m_reload_pressed = false;
    m_reload_requested = false;
    m_debug_hud_toggle.reset();
    m_debug_hud_toggle_requested = false;
    cancelFlightTouches();
}

int32_t AndroidInput::handle(AInputEvent const* const event) noexcept
{
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        return handleKey(event);
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        return handleMotion(event);
    }
    return 0;
}

shared::Direction AndroidInput::direction() const noexcept
{
    return m_state.direction();
}

int8_t AndroidInput::flightDirection() const noexcept
{
    return static_cast<int8_t>(
        static_cast<int8_t>(m_ascending_touch_pointer_id >= 0)
        - static_cast<int8_t>(m_descending_touch_pointer_id >= 0)
    );
}

bool AndroidInput::consumeStopRequest() noexcept
{
    bool const requested = m_stop_requested;
    m_stop_requested = false;
    return requested;
}

bool AndroidInput::consumeReloadRequest() noexcept
{
    bool const requested = m_reload_requested;
    m_reload_requested = false;
    return requested;
}

bool AndroidInput::consumeDebugHudToggleRequest() noexcept
{
    bool const requested = m_debug_hud_toggle_requested;
    m_debug_hud_toggle_requested = false;
    return requested;
}

bool AndroidInput::consumeLookDelta(float& horizontal, float& vertical) noexcept
{
    return m_state.consumeLookDelta(horizontal, vertical);
}

int32_t AndroidInput::handleKey(AInputEvent const* const event) noexcept
{
    int32_t const action = AKeyEvent_getAction(event);
    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
        return 0;
    }
    bool const pressed = action == AKEY_EVENT_ACTION_DOWN;
    switch (AKeyEvent_getKeyCode(event)) {
    case AKEYCODE_W:
    case AKEYCODE_DPAD_UP:
        m_state.setMoveKeyPressed(AndroidMoveKey::Up, pressed);
        return 1;
    case AKEYCODE_S:
    case AKEYCODE_DPAD_DOWN:
        m_state.setMoveKeyPressed(AndroidMoveKey::Down, pressed);
        return 1;
    case AKEYCODE_A:
    case AKEYCODE_DPAD_LEFT:
        m_state.setMoveKeyPressed(AndroidMoveKey::Left, pressed);
        return 1;
    case AKEYCODE_D:
    case AKEYCODE_DPAD_RIGHT:
        m_state.setMoveKeyPressed(AndroidMoveKey::Right, pressed);
        return 1;
    case AKEYCODE_R:
        if (pressed && !m_reload_pressed) {
            m_reload_requested = true;
        }
        m_reload_pressed = pressed;
        return 1;
    case AKEYCODE_F1:
        if (m_debug_hud_toggle.update(pressed)) {
            m_debug_hud_toggle_requested = true;
        }
        return 1;
    case AKEYCODE_BACK:
    case AKEYCODE_ESCAPE:
        if (pressed) {
            m_stop_requested = true;
        }
        return 1;
    default:
        return 0;
    }
}

int32_t AndroidInput::handleMotion(AInputEvent const* const event) noexcept
{
    if (
        (static_cast<uint32_t>(AInputEvent_getSource(event))
            & static_cast<uint32_t>(AINPUT_SOURCE_TOUCHSCREEN)) == 0
    ) {
        return 0;
    }
    int32_t const action = AMotionEvent_getAction(event);
    int32_t const action_mask = action & AMOTION_EVENT_ACTION_MASK;
    size_t const action_index = static_cast<size_t>(
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT
    );

    if (action_mask == AMOTION_EVENT_ACTION_CANCEL) {
        static_cast<void>(m_state.cancelTouch());
        static_cast<void>(m_state.cancelLookTouch());
        cancelFlightTouches();
        return 1;
    }

    if (action_mask == AMOTION_EVENT_ACTION_DOWN || action_mask == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        float const x = AMotionEvent_getX(event, action_index);
        float const y = AMotionEvent_getY(event, action_index);
        int32_t const pointer_id = AMotionEvent_getPointerId(event, action_index);
        if (beginFlightTouch(pointer_id, x, y)) {
            return 1;
        }
        if (m_state.beginLookTouch(pointer_id, x, y, m_surface_width)) {
            return 1;
        }
        return m_state.beginTouch(
            pointer_id,
            x,
            y,
            m_surface_width
        ) ? 1 : 0;
    }

    if (action_mask == AMOTION_EVENT_ACTION_MOVE) {
        size_t const pointer_count = AMotionEvent_getPointerCount(event);
        bool handled = false;
        for (size_t index = 0; index < pointer_count; ++index) {
            int32_t const pointer_id = AMotionEvent_getPointerId(event, index);
            float const x = AMotionEvent_getX(event, index);
            float const y = AMotionEvent_getY(event, index);
            handled = isFlightTouch(pointer_id) || handled;
            handled = m_state.moveTouch(pointer_id, x, y) || handled;
            handled = m_state.moveLookTouch(pointer_id, x, y) || handled;
        }
        return handled ? 1 : 0;
    }

    if (action_mask == AMOTION_EVENT_ACTION_UP || action_mask == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t const pointer_id = AMotionEvent_getPointerId(event, action_index);
        if (endFlightTouch(pointer_id)) {
            return 1;
        }
        if (m_state.endLookTouch(pointer_id)) {
            return 1;
        }
        return m_state.endTouch(pointer_id) ? 1 : 0;
    }
    return 0;
}

bool AndroidInput::beginFlightTouch(
    int32_t const pointer_id,
    float const x,
    float const y
) noexcept
{
    if (pointer_id < 0) {
        return false;
    }
    std::optional<int8_t> const direction = AndroidFlightTouchControls::direction(
        x,
        y,
        m_surface_width,
        m_surface_height
    );
    if (!direction.has_value()) {
        return false;
    }
    int32_t& active_pointer_id = *direction > 0
        ? m_ascending_touch_pointer_id
        : m_descending_touch_pointer_id;
    if (active_pointer_id >= 0) {
        return false;
    }
    active_pointer_id = pointer_id;
    return true;
}

bool AndroidInput::isFlightTouch(int32_t const pointer_id) const noexcept
{
    return pointer_id >= 0
        && (pointer_id == m_ascending_touch_pointer_id || pointer_id == m_descending_touch_pointer_id);
}

bool AndroidInput::endFlightTouch(int32_t const pointer_id) noexcept
{
    if (pointer_id < 0) {
        return false;
    }
    if (pointer_id == m_ascending_touch_pointer_id) {
        m_ascending_touch_pointer_id = -1;
        return true;
    }
    if (pointer_id == m_descending_touch_pointer_id) {
        m_descending_touch_pointer_id = -1;
        return true;
    }
    return false;
}

void AndroidInput::cancelFlightTouches() noexcept
{
    m_ascending_touch_pointer_id = -1;
    m_descending_touch_pointer_id = -1;
}

} // namespace game_android
