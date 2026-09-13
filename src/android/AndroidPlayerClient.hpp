#pragma once

#include "AndroidInput.hpp"
#include "AndroidShaderAssets.hpp"

#include <client/Camera.hpp>
#include <client/GameClient.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>

#include <android_native_app_glue.h>

#include <cstdint>
#include <memory>

namespace game_android {

class AndroidPlayerClient final : public client::GameClient {
public:
    explicit AndroidPlayerClient(
        android_app& app,
        shared::WorldMode mode = shared::WorldMode::Flat
    );
    ~AndroidPlayerClient();
private:
    shared::Direction input() override;
    void render() override;

    static void handleAppCommand(android_app* app, int32_t command);
    static int32_t handleInputEvent(android_app* app, AInputEvent* event);

    void onAppCommand(int32_t command);
    [[nodiscard]] bool updateVerticalInput(AInputEvent const* event) noexcept;
    void createWindowResources();
    void destroyWindowResources() noexcept;
    void pollOneEvent(int32_t timeout_millis);
    void drainEvents();
    void stop() noexcept;

    [[nodiscard]]
    bool canRender() const noexcept;
private:
    android_app& m_app;
    AndroidInput m_input;
    AndroidShaderAssets m_shader_assets;
    client::Camera m_camera{
        { .position = { 9.0, 9.0, 13.0 } },
    };
    shared::ChunkMesher m_chunk_mesher;
    shared::ChunkMesh const* m_chunk_mesh = nullptr;
    std::unique_ptr<client::VulkanRenderer> m_renderer;
    float m_density_scale = 1.0F;
    bool m_resumed = false;
    bool m_has_focus = false;
    bool m_ascend_pressed = false;
    bool m_descend_pressed = false;
};

} // namespace game_android
