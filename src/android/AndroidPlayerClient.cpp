#include "AndroidPlayerClient.hpp"

#include <client/CameraController.hpp>
#include <client/PlayerPresentation.hpp>

#include <shared/world/CanonicalWorld.hpp>

#include <core/IO/Log.hpp>

#include <android/configuration.h>
#include <android/looper.h>
#include <android/native_window.h>

#include <chrono>
#include <optional>
#include <vector>

namespace game_android {
namespace {

constexpr std::chrono::milliseconds ANDROID_PRESENT_WAIT_BUDGET{ 1 };

} // namespace

AndroidPlayerClient::AndroidPlayerClient(android_app& app, shared::WorldMode const mode)
    : client::GameClient{ mode }
    , m_app(app)
    , m_shader_assets(app.activity->assetManager)
{
    if (mode == shared::WorldMode::Flight) {
        m_chunk_mesh = &m_chunk_mesher.update(shared::canonicalWorld().chunk());
    }
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
    shared::Direction const input = m_input.direction();
    int8_t const touch_flight_direction = m_input.flightDirection();
    client::MovementDirection const movement = client::CameraController::cameraRelativeMovement(
        {
            .strafe = static_cast<int8_t>(input.x),
            .forward = static_cast<int8_t>(-static_cast<int8_t>(input.y)),
        },
        m_camera.pose().angles.yaw_degrees
    );
    return {
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
        .z = static_cast<uint8_t>(client::CameraController::verticalMovement(
            m_ascend_pressed || touch_flight_direction > 0,
            m_descend_pressed || touch_flight_direction < 0
        )),
    };
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

    float look_horizontal = 0.0F;
    float look_vertical = 0.0F;
    if (m_input.consumeLookDelta(look_horizontal, look_vertical)) {
        static_cast<void>(m_camera.rotate(
            static_cast<double>(look_horizontal) * 0.15,
            static_cast<double>(-look_vertical) * 0.15
        ));
    }
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    std::optional<client::PlayerPresentationPosition> const local_position = predictedLocalPresentation(now);
    if (local_position.has_value()) {
        client::CameraPose const camera_pose = client::localPlayerFirstPersonPose(
            *local_position,
            m_camera.pose().angles
        );
        static_cast<void>(m_camera.setPosition(camera_pose.position));
    }

    std::vector<client::PlayerRenderData> players;
    players.reserve(m_world.players().size());
    for (shared::Player const& player : m_world.players()) {
        if (player.ch == m_local_character) {
            continue;
        }
        if (auto const position = m_player_presentation.sample(player.ch, now)) {
            players.push_back({
                .x = static_cast<float>(position->x),
                .y = static_cast<float>(position->y),
                .color = {
                    static_cast<float>(player.ch) / 256.0f,
                    1.0f - static_cast<float>(player.ch) / 256.0f,
                    1.0f,
                    1.0f,
                },
                .z = static_cast<float>(position->z),
            });
        }
    }
    client::DebugHudInput const debug_hud_input = [&] {
        client::DebugHudInput input;
        input.touch_flight_help = m_world.mode() == shared::WorldMode::Flight;
        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.player_x = static_cast<float>(shared::playerPositionX(*player));
            input.player_y = static_cast<float>(shared::playerPositionY(*player));
            input.player_z = static_cast<float>(shared::playerPositionZ(*player));
        }
        client::CameraAngles const angles = m_camera.pose().angles;
        input.camera_yaw_degrees = static_cast<float>(angles.yaw_degrees);
        input.camera_pitch_degrees = static_cast<float>(angles.pitch_degrees);
        input.camera_roll_degrees = static_cast<float>(angles.roll_degrees);
        return input;
    }();
    if (m_input.consumeDebugHudToggleRequest()) {
        m_renderer->toggleDebugHud();
    }
    m_renderer->setCamera(m_camera.pose());
    static_cast<void>(m_renderer->render(
        players,
        debug_hud_input,
        m_density_scale,
        std::chrono::steady_clock::now() + ANDROID_PRESENT_WAIT_BUDGET
    ));
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
    if (self == nullptr) {
        return 0;
    }
    bool const vertical_handled = self->updateVerticalInput(event);
    return self->m_input.handle(event) != 0 || vertical_handled ? 1 : 0;
}

bool AndroidPlayerClient::updateVerticalInput(AInputEvent const* const event) noexcept
{
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) {
        return false;
    }
    int32_t const action = AKeyEvent_getAction(event);
    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
        return false;
    }
    bool const pressed = action == AKEY_EVENT_ACTION_DOWN;
    switch (AKeyEvent_getKeyCode(event)) {
    case AKEYCODE_SPACE:
    case AKEYCODE_BUTTON_R1:
        m_ascend_pressed = pressed;
        return true;
    case AKEYCODE_SHIFT_LEFT:
    case AKEYCODE_SHIFT_RIGHT:
    case AKEYCODE_BUTTON_L1:
        m_descend_pressed = pressed;
        return true;
    default:
        return false;
    }
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
            m_input.setSurfaceHeight(height);
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
        m_ascend_pressed = false;
        m_descend_pressed = false;
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
    m_input.setSurfaceHeight(static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window)));
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
    m_renderer->setDebugHudEnabled(true);
    if (m_chunk_mesh != nullptr) {
        m_renderer->setChunkMesh(*m_chunk_mesh);
    }
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
    m_ascend_pressed = false;
    m_descend_pressed = false;
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
