#include <client/PlayerClient.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>

namespace client {

shared::Direction PlayerClient::input() {
    if (m_window.keyPressed(core::platform::glfw::WindowKey::Escape)) {
        m_running = false;
    }
    uint8_t off_x = 0;
    uint8_t off_y = 0;
    if (m_window.keyPressed(core::platform::glfw::WindowKey::W)) {
        off_y = static_cast<uint8_t>(-1);
    } else if (m_window.keyPressed(core::platform::glfw::WindowKey::S)) {
        off_y = 1;
    }
    if (m_window.keyPressed(core::platform::glfw::WindowKey::D)) {
        off_x = 1;
    } else if (m_window.keyPressed(core::platform::glfw::WindowKey::A)) {
        off_x = static_cast<uint8_t>(-1);
    }
    return shared::Direction{
        .x = off_x,
        .y = off_y,
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
        return input;
    }();
    float content_scale_x = 1.0F;
    float content_scale_y = 1.0F;
    glfwGetWindowContentScale(m_window.nativeHandle(), &content_scale_x, &content_scale_y);
    bool const debug_hud_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_F1) == GLFW_PRESS;
    if (m_debug_hud_toggle.update(debug_hud_pressed)) {
        m_renderer.toggleDebugHud();
    }
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
