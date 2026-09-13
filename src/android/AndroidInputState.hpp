#pragma once

#include <shared/world/World.hpp>

#include <cstdint>

namespace game_android {

enum class AndroidMoveKey : uint8_t {
    Up,
    Down,
    Left,
    Right,
};

class AndroidInputState final {
public:
    void setDensity(float density) noexcept;
    void clear() noexcept;
    void setMoveKeyPressed(AndroidMoveKey key, bool pressed) noexcept;

    [[nodiscard]]
    bool beginTouch(int32_t pointer_id, float x, float y, uint32_t surface_width) noexcept;
    [[nodiscard]]
    bool moveTouch(int32_t pointer_id, float x, float y) noexcept;
    [[nodiscard]]
    bool endTouch(int32_t pointer_id) noexcept;
    bool cancelTouch() noexcept;
    [[nodiscard]]
    bool beginLookTouch(int32_t pointer_id, float x, float y, uint32_t surface_width) noexcept;
    [[nodiscard]]
    bool moveLookTouch(int32_t pointer_id, float x, float y) noexcept;
    [[nodiscard]]
    bool endLookTouch(int32_t pointer_id) noexcept;
    bool cancelLookTouch() noexcept;
    [[nodiscard]]
    bool consumeLookDelta(float& horizontal, float& vertical) noexcept;

    [[nodiscard]]
    shared::Direction direction() const noexcept;
private:
    [[nodiscard]]
    static uint8_t directionComponent(float offset, float dead_zone) noexcept;
private:
    float m_density = 1.0f;

    bool m_up_pressed = false;
    bool m_down_pressed = false;
    bool m_left_pressed = false;
    bool m_right_pressed = false;

    int32_t m_touch_pointer_id = -1;
    float m_touch_origin_x = 0.0f;
    float m_touch_origin_y = 0.0f;
    uint8_t m_touch_x = 0;
    uint8_t m_touch_y = 0;

    int32_t m_look_pointer_id = -1;
    float m_look_x = 0.0F;
    float m_look_y = 0.0F;
    float m_look_delta_x = 0.0F;
    float m_look_delta_y = 0.0F;
};

} // namespace game_android
