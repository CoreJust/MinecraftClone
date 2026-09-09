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

shared::Direction AndroidInputState::direction() const noexcept
{
    uint8_t const hardware_x = m_right_pressed
        ? 1
        : (m_left_pressed ? static_cast<uint8_t>(-1) : 0);
    uint8_t const hardware_y = m_down_pressed
        ? 1
        : (m_up_pressed ? static_cast<uint8_t>(-1) : 0);
    return {
        .x = hardware_x == 0 ? m_touch_x : hardware_x,
        .y = hardware_y == 0 ? m_touch_y : hardware_y,
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
