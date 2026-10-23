#include "AndroidPlayerClient.hpp"

#include <client/CameraController.hpp>
#include <client/CameraObstruction.hpp>
#include <client/GenerationMapPresentation.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewMeshing.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>

#include <core/IO/Log.hpp>

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>

#include <glm/ext/vector_double3.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace game_android {
namespace {

constexpr std::chrono::milliseconds ANDROID_PRESENT_WAIT_BUDGET{ 1 };
constexpr std::chrono::milliseconds ANDROID_PREVIEW_MESH_TIME_BUDGET{ 2 };
constexpr std::chrono::milliseconds ANDROID_PREVIEW_UPLOAD_TIME_BUDGET{ 2 };
constexpr uint32_t ANDROID_PREVIEW_CHANGE_BUDGET = 16U;
constexpr uint32_t ANDROID_PREVIEW_MESH_BUDGET = 8U;
constexpr uint32_t CAMERA_OBSTRUCTION_SAMPLE_COUNT = 24U;
constexpr uint32_t CAMERA_OBSTRUCTION_BINARY_STEPS = 8U;
constexpr double CAMERA_OBSTRUCTION_MARGIN = 0.03;

[[nodiscard]] int32_t floorDivideByHeightTileSide(int32_t const value) noexcept
{
    int32_t result = value / static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH);
    if (value < 0 && value % static_cast<int32_t>(shared::HEIGHT_TILE_SIDE_LENGTH) != 0) {
        --result;
    }
    return result;
}

[[nodiscard]] client::HeightTileKey offsetHeightTileKey(
    client::HeightTileKey const key,
    int32_t const x_offset,
    int32_t const y_offset
) noexcept
{
    return shared::normalizeHeightTileKey({ key.x + x_offset, key.y + y_offset });
}

} // namespace

AndroidPlayerClient::AndroidPlayerClient(android_app& app, shared::WorldMode const mode)
    : client::GameClient{ mode }
    , m_app(app)
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
    if (worldLoadingStatus().state != client::WorldLoadingState::Playing) {
        return {};
    }
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
        m_look_camera.pose().angles.yaw_degrees
    );
    bool const ascend_requested = m_ascend_input.consumePress();
    bool const touch_ascend_requested = m_input.consumeFlightAscendRequest();
    m_input_actions.observeJump(ascend_requested || touch_ascend_requested);
    glm::dvec3 const view = m_look_camera.forward();
    return {
        .x = static_cast<uint8_t>(movement.x),
        .y = static_cast<uint8_t>(movement.y),
        .z = static_cast<uint8_t>(client::CameraController::verticalMovement(
            m_input_actions.jumpRequested(m_ascend_input.isPressed() || touch_flight_direction > 0),
            m_descend_pressed || touch_flight_direction < 0
        )),
        .cycle_movement_capabilities = m_input_actions.cycleRequested(),
        .view_x = client::CameraController::quantize(view.x),
        .view_y = client::CameraController::quantize(view.y),
    };
}

void AndroidPlayerClient::onInputSubmitted(shared::Direction const& direction)
{
    m_input_actions.submitted(direction);
}

void AndroidPlayerClient::servicePlatformEvents()
{
    drainEvents();
    if (m_app.destroyRequested || m_input.consumeStopRequest()) {
        stop();
        return;
    }
    if (m_running && !m_accepted && canRender()) {
        presentFrame();
    }
}

void AndroidPlayerClient::render()
{
    m_presentation_succeeded = false;
    servicePlatformEvents();
    if (m_running && !m_app.destroyRequested && !canRender()) {
        return;
    }
    if (m_app.destroyRequested || m_input.consumeStopRequest()) {
        stop();
        return;
    }
    if (!m_running || !canRender()) {
        return;
    }
    presentFrame();
}

