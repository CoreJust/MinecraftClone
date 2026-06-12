#pragma once

#include "GameClient.hpp"
#include "render/VulkanRenderer.hpp"

#include <shared/ProjectInfo.hpp>

namespace client {

class PlayerClient final : public GameClient {
public:
    explicit PlayerClient()
        : m_window(std::string{ shared::PROJECT_NAME })
        , m_renderer(m_window)
    { }
private:
    shared::Direction input() override;
    void render() override;
private:
    core::Window m_window;
    VulkanRenderer m_renderer;
};

} // namespace client
