#include <client/render/VulkanRenderer.hpp>

#include <core/window/Window.hpp>

#include <gtest/gtest.h>

#include <array>

namespace {

void renderSmoke(bool const prefer_mesh_shaders)
{
    static constexpr uint32_t FRAME_COUNT = 6;
    static constexpr uint32_t RELOAD_FRAME = 3;
    core::Window const window{ "MinecraftClone renderer smoke", 320, 240 };
    client::VulkanRenderer renderer{
        window,
        { .require_validation = true, .prefer_mesh_shaders = prefer_mesh_shaders },
    };
    std::array<client::PlayerRenderData, 2> players{
        client::PlayerRenderData{ .x = 2, .y = 3, .color = { 1.f, 0.f, 0.f, 1.f } },
        client::PlayerRenderData{ .x = 29, .y = 28, .color = { 0.f, 1.f, 0.f, 1.f } },
    };
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_FALSE(window.isFramebufferSizeZero());
        if (frame == RELOAD_FRAME) {
            renderer.hotReload();
        }
        players.front().x += 1;
        renderer.render(frame == 0 ? std::span<client::PlayerRenderData const>{} : players);
    }
}

} // namespace

TEST(RendererSmokeTest, AutomaticPipelineDrawsAndReloads)
{
    renderSmoke(true);
}

TEST(RendererSmokeTest, VertexFallbackDrawsAndReloads)
{
    renderSmoke(false);
}
