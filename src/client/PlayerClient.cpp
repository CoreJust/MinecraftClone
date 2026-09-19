#include <client/PlayerClient.hpp>

#include <client/CameraController.hpp>
#include <client/PlayerPresentation.hpp>

#include <shared/world/CanonicalWorld.hpp>
#include <shared/world/ChunkMesher.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <optional>
#include <string>

namespace client {

PlayerClient::~PlayerClient()
{
    if (m_window.nativeHandle() != nullptr) {
        glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

PlayerClient::PlayerClient(shared::WorldMode const mode)
    : GameClient{ mode }
    , m_window(core::platform::glfw::WindowDescriptor{
        .width = 1280U,
        .height = 720U,
        .title = std::string{ shared::PROJECT_NAME },
    })
    , m_renderer(VulkanRenderer::createPresentationContext(m_window), m_shader_assets)
{
    beginContinuousLook();
    m_renderer.setDebugHudEnabled(true);
    if (mode == shared::WorldMode::Flight) {
        shared::ChunkMesher mesher;
        m_renderer.setChunkMesh(mesher.update(shared::canonicalWorld().chunk()));
    }
}

void PlayerClient::beginContinuousLook() noexcept
{
    glfwSetInputMode(m_window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

shared::Direction PlayerClient::input() {
    if (m_window.keyPressed(core::platform::glfw::WindowKey::Escape)) {
        m_running = false;
    }
    MovementIntent const intent{
        .strafe = static_cast<int8_t>(
            static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::D))
            - static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::A))
        ),
        .forward = static_cast<int8_t>(
            static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::W))
            - static_cast<int8_t>(m_window.keyPressed(core::platform::glfw::WindowKey::S))
        ),
    };
    MovementDirection const movement = CameraController::cameraRelativeMovement(
        intent,
        m_camera.pose().angles.yaw_degrees
    );
    return shared::Direction{
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
        .z = static_cast<uint8_t>(CameraController::verticalMovement(
            glfwGetKey(m_window.nativeHandle(), GLFW_KEY_SPACE) == GLFW_PRESS,
            glfwGetKey(m_window.nativeHandle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
            || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS
        )),
        .accelerated = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
            || glfwGetKey(m_window.nativeHandle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS,
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
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(m_window.nativeHandle(), &cursor_x, &cursor_y);
    if (m_has_cursor_position) {
        static_cast<void>(m_camera.rotate(
            (cursor_x - m_last_cursor_x) * 0.15,
            (m_last_cursor_y - cursor_y) * 0.15
        ));
    }
    m_last_cursor_x = cursor_x;
    m_last_cursor_y = cursor_y;
    m_has_cursor_position = true;
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    std::vector<PreviewHandle> const previews = previewResidency().handles();
    uint64_t const preview_serial = previewResidency().changeSerial();
    if (m_preview_observed_serial != preview_serial) {
        m_preview_observed_serial = preview_serial;
        m_preview_mesh_ready_at = now + std::chrono::milliseconds{100};
    }
    if (m_preview_mesh_serial != preview_serial && now >= m_preview_mesh_ready_at) {
        std::vector<PreviewHandle> mesh_sources;
        std::vector<shared::ChunkMesh> meshes;
        mesh_sources.reserve(previews.size());
        meshes.reserve(previews.size());
        shared::ChunkMesher mesher;
        for (PreviewHandle const& preview : previews) {
            if (preview->bytes().size() != shared::Chunk::BLOCK_COUNT) {
                continue;
            }
            auto const existing = std::ranges::find(m_preview_mesh_sources, preview);
            if (existing != m_preview_mesh_sources.end()) {
                uint64_t const index = static_cast<uint64_t>(existing - m_preview_mesh_sources.begin());
                mesh_sources.push_back(preview);
                meshes.push_back(std::move(m_preview_meshes[index]));
                continue;
            }
            shared::Chunk::Blocks blocks{};
            for (uint32_t index = 0U; index < shared::Chunk::BLOCK_COUNT; ++index) {
                uint8_t const value = preview->bytes()[index];
                blocks[index] = value == static_cast<uint8_t>(shared::Block::Stone)
                    ? shared::Block::Stone : shared::Block::Air;
            }
            PreviewChunkKey const key = preview->key();
            shared::Chunk const chunk{{.x = key.x, .y = key.y, .z = key.z}, std::move(blocks)};
            mesh_sources.push_back(preview);
            meshes.push_back(mesher.update(chunk));
        }
        m_preview_mesh_sources = std::move(mesh_sources);
        m_preview_meshes = std::move(meshes);
        m_preview_mesh_serial = preview_serial;
        m_renderer.setChunkMeshes(m_preview_meshes);
        for (PreviewHandle const& preview : previews) {
            static_cast<void>(previewResidency().markUploaded(preview));
        }
    }
    std::optional<PlayerPresentationPosition> const local_position = predictedLocalPresentation(now);
    if (local_position.has_value()) {
        CameraPose const camera_pose = localPlayerFirstPersonPose(*local_position, m_camera.pose().angles);
        static_cast<void>(m_camera.setPosition(camera_pose.position));
    }
    m_renderer.recreate(width, height);
    m_render_data.clear();
    m_render_data.reserve(m_world.players().size());
    for (shared::Player const& p : m_world.players()) {
        if (p.ch == m_local_character) {
            continue;
        }
        if (auto const position = m_player_presentation.sample(p.ch, now)) {
            m_render_data.push_back({
                .x = static_cast<float>(position->x),
                .y = static_cast<float>(position->y),
                .color = { float(p.ch) / 256.f, 1.f - float(p.ch) / 256.f, 1.f, 1.f },
                .z = static_cast<float>(position->z),
            });
        }
    }
    DebugHudInput const debug_hud_input = [&] {
        DebugHudInput input;
        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.player_x = static_cast<float>(shared::playerPositionX(*player));
            input.player_y = static_cast<float>(shared::playerPositionY(*player));
            input.player_z = static_cast<float>(shared::playerPositionZ(*player));
        }
        CameraAngles const angles = m_camera.pose().angles;
        input.camera_yaw_degrees = static_cast<float>(angles.yaw_degrees);
        input.camera_pitch_degrees = static_cast<float>(angles.pitch_degrees);
        input.camera_roll_degrees = static_cast<float>(angles.roll_degrees);
        return input;
    }();
    float content_scale_x = 1.0F;
    float content_scale_y = 1.0F;
    glfwGetWindowContentScale(m_window.nativeHandle(), &content_scale_x, &content_scale_y);
    bool const debug_hud_pressed = glfwGetKey(m_window.nativeHandle(), GLFW_KEY_F1) == GLFW_PRESS;
    if (m_debug_hud_toggle.update(debug_hud_pressed)) {
        m_renderer.toggleDebugHud();
    }
    m_renderer.setCamera(m_camera.pose());
    static_cast<void>(m_renderer.render(
        m_render_data,
        debug_hud_input,
        std::max(content_scale_x, content_scale_y)
    ));

    bool const reload_pressed = m_window.keyPressed(core::platform::glfw::WindowKey::R);
    if (reload_pressed && !m_was_reload_pressed) {
        m_renderer.hotReload();
    }
    m_was_reload_pressed = reload_pressed;
}

} // namespace client