void AndroidPlayerClient::presentFrame()
{
    client::WorldLoadingStatus const loading_status = worldLoadingStatus();
    bool const loading = loading_status.state != client::WorldLoadingState::Playing;
    if (m_frame_completion_tracking_enabled != loading) {
        m_renderer->setFrameCompletionTracking(loading);
        m_frame_completion_tracking_enabled = loading;
    }
    if (!loading) {
        m_loading_prompt_frame_target.reset();
        m_loading_prompt_frame_completed = false;
    }
    if (loading_status.state == client::WorldLoadingState::Playing) {
        float look_horizontal = 0.0F;
        float look_vertical = 0.0F;
        if (m_input.consumeLookDelta(look_horizontal, look_vertical)) {
            static_cast<void>(m_look_camera.rotate(
                static_cast<double>(look_horizontal) * 0.15,
                static_cast<double>(-look_vertical) * 0.15
            ));
        }
        if (m_input.consumeCameraPerspectiveCycleRequest()) {
            m_camera_perspective = client::nextCameraPerspective(m_camera_perspective);
        }
    }
    updateLoadingPresentation(loading_status);
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    std::optional<client::PlayerPresentationPosition> const local_position = predictedLocalPresentation(now);
    if (local_position && !terrainLodWorld()) {
        refreshPreviewTerrain(*local_position);
    }
    double camera_distance = client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
    if (local_position.has_value()) {
        camera_distance = maximumUnobstructedCameraDistance(*local_position);
        client::PlayerCameraView const camera_view = client::resolveLocalPlayerCamera(
            *local_position,
            m_look_camera.pose().angles,
            m_camera_perspective,
            camera_distance
        );
        static_cast<void>(m_camera.setPosition(camera_view.pose.position));
        static_cast<void>(m_camera.setAngles(camera_view.pose.angles));
    }

    std::vector<client::PlayerRenderData> players;
    players.reserve(m_world.players().size());
    for (shared::Player const& player : m_world.players()) {
        if (!client::shouldRenderPlayerBody(player, m_local_character, m_camera_perspective)) {
            continue;
        }
        if (auto const position = m_player_presentation.sample(player.ch, now)) {
            players.push_back({
                .x = static_cast<float>(position->x),
                .y = static_cast<float>(position->y),
                .color = client::playerPaletteColor(player.palette_index),
                .z = static_cast<float>(position->z),
                .render_on_top = player.ch == m_local_character
                    && m_camera_perspective != client::CameraPerspective::FirstPerson
                    && camera_distance <= 0.0,
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
            input.flight_enabled = player->movement_capabilities.allows(shared::MovementCapability::Flight);
            input.collision_bypass_enabled = player->movement_capabilities.allows(
                shared::MovementCapability::CollisionBypass
            );
        }
        client::CameraAngles const angles = m_camera.pose().angles;
        input.camera_yaw_degrees = static_cast<float>(angles.yaw_degrees);
        input.camera_pitch_degrees = static_cast<float>(angles.pitch_degrees);
        input.camera_roll_degrees = static_cast<float>(angles.roll_degrees);
        return input;
    }();
    if (loading_status.state == client::WorldLoadingState::Playing
        && m_input.consumeDebugHudToggleRequest()) {
        m_debug_hud_enabled = !m_debug_hud_enabled;
        m_renderer->setDebugHudEnabled(m_debug_hud_enabled);
    }
    m_terrain_lod.update(
        terrainLodWorld(), authoritativeWorld(), predictedLocalPlayer(), *m_renderer,
        std::chrono::steady_clock::now() + ANDROID_PREVIEW_UPLOAD_TIME_BUDGET
    );
    client::CameraProjection terrain_projection = m_camera.projection();
    if (m_terrain_lod.active()) {
        terrain_projection.far_plane = 50'000.0;
        clearPreviewTerrain(true);
    } else if (!terrainLodWorld()) {
        processPreviewTerrain();
    }
    m_renderer->setCamera(m_camera.pose(), terrain_projection);
    bool const presentation_succeeded = m_renderer->render(
        players,
        debug_hud_input,
        m_density_scale,
        std::chrono::steady_clock::now() + ANDROID_PRESENT_WAIT_BUDGET
    );
    m_presentation_succeeded = presentation_succeeded;
    client::RendererRuntimeInfo const runtime = m_renderer->runtimeInfo();
    reportTerrainPhase(runtime);
    if (presentation_succeeded && runtime.terrain_lod_active
        && m_terrain_lod_reported_columns != runtime.terrain_lod_actual_column_count) {
        __android_log_print(
            ANDROID_LOG_INFO, "MinecraftCloneLOD",
            "presented model=%s leaves=%u actual_columns=%u faces=%u submitted=%llu",
            shared::TERRAIN_LOD_MODEL.data(), runtime.terrain_lod_leaf_count,
            runtime.terrain_lod_actual_column_count, runtime.chunk_draw_face_count,
            static_cast<unsigned long long>(runtime.submitted_frame_count)
        );
        if (auto const* actual = authoritativeWorld()) {
            __android_log_print(
                ANDROID_LOG_INFO, "MinecraftCloneLOD", "source chunks=%llu surface_tiles=%llu",
                static_cast<unsigned long long>(actual->residentChunkCount()),
                static_cast<unsigned long long>(actual->surfaceMetadataTileCount())
            );

        }
        m_terrain_lod_reported_columns = runtime.terrain_lod_actual_column_count;
    }
    if (loading_status.state == client::WorldLoadingState::ReadyForStart) {
        client::RendererFrameProgress const frame_progress = m_renderer->frameProgress();
        if (m_loading_prompt_frame_target.has_value()
            && frame_progress.submitted.has_value()
            && frame_progress.submitted->swapchain_generation
                != m_loading_prompt_frame_target->swapchain_generation) {
            m_loading_prompt_frame_target.reset();
            m_loading_prompt_frame_completed = false;
        }
        if (!m_loading_prompt_frame_target.has_value()
            && presentation_succeeded
            && frame_progress.submitted.has_value()) {
            m_loading_prompt_frame_target = frame_progress.submitted;
        }
        if (!m_loading_prompt_frame_completed
            && m_loading_prompt_frame_target.has_value()
            && frame_progress.completed.has_value()
            && frame_progress.completed->swapchain_generation
                == m_loading_prompt_frame_target->swapchain_generation
            && frame_progress.completed->sequence >= m_loading_prompt_frame_target->sequence) {
            static_cast<void>(markLoadingPromptRendered(std::chrono::steady_clock::now()));
            m_loading_prompt_frame_completed = true;
        }
    } else {
        m_loading_prompt_frame_target.reset();
        m_loading_prompt_frame_completed = false;
    }
    if (m_input.consumeReloadRequest()) {
        m_renderer->hotReload();
    }
}

void AndroidPlayerClient::reportTerrainPhase(client::RendererRuntimeInfo const& runtime)
{
    static constexpr std::chrono::seconds LOG_INTERVAL{1};
    static constexpr uint32_t MAXIMUM_RECORDS = 96U;
    auto const now = std::chrono::steady_clock::now();
    if (m_terrain_phase_log_records >= MAXIMUM_RECORDS
        || now - m_terrain_phase_last_logged < LOG_INTERVAL) {
        return;
    }
    m_terrain_phase_last_logged = now;
    ++m_terrain_phase_log_records;
    auto const identity = activeTerrainIdentity();
    auto const epoch = activeCoverageEpoch();
    auto const status = worldLoadingStatus();
    __android_log_print(
        ANDROID_LOG_INFO, "MinecraftClonePhase",
        "record=%u steady_ns=%lld renderer_generation=%llu source_ready=%d root_ready=%d "
        "session_identity_available=%d join_epoch_observable=0 coverage_epoch=%llu "
        "terrain_version=%u revision=%llu seed=%llu loading_state=%u full_ready=%d "
        "profiles=%u required=%u runtime_active=%d runtime_job_observable=0 runtime_capture_observable=0 "
        "renderer_active=%d upload_pending=%d uploaded_faces=%llu actual_columns=%u leaves=%u "
        "faces=%u draw_faces=%u preview_meshes=%u submitted=%llu presented=%d",
        m_terrain_phase_log_records,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()),
        static_cast<unsigned long long>(m_renderer_generation),
        terrainLodWorld() != nullptr, terrainLodRoot() != nullptr, identity.has_value(),
        static_cast<unsigned long long>(epoch.value_or(0U)),
        identity ? identity->terrain_version : 0U,
        static_cast<unsigned long long>(identity ? identity->revision : 0U),
        static_cast<unsigned long long>(identity ? identity->seed : 0U),
        static_cast<uint32_t>(status.state), status.playable_sphere_ready,
        status.resident_authoritative_columns, status.required_authoritative_columns,
        m_terrain_lod.active(), runtime.terrain_lod_active, runtime.terrain_lod_upload_pending,
        static_cast<unsigned long long>(runtime.terrain_lod_uploaded_face_count),
        runtime.terrain_lod_actual_column_count, runtime.terrain_lod_leaf_count,
        runtime.chunk_face_count, runtime.chunk_draw_face_count, runtime.height_tile_mesh_count,
        static_cast<unsigned long long>(runtime.submitted_frame_count), m_presentation_succeeded
    );
}

void AndroidPlayerClient::updateLoadingPresentation(client::WorldLoadingStatus const& status)
{
    if (status.state == client::WorldLoadingState::Playing) {
        if (m_loading_overlay_active) {
            m_renderer->clearMapImage();
            m_renderer->clearGuiText();
            m_renderer->setDebugHudEnabled(m_debug_hud_enabled);
            m_loading_map_snapshot.reset();
            m_loading_overlay_active = false;
        }
        return;
    }

    m_loading_overlay_active = true;
    m_renderer->setDebugHudEnabled(false);
    if (status.map_snapshot != m_loading_map_snapshot) {
        m_loading_map_snapshot = status.map_snapshot;
        auto const image = status.map_snapshot
            ? client::rasterizeGenerationMap(*status.map_snapshot)
            : std::nullopt;
        if (image) {
            static_cast<void>(m_renderer->setMapImage(image->width, image->height, image->rgba));
        } else {
            m_renderer->clearMapImage();
        }
    }

    static constexpr std::array<std::string_view, 8U> STAGE_LABELS{
        "Tectonics", "Coasts", "Initial drainage", "Provisional climate",
        "Terrain erosion", "Final climate", "Final drainage", "Atlas complete",
    };
    std::string text;
    if (status.state == client::WorldLoadingState::Failed) {
        text = "World preparation failed: " + status.error;
    } else if (status.state == client::WorldLoadingState::Cancelled) {
        text = "World preparation cancelled";
    } else if (status.state == client::WorldLoadingState::ReadyForStart) {
        text = "Press any key to start";
    } else if (status.state == client::WorldLoadingState::Starting) {
        text = "Starting...";
    } else if (status.atlas_ready) {
        text = "Preparing safe spawn and generated neighborhood";
    } else {
        uint32_t const stage = std::min(
            static_cast<uint32_t>(status.generation_stage),
            static_cast<uint32_t>(STAGE_LABELS.size() - 1U)
        );
        text = std::string{ STAGE_LABELS[stage] } + " " + std::to_string(status.generation_completed)
            + "/" + std::to_string(status.generation_total);
        if (!status.map_snapshot) {
            text += "\nWorld map pending its first generated stage";
        }
    }
    if (status.request_accepted_at != std::chrono::steady_clock::time_point{}) {
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - status.request_accepted_at
        );
        text += "\nElapsed " + std::to_string(elapsed.count() / 1'000) + "."
            + std::to_string((elapsed.count() / 100) % 10) + "s";
    }
    text += "\nInitial sphere " + std::to_string(status.initial_sphere_ready_chunks)
        + "/" + std::to_string(status.initial_sphere_required_chunks)
        + "  Playable sphere " + std::to_string(status.playable_sphere_ready_chunks)
        + "/" + std::to_string(status.playable_sphere_required_chunks);
    m_renderer->setGuiText(text);
}

double AndroidPlayerClient::maximumUnobstructedCameraDistance(
    client::PlayerPresentationPosition const& local_position
) const noexcept
{
    shared::SparseWorld const* const world = authoritativeWorld();
    if (m_camera_perspective == client::CameraPerspective::FirstPerson || world == nullptr) {
        return client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
    }

    glm::dvec3 const eye = client::localPlayerEyePosition(local_position);
    client::PlayerCameraView const intended_camera = client::resolveLocalPlayerCamera(
        local_position,
        m_look_camera.pose().angles,
        m_camera_perspective,
        client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE
    );
    glm::dvec3 const ray = (intended_camera.pose.position - eye)
        / client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE;
    auto const obstructed = [&](double const distance) {
        glm::dvec3 const probe = eye + ray * distance;
        shared::WorldCoordinate const coordinate{
            .x = static_cast<int64_t>(std::floor(probe.x)),
            .y = static_cast<int64_t>(std::floor(probe.y)),
            .z = static_cast<int64_t>(std::floor(probe.z)),
        };
        std::optional<shared::WorldCoordinate> const normalized = shared::WorldBounds::normalize(coordinate);
        if (!normalized) {
            return false;
        }
        shared::Chunk const* const chunk = world->residentChunk(
            shared::WorldBounds::chunkCoordinate(*normalized)
        );
        if (chunk == nullptr) {
            return false;
        }
        std::optional<shared::Block> const block = chunk->blockAt(
            shared::WorldBounds::blockCoordinate(*normalized)
        );
        if (block && shared::isSolidBlock(*block)) {
            return true;
        }
        return false;
    };
    if (obstructed(0.0)) {
        return 0.0;
    }
    return client::maximumUnobstructedCameraDistance(
        client::MAX_LOCAL_PLAYER_CAMERA_DISTANCE,
        CAMERA_OBSTRUCTION_SAMPLE_COUNT,
        CAMERA_OBSTRUCTION_BINARY_STEPS,
        CAMERA_OBSTRUCTION_MARGIN,
        obstructed
    );
}

void AndroidPlayerClient::handleAppCommand(android_app* const app, int32_t const command)
{
    auto* const self = static_cast<AndroidPlayerClient*>(app->userData);
    if (self != nullptr) {
        try {
            self->onAppCommand(command);
        } catch (std::exception const& error) {
            try {
                CORE_ERROR("Android app command failed: {}", error.what());
            } catch (...) {
            }
            self->stop();
        } catch (...) {
            try {
                CORE_ERROR("Android app command failed with an unknown exception");
            } catch (...) {
            }
            self->stop();
        }
    }
}

int32_t AndroidPlayerClient::handleInputEvent(android_app* const app, AInputEvent* const event)
{
    auto* const self = static_cast<AndroidPlayerClient*>(app->userData);
    if (self == nullptr) {
        return 0;
    }
    try {
        return self->handleGameInputEvent(event);
    } catch (std::exception const& error) {
        try {
            CORE_ERROR("Android input event failed: {}", error.what());
        } catch (...) {
        }
        self->stop();
    } catch (...) {
        try {
            CORE_ERROR("Android input event failed with an unknown exception");
        } catch (...) {
        }
        self->stop();
    }
    return 1;
}

int32_t AndroidPlayerClient::handleGameInputEvent(AInputEvent const* const event)
{
    client::WorldLoadingStatus const loading_status = worldLoadingStatus();
    bool const playing = loading_status.state == client::WorldLoadingState::Playing;
    int32_t const event_type = AInputEvent_getType(event);
    if (event_type == AINPUT_EVENT_TYPE_KEY) {
        int32_t const action = AKeyEvent_getAction(event);
        int32_t const key_code = AKeyEvent_getKeyCode(event);
        bool const key_is_trackable = key_code >= 0
            && key_code < static_cast<int32_t>(m_loading_transition_keys.size());
        if (key_is_trackable) {
            uint32_t const key_index = static_cast<uint32_t>(key_code);
            if (m_loading_transition_keys.test(key_index)) {
                if (action == AKEY_EVENT_ACTION_UP) {
                    m_loading_transition_keys.reset(key_index);
                }
                return 1;
            }
        }
        if (!playing) {
            if (key_code == AKEYCODE_BACK || key_code == AKEYCODE_ESCAPE) {
                return m_input.handle(event) != 0 ? 1 : 0;
            }
            if (action == AKEY_EVENT_ACTION_DOWN) {
                if (key_is_trackable) {
                    m_loading_transition_keys.set(static_cast<uint32_t>(key_code));
                }
                if (loading_status.state == client::WorldLoadingState::ReadyForStart) {
                    static_cast<void>(acceptLoadingStartInput(std::chrono::steady_clock::now()));
                }
            }
            return 1;
        }
        if (key_code == AKEYCODE_F6 || key_code == AKEYCODE_6) {
            if (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP) {
                m_input_actions.observeCycle(action == AKEY_EVENT_ACTION_DOWN);
            }
            return 1;
        }
        bool const vertical_handled = updateVerticalInput(event);
        return m_input.handle(event) != 0 || vertical_handled ? 1 : 0;
    }

    bool const is_touch_event = event_type == AINPUT_EVENT_TYPE_MOTION
        && (static_cast<uint32_t>(AInputEvent_getSource(event))
            & static_cast<uint32_t>(AINPUT_SOURCE_TOUCHSCREEN)) != 0U;
    if (is_touch_event && (!playing || m_loading_touch_pointer_mask != 0U)) {
        int32_t const action = AMotionEvent_getAction(event);
        int32_t const action_mask = action & AMOTION_EVENT_ACTION_MASK;
        if (!playing
            && loading_status.state == client::WorldLoadingState::ReadyForStart
            && (action_mask == AMOTION_EVENT_ACTION_DOWN
                || action_mask == AMOTION_EVENT_ACTION_POINTER_DOWN)) {
            static_cast<void>(acceptLoadingStartInput(std::chrono::steady_clock::now()));
        }
        consumeLoadingTouch(event);
        return 1;
    }
    if (!playing) {
        return 1;
    }
    return m_input.handle(event) != 0 ? 1 : 0;
}

void AndroidPlayerClient::consumeLoadingTouch(AInputEvent const* const event)
{
    int32_t const action = AMotionEvent_getAction(event);
    int32_t const action_mask = action & AMOTION_EVENT_ACTION_MASK;
    if (action_mask == AMOTION_EVENT_ACTION_CANCEL) {
        m_loading_touch_pointer_mask = 0U;
        return;
    }
    if (action_mask != AMOTION_EVENT_ACTION_DOWN
        && action_mask != AMOTION_EVENT_ACTION_POINTER_DOWN
        && action_mask != AMOTION_EVENT_ACTION_UP
        && action_mask != AMOTION_EVENT_ACTION_POINTER_UP) {
        return;
    }

    uint32_t const action_index = static_cast<uint32_t>(
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT
    );
    int32_t const pointer_id = AMotionEvent_getPointerId(event, action_index);
    if (pointer_id < 0 || pointer_id >= 32) {
        return;
    }
    uint32_t const pointer_mask = 1U << static_cast<uint32_t>(pointer_id);
    if (action_mask == AMOTION_EVENT_ACTION_DOWN || action_mask == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        m_loading_touch_pointer_mask |= pointer_mask;
    } else {
        m_loading_touch_pointer_mask &= ~pointer_mask;
    }
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
        m_ascend_input.setPressed(pressed);
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
        m_resumed = false;
        [[fallthrough]];
    case APP_CMD_LOST_FOCUS:
        CORE_INFO("Android rendering paused by app command {}", command);
        m_has_focus = false;
        m_input.clear();
        m_loading_touch_pointer_mask = 0U;
        m_ascend_input.clear();
        m_input_actions.reset();
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

void AndroidPlayerClient::onConnectionStateReset()
{
    m_input.clear();
    m_loading_transition_keys.reset();
    m_loading_touch_pointer_mask = 0U;
    m_loading_prompt_frame_target.reset();
    m_loading_prompt_frame_completed = false;
    m_ascend_input.clear();
    m_input_actions.reset();
    m_descend_pressed = false;
    clearPreviewTerrain(true);
    m_terrain_lod.reset(m_renderer.get());
    m_terrain_lod_reported_columns.reset();
}

void AndroidPlayerClient::refreshPreviewTerrain(
    client::PlayerPresentationPosition const& local_position
)
{
    std::optional<uint32_t> const radius = renderDistance();
    if (!radius) {
        return;
    }
    client::CameraProjection projection = m_camera.projection();
    double const far_plane = static_cast<double>(*radius) * shared::HEIGHT_TILE_SIDE_LENGTH
        + shared::WorldExtent::DEPTH;
    if (projection.far_plane != far_plane) {
        projection.far_plane = far_plane;
        static_cast<void>(m_camera.setProjection(projection));
    }

    int32_t const player_x = static_cast<int32_t>(std::floor(local_position.x));
    int32_t const player_y = static_cast<int32_t>(std::floor(local_position.y));
    shared::HeightTileKey const center = shared::normalizeHeightTileKey({
        .x = floorDivideByHeightTileSide(player_x),
        .y = floorDivideByHeightTileSide(player_y),
    });
    bool const center_changed = !m_preview_interest_center || *m_preview_interest_center != center;
    bool const radius_changed = !m_preview_interest_radius || *m_preview_interest_radius != *radius;
    if (center_changed || radius_changed) {
        std::optional<shared::HeightTileKey> const previous_center = m_preview_interest_center;
        m_preview_interest_center = center;
        m_preview_interest_radius = radius;
        if (previous_center && !radius_changed) {
            client::PlayerPreviewInterestDelta const delta = client::playerPreviewInterestDelta(
                *previous_center, center, *radius
            );
            for (shared::HeightTileKey const key : delta.removals) {
                m_preview_lod.erase(key);
                if (m_visible_preview_mesh_bases.contains(key)) {
                    m_preview_mesh_work.enqueue(key);
                }
            }
            for (shared::HeightTileKey const key : delta.additions) {
                enqueuePreviewMeshIfCurrent(key);
            }
        } else {
            m_preview_lod.clear();
            for (auto const& [key, mesh] : m_visible_preview_mesh_bases) {
                static_cast<void>(mesh);
                m_preview_mesh_work.enqueue(key);
            }
        }
        m_preview_mesh_work.retain([this](shared::HeightTileKey const key) {
            return isPreviewTileInInterest(key) || m_visible_preview_mesh_bases.contains(key);
        });
    }
    double const tile_origin_x = static_cast<double>(center.x) * shared::HEIGHT_TILE_SIDE_LENGTH;
    double const tile_origin_y = static_cast<double>(center.y) * shared::HEIGHT_TILE_SIDE_LENGTH;
    double constexpr PLAYER_WIDTH = static_cast<double>(shared::World::PLAYER_WIDTH_SUBCELLS)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    double constexpr PLAYER_HEIGHT = static_cast<double>(shared::World::PLAYER_HEIGHT_SUBCELLS)
        / static_cast<double>(shared::SUBCELLS_PER_CELL);
    shared::HeightTileSurfaceBounds const viewer_bounds{
        .min_x_blocks = local_position.x - tile_origin_x,
        .max_x_blocks = local_position.x - tile_origin_x + PLAYER_WIDTH,
        .min_y_blocks = local_position.y - tile_origin_y,
        .max_y_blocks = local_position.y - tile_origin_y + PLAYER_WIDTH,
        .min_z_blocks = local_position.z,
        .max_z_blocks = local_position.z + PLAYER_HEIGHT,
    };
    client::RendererRuntimeInfo const runtime = m_renderer->runtimeInfo();
    shared::HeightTileSurfaceProjection const mesh_projection{
        .vertical_fov_degrees = projection.vertical_fov_degrees,
        .viewport_width_pixels = runtime.width,
        .viewport_height_pixels = runtime.height,
    };
    client::PlayerPreviewLodRefresh const refresh = m_preview_lod.refresh(
        center, viewer_bounds, mesh_projection
    );
    for (shared::HeightTileKey const key : refresh.changed_keys) {
        enqueuePreviewMeshIfCurrent(key);
        enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, -1, 0));
        enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, 1, 0));
        enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, 0, -1));
        enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, 0, 1));
    }
}

