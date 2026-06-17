#include <client/PlayerClient.hpp>

#include <core/window/Keyboard.hpp>

namespace client {

shared::Direction PlayerClient::input() {
    if (core::isKeyPressed(core::Key::Escape)) {
        m_running = false;
    }
    uint8_t off_x = 0;
    uint8_t off_y = 0;
    if (core::isKeyPressed(core::Key::W)) {
        off_y = static_cast<uint8_t>(-1);
    } else if (core::isKeyPressed(core::Key::S)) {
        off_y = 1;
    }
    if (core::isKeyPressed(core::Key::D)) {
        off_x = 1;
    } else if (core::isKeyPressed(core::Key::A)) {
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

    std::vector<PlayerRenderData> rds;
    for (shared::Player const& p : m_world.players()) {
        rds.push_back({
            .x = p.x,
            .y = p.y,
            .color = { float(p.ch) / 256.f, 1.f - float(p.ch) / 256.f, 1.f, 1.f },
        });
    }
    m_renderer.render(rds);
}

} // namespace client
