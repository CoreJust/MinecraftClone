#include <client/PlayerClient.hpp>

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
    static_cast<void>(m_renderer.render(m_render_data));

    bool const reload_pressed = m_window.keyPressed(core::platform::glfw::WindowKey::R);
    if (reload_pressed && !m_was_reload_pressed) {
        m_renderer.hotReload();
    }
    m_was_reload_pressed = reload_pressed;
}

} // namespace client