void AndroidPlayerClient::processPreviewTerrain()
{
    client::drainPreviewTileChanges(
        heightTileResidency(),
        ANDROID_PREVIEW_CHANGE_BUDGET,
        m_preview_interest_center.has_value() && m_preview_interest_radius.has_value(),
        [this](client::HeightTileChange const change) {
            if (change.kind == client::HeightTileChangeKind::Remove) {
                m_preview_lod.erase(change.key);
            }
            m_preview_mesh_work.enqueueChange(change, [this](shared::HeightTileKey const key) {
                return (isPreviewTileInInterest(key) && heightTileResidency().resident(key))
                    || m_visible_preview_mesh_bases.contains(key);
            });
        }
    );

    auto const work_deadline = std::chrono::steady_clock::now()
        + ANDROID_PREVIEW_MESH_TIME_BUDGET;
    for (uint32_t completed = 0U;
         completed < ANDROID_PREVIEW_MESH_BUDGET
         && std::chrono::steady_clock::now() < work_deadline;
         ++completed) {
        std::optional<client::HeightTileKey> const next = m_preview_mesh_work.take();
        if (!next) {
            break;
        }
        client::HeightTileKey const key = *next;
        client::HeightTileHandle const tile = heightTileResidency().resident(key);
        if (!tile || !isPreviewTileInInterest(key)) {
            m_preview_lod.erase(key);
            if (m_visible_preview_mesh_bases.erase(key) > 0U) {
                static_cast<void>(m_renderer->removeHeightTileMesh({ key.x, key.y }));
                m_visible_preview_mesh_seam_bridges.erase(key);
                enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, -1, 0));
                enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, 1, 0));
                enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, 0, -1));
                enqueuePreviewMeshIfCurrent(offsetHeightTileKey(key, 0, 1));
            }
            continue;
        }

        uint16_t minimum = std::numeric_limits<uint16_t>::max();
        uint16_t maximum = 0U;
        for (uint16_t const height : tile->heights()) {
            minimum = std::min(minimum, height);
            maximum = std::max(maximum, height);
        }
        static_cast<void>(m_preview_lod.selectDetail(key, minimum, maximum));

        std::array<client::HeightTileKey, 4> const adjacent_keys{
            offsetHeightTileKey(key, -1, 0),
            offsetHeightTileKey(key, 1, 0),
            offsetHeightTileKey(key, 0, -1),
            offsetHeightTileKey(key, 0, 1),
        };
        std::array<client::HeightTileHandle, 4> neighbors;
        std::array<shared::HeightTileSurfaceDetail, 4> neighbor_details{
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
            shared::HeightTileSurfaceDetail::Fine,
        };
        for (uint32_t index = 0U; index < static_cast<uint32_t>(neighbors.size()); ++index) {
            neighbors[index] = heightTileResidency().resident(adjacent_keys[index]);
            if (neighbors[index]) {
                neighbor_details[index] = m_preview_lod.detail(adjacent_keys[index]).value_or(
                    shared::HeightTileSurfaceDetail::Fine
                );
            }
        }
        shared::HeightTileSurfaceMesh mesh = client::buildPreviewMesh({
            .tile = tile,
            .neighbors = neighbors,
            .detail = m_preview_lod.detail(key).value_or(shared::HeightTileSurfaceDetail::Fine),
            .neighbor_details = neighbor_details,
        });

        client::PreviewMeshSeamBridgeSet center_bridges;
        if (auto const bridges = m_visible_preview_mesh_seam_bridges.find(key);
            bridges != m_visible_preview_mesh_seam_bridges.end()) {
            center_bridges = bridges->second;
        }
        std::array<client::PreviewMeshSeamNeighborSnapshot, 4> seam_neighbors;
        for (uint32_t index = 0U; index < static_cast<uint32_t>(adjacent_keys.size()); ++index) {
            auto const base = m_visible_preview_mesh_bases.find(adjacent_keys[index]);
            if (base == m_visible_preview_mesh_bases.end()) {
                continue;
            }
            auto const bridges = m_visible_preview_mesh_seam_bridges.find(adjacent_keys[index]);
            seam_neighbors[index] = {
                .mesh = &base->second,
                .bridges = bridges == m_visible_preview_mesh_seam_bridges.end() ? nullptr : &bridges->second,
                .surface = neighbors[index] ? &neighbors[index]->surface() : nullptr,
            };
        }
        client::PreviewMeshSeamPublicationPlan plan = client::planPreviewMeshSeamPublication(
            mesh, center_bridges, seam_neighbors, &tile->surface()
        );
        client::PreviewMeshSeamPublicationBatch const batch = client::buildPreviewMeshSeamPublicationBatch(
            plan, seam_neighbors
        );
        auto const upload_deadline = std::chrono::steady_clock::now()
            + ANDROID_PREVIEW_UPLOAD_TIME_BUDGET;
        if (!m_renderer->upsertHeightTileMeshes(
                std::span<shared::HeightTileSurfaceMesh const>{ batch.meshes.data(), batch.count },
                upload_deadline
            )) {
            m_preview_mesh_work.enqueue(key);
            break;
        }

        bool const first_mesh = m_visible_preview_mesh_bases.empty();
        m_visible_preview_mesh_bases.insert_or_assign(key, std::move(mesh));
        m_visible_preview_mesh_seam_bridges.insert_or_assign(key, std::move(plan.center_bridges));
        for (uint32_t index = 0U; index < static_cast<uint32_t>(adjacent_keys.size()); ++index) {
            if (plan.changed_neighbors[index]) {
                m_visible_preview_mesh_seam_bridges.insert_or_assign(
                    adjacent_keys[index], std::move(plan.neighbor_bridges[index])
                );
            }
        }
        if (first_mesh) {
            CORE_INFO(
                "Android terrain mesh published at tile ({}, {}); renderer now has {} height-tile meshes",
                key.x, key.y, m_renderer->runtimeInfo().height_tile_mesh_count
            );
        }
    }
}

