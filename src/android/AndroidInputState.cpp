#include "AndroidInputState.hpp"

#include <cmath>

namespace game_android {
namespace {

constexpr float TOUCH_DEAD_ZONE_DP = 24.0f;

} // namespace

void AndroidInputState::setDensity(float const density) noexcept
{
    if (density > 0.0f && std::isfinite(density)) {
        m_density = density;
    }
}

void AndroidInputState::clear() noexcept
{
    m_up_pressed = false;
    m_down_pressed = false;
    m_left_pressed = false;
    m_right_pressed = false;
    static_cast<void>(cancelTouch());
    static_cast<void>(cancelLookTouch());
}

void AndroidInputState::setMoveKeyPressed(
    AndroidMoveKey const key,
    bool const pressed
) noexcept {
    switch (key) {
    case AndroidMoveKey::Up:
        m_up_pressed = pressed;
        break;
    case AndroidMoveKey::Down:
        m_down_pressed = pressed;
        break;
    case AndroidMoveKey::Left:
        m_left_pressed = pressed;
        break;
    case AndroidMoveKey::Right:
        m_right_pressed = pressed;
        break;
    }
}

bool AndroidInputState::beginTouch(
    int32_t const pointer_id,
    float const x,
    float const y,
    uint32_t const surface_width
) noexcept {
    if (
        m_touch_pointer_id >= 0
        || pointer_id < 0
        || surface_width == 0
        || !std::isfinite(x)
        || !std::isfinite(y)
        || x >= static_cast<float>(surface_width) / 2.0f
    ) {
        return false;
    }

    m_touch_pointer_id = pointer_id;
    m_touch_origin_x = x;
    m_touch_origin_y = y;
    m_touch_x = 0;
    m_touch_y = 0;
    return true;
}

bool AndroidInputState::moveTouch(
    int32_t const pointer_id,
    float const x,
    float const y
) noexcept {
    if (pointer_id != m_touch_pointer_id || m_touch_pointer_id < 0) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        static_cast<void>(cancelTouch());
        return true;
    }

    float const dead_zone = TOUCH_DEAD_ZONE_DP * m_density;
    m_touch_x = directionComponent(x - m_touch_origin_x, dead_zone);
    m_touch_y = directionComponent(y - m_touch_origin_y, dead_zone);
    return true;
}

bool AndroidInputState::endTouch(int32_t const pointer_id) noexcept
{
    if (pointer_id != m_touch_pointer_id || m_touch_pointer_id < 0) {
        return false;
    }
    static_cast<void>(cancelTouch());
    return true;
}

bool AndroidInputState::cancelTouch() noexcept
{
    bool const had_touch = m_touch_pointer_id >= 0;
    m_touch_pointer_id = -1;
    m_touch_x = 0;
    m_touch_y = 0;
    return had_touch;
}

bool AndroidInputState::beginLookTouch(
    int32_t const pointer_id,
    float const x,
    float const y,
    uint32_t const surface_width
) noexcept
{
    if (
        m_look_pointer_id >= 0
        || pointer_id < 0
        || surface_width == 0U
        || !std::isfinite(x)
        || !std::isfinite(y)
        || x < static_cast<float>(surface_width) / 2.0F
    ) {
        return false;
    }
    m_look_pointer_id = pointer_id;
    m_look_x = x;
    m_look_y = y;
    return true;
}

bool AndroidInputState::moveLookTouch(
    int32_t const pointer_id,
    float const x,
    float const y
) noexcept
{
    if (pointer_id != m_look_pointer_id || m_look_pointer_id < 0) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        static_cast<void>(cancelLookTouch());
        return true;
    }
    m_look_delta_x += x - m_look_x;
    m_look_delta_y += y - m_look_y;
    m_look_x = x;
    m_look_y = y;
    return true;
}

bool AndroidInputState::endLookTouch(int32_t const pointer_id) noexcept
{
    if (pointer_id != m_look_pointer_id || m_look_pointer_id < 0) {
        return false;
    }
    static_cast<void>(cancelLookTouch());
    return true;
}

bool AndroidInputState::cancelLookTouch() noexcept
{
    bool const had_look = m_look_pointer_id >= 0;
    m_look_pointer_id = -1;
    m_look_x = 0.0F;
    m_look_y = 0.0F;
    m_look_delta_x = 0.0F;
    m_look_delta_y = 0.0F;
    return had_look;
}

bool AndroidInputState::consumeLookDelta(float& horizontal, float& vertical) noexcept
{
    horizontal = m_look_delta_x;
    vertical = m_look_delta_y;
    m_look_delta_x = 0.0F;
    m_look_delta_y = 0.0F;
    return horizontal != 0.0F || vertical != 0.0F;
}

shared::Direction AndroidInputState::direction() const noexcept
{
    int8_t const hardware_x = static_cast<int8_t>(m_right_pressed) - static_cast<int8_t>(m_left_pressed);
    int8_t const hardware_y = static_cast<int8_t>(m_down_pressed) - static_cast<int8_t>(m_up_pressed);
    return {
        .x = (m_right_pressed || m_left_pressed) ? static_cast<uint8_t>(hardware_x) : m_touch_x,
        .y = (m_down_pressed || m_up_pressed) ? static_cast<uint8_t>(hardware_y) : m_touch_y,
    };
}

uint8_t AndroidInputState::directionComponent(
    float const offset,
    float const dead_zone
) noexcept {
    if (offset < -dead_zone) {
        return static_cast<uint8_t>(-1);
    }
    if (offset > dead_zone) {
        return 1;
    }
    return 0;
}

} // namespace game_android
