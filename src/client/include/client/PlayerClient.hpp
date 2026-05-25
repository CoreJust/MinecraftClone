#pragma once

#include "GameClient.hpp"
#include <client/render/Window.hpp>

namespace client {

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient()
        : m_window(Window::makeConsoleWindow(shared::World::WIDTH, shared::World::HEIGHT))
    { }
private:
    shared::Direction input() override;
    void render() override;
private:
    std::unique_ptr<Window> m_window;
};

} // namespace client