void AndroidPlayerClient::enqueuePreviewMeshIfCurrent(shared::HeightTileKey key)
{
    key = shared::normalizeHeightTileKey(key);
    if ((isPreviewTileInInterest(key) && heightTileResidency().resident(key))
        || m_visible_preview_mesh_bases.contains(key)) {
        m_preview_mesh_work.enqueue(key);
    }
}

bool AndroidPlayerClient::isPreviewTileInInterest(shared::HeightTileKey const key) const
{
    return m_preview_interest_center && m_preview_interest_radius
        && client::playerPreviewTileWithinInterest(
            *m_preview_interest_center, key, *m_preview_interest_radius
        );
}

void AndroidPlayerClient::clearPreviewTerrain(bool const remove_renderer_meshes) noexcept
{
    if (remove_renderer_meshes && m_renderer != nullptr) {
        for (auto const& [key, mesh] : m_visible_preview_mesh_bases) {
            static_cast<void>(mesh);
            static_cast<void>(m_renderer->removeHeightTileMesh({ key.x, key.y }));
        }
    }
    m_preview_mesh_work.clear();
    m_preview_lod.clear();
    m_preview_interest_center.reset();
    m_preview_interest_radius.reset();
    m_visible_preview_mesh_bases.clear();
    m_visible_preview_mesh_seam_bridges.clear();
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
    ++m_renderer_generation;
    m_renderer->setDebugHudEnabled(m_debug_hud_enabled);
    m_loading_map_snapshot.reset();
    m_loading_overlay_active = false;
    m_loading_prompt_frame_target.reset();
    m_loading_prompt_frame_completed = false;
    m_frame_completion_tracking_enabled = false;
    m_visible_preview_mesh_bases.clear();
    m_visible_preview_mesh_seam_bridges.clear();
    m_preview_lod.clear();
    CORE_INFO(
        "Android renderer created for {}x{} surface",
        ANativeWindow_getWidth(m_app.window),
        ANativeWindow_getHeight(m_app.window)
    );
}

