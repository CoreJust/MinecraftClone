#include <client/PlayerClient.hpp>

#include <client/CameraController.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>

namespace client {

PlayerClient::~PlayerClient()
{
    if (m_window.nativeHandle() != nullptr) {
        glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

void PlayerClient::beginContinuousLook() noexcept
{
    glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

shared::Direction PlayerClient::input() {
    if (m_window.keyPressed(core::platform::glfw::WindowKey::Escape)) {
        m_running = false;
    }
    MovementIntent const intent{
        .strafe = static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::D)
            ? 1 : (m_window.keyPressed(core::platform::glfw::WindowKey::A) ? -1 : 0)),
        .forward = static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::W)
            ? 1 : (m_window.keyPressed(core::platform::glfw::WindowKey::S) ? -1 : 0)),
    };
    DiscreteMovement const movement = CameraController::cameraRelativeMovement(
        intent,
        m_camera.pose().angles.yaw_degrees
    );
    return shared::Direction{
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
    };
}

void PlayerClient::render() {
    if (!m_window.nextFrame()) {
        m_running = false;
        return;
    }

    uint32_t width = 0U;
    uint32_t height = 0U;
    m_window.framebufferSize(width, height);
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(m_window.nativeHandle(), &cursor_x, &cursor_y);
    if (m_has_cursor_position) {
        static_cast<void>(m_camera.rotate(
            (cursor_x - m_last_cursor_x) * 0.15,
            (m_last_cursor_y - cursor_y) * 0.15
        ));
    }
    m_last_cursor_x = cursor_x;
    m_last_cursor_y = cursor_y;
    m_has_cursor_position = true;
    m_renderer.recreate(width, height);
    m_render_data.clear();
    m_render_data.reserve(m_world.players().size());
    for (shared::Player const& p : m_world.players()) {
        m_render_data.push_back({
            .x = p.x,
            .y = p.y,
            .color = { float(p.ch) / 256.f, 1.f - float(p.ch) / 256.f, 1.f, 1.f },
        });
    }
    DebugHudInput const debug_hud_input = [&] {
        DebugHudInput input;
        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.player_x = static_cast<float>(player->x);
            input.player_y = static_cast<float>(player->y);
        }
        CameraAngles const angles = m_camera.pose().angles;
        input.camera_yaw_degrees = static_cast<float>(angles.yaw_degrees);
        input.camera_pitch_degrees = static_cast<float>(angles.pitch_degrees);
        input.camera_roll_degrees = static_cast<float>(angles.roll_degrees);
        return input;
    }();
    float content_scale_x = 1.0F;
    float content_scale_y = 1.0F;
    glfwGetWindowContentScale(m_window.nativeHandle(), &content_scale_x, &content_scale_y);
    bool const debug_hud_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_F1) == GLFW_PRESS;
    if (m_debug_hud_toggle.update(debug_hud_pressed)) {
        m_renderer.toggleDebugHud();
    }
    m_renderer.setCamera(m_camera.pose());
    static_cast<void>(m_renderer.render(
        m_render_data,
        debug_hud_input,
        std::max(content_scale_x, content_scale_y)
    ));

    bool const reload_pressed = m_window.keyPressed(core::platform::glfw::WindowKey::R);
    if (reload_pressed && !m_was_reload_pressed) {
        m_renderer.hotReload();
    }
    m_was_reload_pressed = reload_pressed;
}

} // namespace client
