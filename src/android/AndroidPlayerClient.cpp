#include "AndroidPlayerClient.hpp"

#include <core/IO/Log.hpp>

#include <android/configuration.h>
#include <android/looper.h>
#include <android/native_window.h>

#include <vector>

namespace game_android {

AndroidPlayerClient::AndroidPlayerClient(android_app& app)
    : m_app(app)
    , m_shader_assets(app.activity->assetManager)
{
    m_app.userData = this;
    m_app.onAppCmd = &AndroidPlayerClient::handleAppCommand;
    m_app.onInputEvent = &AndroidPlayerClient::handleInputEvent;

    int32_t const density = AConfiguration_getDensity(m_app.config);
    if (density > 0) {
        m_density_scale = static_cast<float>(density)
            / static_cast<float>(static_cast<int32_t>(ACONFIGURATION_DENSITY_MEDIUM));
        m_input.setDensity(m_density_scale);
    }
}

AndroidPlayerClient::~AndroidPlayerClient()
{
    stop();
    if (m_app.userData == this) {
        m_app.userData = nullptr;
        m_app.onAppCmd = nullptr;
        m_app.onInputEvent = nullptr;
    }
}

shared::Direction AndroidPlayerClient::input()
{
    if (m_input.consumeStopRequest()) {
        stop();
    }
    return m_input.direction();
}

void AndroidPlayerClient::render()
{
    drainEvents();
    while (m_running && !m_app.destroyRequested && !canRender()) {
        pollOneEvent(-1);
    }
    if (m_app.destroyRequested || m_input.consumeStopRequest()) {
        stop();
        return;
    }
    if (!m_running || !canRender()) {
        return;
    }

    std::vector<client::PlayerRenderData> players;
    players.reserve(m_world.players().size());
    for (shared::Player const& player : m_world.players()) {
        players.push_back({
            .x = player.x,
            .y = player.y,
            .color = {
                static_cast<float>(player.ch) / 256.0f,
                1.0f - static_cast<float>(player.ch) / 256.0f,
                1.0f,
                1.0f,
            },
        });
    }
    client::DebugHudInput const debug_hud_input = [&] {
        client::DebugHudInput input;
        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.player_x = static_cast<float>(player->x);
            input.player_y = static_cast<float>(player->y);
        }
        return input;
    }();
    if (m_input.consumeDebugHudToggleRequest()) {
        m_renderer->toggleDebugHud();
    }
    static_cast<void>(m_renderer->render(players, debug_hud_input, m_density_scale));
    if (m_input.consumeReloadRequest()) {
        m_renderer->hotReload();
    }
}

void AndroidPlayerClient::handleAppCommand(android_app* const app, int32_t const command)
{
    auto* const self = static_cast<AndroidPlayerClient*>(app->userData);
    if (self != nullptr) {
        self->onAppCommand(command);
    }
}

int32_t AndroidPlayerClient::handleInputEvent(android_app* const app, AInputEvent* const event)
{
    auto* const self = static_cast<AndroidPlayerClient*>(app->userData);
    return self == nullptr ? 0 : self->m_input.handle(event);
}

void AndroidPlayerClient::onAppCommand(int32_t const command)
{
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        CORE_INFO("Android native window initialized");
        createWindowResources();
        break;
    case APP_CMD_TERM_WINDOW:
        CORE_INFO("Android native window terminated");
        destroyWindowResources();
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        if (m_app.window != nullptr) {
            uint32_t const width = static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window));
            uint32_t const height = static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window));
            m_input.setSurfaceWidth(width);
            if (m_renderer != nullptr) {
                m_renderer->recreate(width, height);
            }
        }
        break;
    case APP_CMD_RESUME:
        CORE_INFO("Android activity resumed");
        m_resumed = true;
        break;
    case APP_CMD_PAUSE:
    case APP_CMD_STOP:
    case APP_CMD_LOST_FOCUS:
        CORE_INFO("Android rendering paused by app command {}", command);
        m_resumed = false;
        m_has_focus = false;
        m_input.clear();
        break;
    case APP_CMD_GAINED_FOCUS:
        m_has_focus = true;
        break;
    case APP_CMD_DESTROY:
        stop();
        break;
    default:
        break;
    }
}

void AndroidPlayerClient::createWindowResources()
{
    if (m_app.window == nullptr) {
        return;
    }
    destroyWindowResources();
    m_input.setSurfaceWidth(static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window)));
    m_renderer = std::make_unique<client::VulkanRenderer>(
        client::VulkanRenderer::createPresentationContext(
            m_app.window,
            static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window)),
            static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window)),
            client::VulkanRendererOptions{
                .prefer_mesh_shaders = false,
            }
        ),
        m_shader_assets,
        client::VulkanRendererOptions{
            .prefer_mesh_shaders = false,
        }
    );
    CORE_INFO(
        "Android renderer created for {}x{} surface",
        ANativeWindow_getWidth(m_app.window),
        ANativeWindow_getHeight(m_app.window)
    );
}

void AndroidPlayerClient::destroyWindowResources() noexcept
{
    if (m_renderer != nullptr) {
        CORE_INFO("Destroying Android renderer before releasing its native window");
    }
    m_renderer.reset();
    m_input.clear();
}

void AndroidPlayerClient::pollOneEvent(int32_t const timeout_millis)
{
    void* data = nullptr;
    int32_t const result = ALooper_pollOnce(
        timeout_millis,
        nullptr,
        nullptr,
        &data
    );
    if (result >= 0 && data != nullptr) {
        auto* const source = static_cast<android_poll_source*>(data);
        source->process(&m_app, source);
    }
}

void AndroidPlayerClient::drainEvents()
{
    for (;;) {
        void* data = nullptr;
        int32_t const result = ALooper_pollOnce(0, nullptr, nullptr, &data);
        if (result < 0) {
            return;
        }
        if (data != nullptr) {
            auto* const source = static_cast<android_poll_source*>(data);
            source->process(&m_app, source);
        }
    }
}

void AndroidPlayerClient::stop() noexcept
{
    m_running = false;
    m_resumed = false;
    m_has_focus = false;
    destroyWindowResources();
}

bool AndroidPlayerClient::canRender() const noexcept
{
    return m_resumed
        && m_has_focus
        && m_renderer != nullptr
        && m_app.window != nullptr
        && ANativeWindow_getWidth(m_app.window) > 0
        && ANativeWindow_getHeight(m_app.window) > 0;
}

} // namespace game_android