void AndroidPlayerClient::destroyWindowResources()
{
    m_terrain_lod.reset(m_renderer.get());
    m_terrain_lod_reported_columns.reset();
    if (m_renderer != nullptr) {
        CORE_INFO("Destroying Android renderer before releasing its native window");
        if (m_frame_completion_tracking_enabled) {
            m_renderer->setFrameCompletionTracking(false);
        }
    }
    m_frame_completion_tracking_enabled = false;
    m_loading_prompt_frame_target.reset();
    m_loading_prompt_frame_completed = false;
    for (auto const& [key, mesh] : m_visible_preview_mesh_bases) {
        static_cast<void>(mesh);
        m_preview_mesh_work.enqueue(key);
    }
    m_visible_preview_mesh_bases.clear();
    m_visible_preview_mesh_seam_bridges.clear();
    m_preview_lod.clear();
    m_renderer.reset();
    m_input.clear();
    m_loading_touch_pointer_mask = 0U;
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
    m_ascend_input.clear();
    m_input_actions.reset();
    m_descend_pressed = false;
    m_loading_transition_keys.reset();
    m_loading_touch_pointer_mask = 0U;
    try {
        destroyWindowResources();
    } catch (...) {
        m_preview_mesh_work.clear();
        m_preview_lod.clear();
        m_preview_interest_center.reset();
        m_preview_interest_radius.reset();
        m_visible_preview_mesh_bases.clear();
        m_visible_preview_mesh_seam_bridges.clear();
        m_renderer.reset();
        m_input.clear();
    }
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
