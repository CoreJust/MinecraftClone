#include <client/PlayerClient.hpp>

namespace client {

shared::Direction PlayerClient::input() {
    uint8_t off_x = 0;
    uint8_t off_y = 0;
    if (m_window->isKeyPressed(Key::W)) {
        off_y = 1;
    } else if (m_window->isKeyPressed(Key::S)) {
        off_y = static_cast<uint8_t>(-1);
    }
    if (m_window->isKeyPressed(Key::D)) {
        off_x = 1;
    } else if (m_window->isKeyPressed(Key::A)) {
        off_x = static_cast<uint8_t>(-1);
    }
    return shared::Direction{
        .x = off_x,
        .y = off_y,
    };
}

void PlayerClient::render() {
    m_window->pollEvents();
    if (m_window->shouldClose()) {
        m_running = false;
        return;
    }

    m_window->clear('.');
    for (shared::Player const& p : m_world.players()) {
        m_window->draw(p.x + 0, p.y + 0, p.ch);
        m_window->draw(p.x + 1, p.y + 0, p.ch);
        m_window->draw(p.x + 0, p.y + 1, p.ch);
        m_window->draw(p.x + 1, p.y + 1, p.ch);
    }
    m_window->display();
}

} // namespace client
