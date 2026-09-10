#pragma once

#include "AndroidInput.hpp"
#include "AndroidShaderAssets.hpp"
#include "AndroidSurfaceProvider.hpp"

#include <client/GameClient.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <android_native_app_glue.h>

#include <cstdint>
#include <memory>

namespace game_android {

class AndroidPlayerClient final : public client::GameClient {
public:
    explicit AndroidPlayerClient(android_app& app);
    ~AndroidPlayerClient();
private:
    shared::Direction input() override;
    void render() override;

    static void handleAppCommand(android_app* app, int32_t command);
    static int32_t handleInputEvent(android_app* app, AInputEvent* event);

    void onAppCommand(int32_t command);
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
    std::unique_ptr<AndroidSurfaceProvider> m_surface_provider;
    std::unique_ptr<client::VulkanRenderer> m_renderer;
    bool m_resumed = false;
    bool m_has_focus = false;
};

} // namespace game_android
