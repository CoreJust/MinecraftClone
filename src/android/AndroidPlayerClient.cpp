#include "AndroidPlayerClient.hpp"

#include <client/CameraController.hpp>
#include <client/CameraObstruction.hpp>
#include <client/GenerationMapPresentation.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/PreviewMeshing.hpp>
#include <client/UiPresentation.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <core/IO/Log.hpp>

#include <android/asset_manager.h>
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
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

struct InteractionControlLayout final {
    std::array<std::array<float, 4U>, 4U> controls{};
    float footer_y = 0.0F;
    float text_scale = 1.0F;
};

InteractionControlLayout interactionControlLayout(uint32_t width, uint32_t height, float density)
{
    auto const hud = client::layoutDebugHud(width, height, density);
    auto const maps = client::layoutS2Map({}, width, height, false, true, density);
    float const margin = 8.0F * density;
    float const button_width = 88.0F * density;
    float const button_height = 48.0F * density;
    float const footer_y = hud.controls_y - 32.0F * density;
    float const top = maps.minimap_bounds[1] + maps.minimap_bounds[3] + margin;
    InteractionControlLayout result{.footer_y = footer_y, .text_scale = 0.5F * density};
    if (top + 2.0F * button_height + margin > footer_y - margin
        || 2.0F * button_width + 3.0F * margin > static_cast<float>(width)) {
        float const row_width = std::min(button_width, (static_cast<float>(width) - 5.0F * margin) / 4.0F);
        float const row_top = footer_y - button_height - margin;
        float const row_right = margin + 4.0F * row_width + 3.0F * margin;
        bool const overlaps_map_y = row_top < maps.minimap_bounds[1] + maps.minimap_bounds[3]
            && row_top + button_height > maps.minimap_bounds[1];
        if (row_width < button_height || row_top < margin
            || (overlaps_map_y && row_right > maps.minimap_bounds[0])) {
            return result;
        }
        for (size_t index = 0U; index < result.controls.size(); ++index) {
            result.controls[index] = {margin + static_cast<float>(index) * (row_width + margin),
                row_top, row_width, button_height};
        }
        return result;
    }
    float const left = static_cast<float>(width) - 2.0F * button_width - 2.0F * margin;
    for (size_t index = 0U; index < result.controls.size(); ++index) {
        result.controls[index] = {left + static_cast<float>(index % 2U) * (button_width + margin),
            top + static_cast<float>(index / 2U) * (button_height + margin), button_width, button_height};
    }
    return result;
}

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
    if (!loadClientMusic([asset_manager = app.activity->assetManager](std::string_view name) {
            std::string const asset_path = "audio/" + std::string{name};
            std::unique_ptr<AAsset, decltype(&AAsset_close)> asset{
                AAssetManager_open(asset_manager, asset_path.c_str(), AASSET_MODE_BUFFER), AAsset_close};
            if (!asset) {
                throw std::runtime_error("Cannot open Android music asset");
            }
            auto const size = AAsset_getLength64(asset.get());
            if (size <= 0 || size > 8 * 1024 * 1024) {
                throw std::runtime_error("Invalid Android music asset size");
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            size_t offset = 0;
            while (offset < bytes.size()) {
                int const count = AAsset_read(asset.get(), bytes.data() + offset, bytes.size() - offset);
                if (count <= 0) {
                    throw std::runtime_error("Cannot read Android music asset");
                }
                offset += static_cast<size_t>(count);
            }
            return bytes;
        })) {
        CORE_ERROR("Android background music assets could not be loaded");
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
    if (worldLoadingStatus().state != client::WorldLoadingState::Playing || m_world_map_open) {
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
    if (!m_frame_completion_tracking_enabled) {
        m_renderer->setFrameCompletionTracking(true);
        m_frame_completion_tracking_enabled = true;
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
        auto const progress = m_renderer->frameProgress();
        input.completed_frame = progress.completed;
        input.completed_frame_observed_at = progress.completed_observed_at;
        input.ready_terrain_columns = loading_status.resident_authoritative_columns;
        input.required_terrain_columns = loading_status.required_authoritative_columns;
        if (auto const* world = authoritativeWorld()) {
            input.loaded_dense_chunks_available = true;
            input.loaded_dense_chunks = world->residentChunkCount();
        }
        if (auto const availability = currentMaterializedInterestChunkAvailability()) {
            input.current_interest_chunks_available = true;
            input.current_interest_resident_chunks = availability->resident_count;
            input.current_interest_required_chunks = availability->required_count;
        }
        if (now - m_hud_sampled_at >= std::chrono::seconds{1}) {
            m_hud_sampled_at = now;
            m_hud_rss_bytes.reset();
            if (auto* file = std::fopen("/proc/self/status", "r")) {
                std::array<char, 256U> line{};
                while (std::fgets(line.data(), static_cast<int>(line.size()), file)) {
                    unsigned long long kib = 0U;
                    if (std::sscanf(line.data(), "VmRSS: %llu kB", &kib) == 1) {
                        m_hud_rss_bytes = static_cast<uint64_t>(kib) * 1'024U;
                        break;
                    }
                }
                std::fclose(file);
            }
        }
        input.client_memory_rss_bytes = m_hud_rss_bytes;
        auto const climate_world = terrainLodWorld();
        if (m_hud_climate_query.valid()
            && m_hud_climate_query.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            try {
                auto const value = m_hud_climate_query.get();
                if (m_hud_climate_world == climate_world && std::isfinite(value)) {
                    m_hud_temperature = value;
                    m_hud_temperature_observed_at = now;
                }
            } catch (std::exception const& error) {
                CORE_ERROR("Android HUD climate query failed: {}", error.what());
            }
        }
        if (m_hud_climate_world != climate_world) {
            m_hud_temperature.reset();
        }

        if (auto const player = m_world.playerByCharacter(m_local_character)) {
            input.player_x = static_cast<float>(shared::playerPositionX(*player));
            input.player_y = static_cast<float>(shared::playerPositionY(*player));
            input.player_z = static_cast<float>(shared::playerPositionZ(*player));
            shared::Direction const movement = m_input.direction();
            input.selected_speedup = movement.speedup;
            input.acceleration_enabled = movement.accelerated;
            input.speedup = movement.accelerated ? movement.speedup : 1U;
            if (climate_world && !m_hud_climate_query.valid()
                && now - m_hud_climate_sampled_at >= std::chrono::seconds{1}) {
                m_hud_climate_sampled_at = now;
                m_hud_climate_world = climate_world;
                double const x = shared::playerPositionX(*player);
                double const y = shared::playerPositionY(*player);
                try {
                    m_hud_climate_query = std::async(std::launch::async, [climate_world, x, y] {
                        return static_cast<float>(climate_world->sampleClimate(x, y).temperature_celsius);
                    });
                } catch (std::system_error const& error) {
                    m_hud_temperature.reset();
                    CORE_ERROR("Android HUD climate worker unavailable: {}", error.what());
                }
            }
            if (now - m_hud_temperature_observed_at <= std::chrono::seconds{2}) {
                input.temperature_celsius = m_hud_temperature;
            }
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
    auto terrain_world = terrainLodWorld();
    auto prepared_root = terrain_world && !m_terrain_lod.active() ? terrainLodRoot() : nullptr;
    m_terrain_lod.update(
        std::move(terrain_world), authoritativeWorld(), predictedLocalPlayer(), *m_renderer,
        std::chrono::steady_clock::now() + ANDROID_PREVIEW_UPLOAD_TIME_BUDGET,
        activeTerrainSession(), activeCoverageEpoch(), false,
        std::move(prepared_root)
    );
    client::CameraProjection terrain_projection = m_camera.projection();
    if (m_terrain_lod.active()) {
        terrain_projection.far_plane = 50'000.0;
        clearPreviewTerrain(true);
    } else if (!terrainLodWorld()) {
        processPreviewTerrain();
    }
    m_renderer->setCamera(m_camera.pose(), terrain_projection);
    updateMapPresentation();
    updateBlockInteractions(now);
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
    static constexpr uint32_t INITIAL_RECORDS = 96U;
    static constexpr uint32_t MAXIMUM_RECORDS = 192U;
    static constexpr std::chrono::seconds INITIAL_LOG_INTERVAL{1};
    static constexpr std::chrono::seconds EXTENDED_LOG_INTERVAL{5};
    auto const now = std::chrono::steady_clock::now();
    if (m_terrain_phase_log_records >= MAXIMUM_RECORDS) {
        return;
    }
    // Continue lower-frequency samples after source publication to expose later cold-start gates.
    std::chrono::seconds const log_interval = m_terrain_phase_log_records < INITIAL_RECORDS
        ? INITIAL_LOG_INTERVAL : EXTENDED_LOG_INTERVAL;
    if (now - m_terrain_phase_last_logged < log_interval) {
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
        "atlas_ready=%d spawn_ready=%d initial_chunks=%u initial_required=%u initial_ready=%d "
        "profiles=%u required=%u runtime_active=%d runtime_job_observable=1 runtime_capture_observable=1 "
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
        status.atlas_ready, status.safe_spawn_position.has_value(), status.initial_sphere_ready_chunks,
        status.initial_sphere_required_chunks, status.initial_sphere_ready,
        status.resident_authoritative_columns, status.required_authoritative_columns,
        m_terrain_lod.active(), runtime.terrain_lod_active, runtime.terrain_lod_upload_pending,
        static_cast<unsigned long long>(runtime.terrain_lod_uploaded_face_count),
        runtime.terrain_lod_actual_column_count, runtime.terrain_lod_leaf_count,
        runtime.chunk_face_count, runtime.chunk_draw_face_count, runtime.height_tile_mesh_count,
        static_cast<unsigned long long>(runtime.submitted_frame_count), m_presentation_succeeded
    );
    auto const progress = m_terrain_lod.progress();
    auto const player = predictedLocalPlayer();
    __android_log_print(
        ANDROID_LOG_INFO, "MinecraftCloneTerrainProgress",
        "record=%u player_present=%d dispatched=%llu completed=%llu failed=%llu captured=%llu published=%llu "
        "source_rejected=%llu plan_rejected=%llu plan_reason=%u source_changes=%llu actual_changes=%llu "
        "job_outstanding=%d job_ready=%d job_wall_ms=%llu candidate_pending=%d last_error=%u error_text=%.127s",
        m_terrain_phase_log_records, player.has_value(),
        static_cast<unsigned long long>(progress.dispatched), static_cast<unsigned long long>(progress.completed),
        static_cast<unsigned long long>(progress.failed), static_cast<unsigned long long>(progress.captured),
        static_cast<unsigned long long>(progress.published), static_cast<unsigned long long>(progress.source_rejected),
        static_cast<unsigned long long>(progress.plan_rejected), static_cast<uint32_t>(progress.last_plan_failure),
        static_cast<unsigned long long>(progress.source_changes),
        static_cast<unsigned long long>(progress.actual_world_changes), progress.job_outstanding, progress.job_ready,
        static_cast<unsigned long long>(progress.job_wall_ms), progress.candidate_pending,
        static_cast<uint32_t>(progress.last_error), progress.last_error_text.data()
    );
}

std::optional<shared::VoxelRay> AndroidPlayerClient::interactionEyeRay() const
{
    std::optional<shared::Player> const player = m_world.playerByCharacter(m_local_character);
    if (!player) {
        return std::nullopt;
    }
    glm::dvec3 const eye = client::localPlayerEyePosition(*player);
    glm::dvec3 const direction = m_look_camera.forward();
    return shared::VoxelRay{
        .origin_x = eye.x,
        .origin_y = eye.y,
        .origin_z = eye.z,
        .direction_x = direction.x,
        .direction_y = direction.y,
        .direction_z = direction.z,
    };
}

std::optional<shared::BlockTarget> AndroidPlayerClient::resolveInteractionTarget() const
{
    shared::SparseWorld const* const world = authoritativeWorld();
    std::optional<shared::VoxelRay> const ray = interactionEyeRay();
    if (!world || !ray) {
        return std::nullopt;
    }
    double const direction_length = std::hypot(ray->direction_x, ray->direction_y, ray->direction_z);
    if (!std::isfinite(direction_length) || direction_length <= 0.0) {
        return std::nullopt;
    }
    shared::VoxelRay transmitted_ray = *ray;
    transmitted_ray.direction_x = static_cast<double>(std::lround(ray->direction_x / direction_length * 32'767.0));
    transmitted_ray.direction_y = static_cast<double>(std::lround(ray->direction_y / direction_length * 32'767.0));
    transmitted_ray.direction_z = static_cast<double>(std::lround(ray->direction_z / direction_length * 32'767.0));
    shared::BlockTargetResult const result = shared::resolveInteractionTarget(
        *world,
        transmitted_ray,
        m_camera_perspective,
        m_world.mode() == shared::WorldMode::Flight
    );
    return result.status == shared::BlockTargetStatus::Hit ? result.target : std::nullopt;
}


shared::ServerBlockActionResultMessage const* AndroidPlayerClient::currentBlockActionStatus() const noexcept
{
    std::optional<shared::TerrainSessionIdentity> const session = activeTerrainSession();
    std::optional<shared::ServerBlockActionResultMessage> const& status = blockActionStatus();
    if (!session || !status || status->session != *session) {
        return nullptr;
    }
    return &*status;
}

void AndroidPlayerClient::clearPendingBlockActions() noexcept
{
    m_block_place_input = {};
    m_pending_block_actions.fill({});
    m_client_block_action_sequence = 1U;
    m_pending_game_mode.reset();
    m_initial_block_action_status_requested = false;
}

void AndroidPlayerClient::rememberBlockAction(
    uint32_t const sequence,
    shared::BlockActionKind const kind,
    std::optional<shared::WorldCoordinate> const target,
    shared::Block const material
)
{
    size_t const slot = static_cast<size_t>(sequence % PENDING_BLOCK_ACTION_PRESENTATIONS);
    m_pending_block_actions[slot] = {
        .sequence = sequence,
        .join_epoch = worldLoadingJoinEpoch(),
        .kind = kind,
        .target = target,
        .material = material,
    };
}

bool AndroidPlayerClient::submitTrackedBlockAction(
    shared::BlockActionKind const kind,
    shared::VoxelRay const& aim,
    std::optional<shared::WorldCoordinate> const target_hint,
    shared::Block const selected
)
{
    if (worldLoadingStatus().state != client::WorldLoadingState::Playing || !activeTerrainSession()) {
        return false;
    }
    uint32_t const sequence = m_client_block_action_sequence;
    bool const submitted = client::GameClient::submitBlockAction(kind, aim, target_hint, selected, m_camera_perspective);
    ++m_client_block_action_sequence;
    if (m_client_block_action_sequence == 0U) {
        m_client_block_action_sequence = 1U;
    }
    if (submitted) {
        rememberBlockAction(sequence, kind, target_hint, selected);
    }
    return submitted;
}

bool AndroidPlayerClient::submitTrackedGameMode(shared::GameMode const mode)
{
    if (worldLoadingStatus().state != client::WorldLoadingState::Playing || !activeTerrainSession()) {
        return false;
    }
    uint32_t const sequence = m_client_block_action_sequence;
    bool const submitted = GameClient::requestGameMode(mode);
    ++m_client_block_action_sequence;
    if (m_client_block_action_sequence == 0U) {
        m_client_block_action_sequence = 1U;
    }
    if (submitted) {
        rememberBlockAction(sequence, shared::BlockActionKind::RequestMode, std::nullopt, m_selected_block);
        m_pending_game_mode = std::pair{sequence, mode};
    }
    return submitted;
}

void AndroidPlayerClient::updateBlockActionInput(
    bool const left_pressed,
    bool const left_was_pressed,
    bool const right_pressed,
    bool const right_was_pressed,
    bool const cycle_block_pressed,
    bool const change_mode_pressed,
    bool const cycle_block_was_pressed,
    bool const change_mode_was_pressed,
    std::chrono::steady_clock::time_point const now
)
{
    shared::ServerBlockActionResultMessage const* status = currentBlockActionStatus();
    if (!status && !m_initial_block_action_status_requested && activeTerrainSession()) {
        m_initial_block_action_status_requested = true;
        static_cast<void>(submitTrackedBlockAction(
            shared::BlockActionKind::BreakRelease,
            {},
            std::nullopt,
            shared::Block::Stone
        ));
    }
    if (status && m_pending_game_mode && status->sequence != 0U
        && (status->sequence == m_pending_game_mode->first
            || shared::isNewerSequence(status->sequence, m_pending_game_mode->first))) {
        m_pending_game_mode.reset();
    }

    auto const action_allowed = [status](shared::InteractionAction const action) {
        return status && client::detail::interactionActionAllowed(status->effective_actions, action);
    };
    auto const material_allowed = [](uint8_t const mask, shared::Block const material) {
        return client::detail::interactionMaterialAllowed(mask, material);
    };

    if (cycle_block_pressed && !cycle_block_was_pressed && status) {
        if (std::optional<shared::Block> const next = client::detail::nextPlaceableBlock(
                status->place_materials,
                m_selected_block
            );
            next) {
            m_selected_block = *next;
        }
    }

    bool const mode_change_edge = change_mode_pressed && !change_mode_was_pressed
        && action_allowed(shared::InteractionAction::ChangeMode) && status && !m_pending_game_mode;
    if (mode_change_edge) {
        shared::GameMode const current_mode = status->mode;
        shared::GameMode next_mode = shared::GameMode::Spectator;
        switch (current_mode) {
        case shared::GameMode::Spectator: next_mode = shared::GameMode::Creative; break;
        case shared::GameMode::Creative: next_mode = shared::GameMode::Survival; break;
        case shared::GameMode::Survival: next_mode = shared::GameMode::Spectator; break;
        }
        releaseActiveBreakAction();
        static_cast<void>(submitTrackedGameMode(next_mode));
        return;
    }

    if (m_pending_game_mode) {
        releaseActiveBreakAction();
        return;
    }
    std::optional<shared::VoxelRay> const ray = interactionEyeRay();
    std::optional<shared::BlockTarget> const target = resolveInteractionTarget();
    bool const can_place = status && target && ray && action_allowed(shared::InteractionAction::Place)
        && material_allowed(status->place_materials, m_selected_block);
    bool const can_break_target = status && target && action_allowed(shared::InteractionAction::Break)
        && material_allowed(status->break_materials, target->block);
    bool const break_target_resumed = m_input_actions.observeBreakTarget(
        left_pressed, target.has_value() && ray.has_value()
    );
    bool const submit_place = client::detail::updateBlockPlaceInput(
        m_block_place_input, right_pressed, right_pressed && !right_was_pressed, can_place, now
    );
    if (right_pressed) {
        releaseActiveBreakAction(false);
        if (submit_place) {
            static_cast<void>(submitTrackedBlockAction(
                shared::BlockActionKind::PlacePress,
                *ray,
                target->cell,
                m_selected_block
            ));
        }
        return;
    }

    client::detail::BlockBreakInputActions const actions = client::detail::updateBlockBreakInput(m_block_break_input, {
        .held = left_pressed,
        .fresh_press = left_pressed && (!left_was_pressed || break_target_resumed),
        .permitted = can_break_target,
        .target = target ? std::optional<shared::WorldCoordinate>{target->cell} : std::nullopt,
        .now = now,
    });
    if (actions.submit_release) {
        static_cast<void>(submitTrackedBlockAction(
            shared::BlockActionKind::BreakRelease,
            {},
            std::nullopt,
            shared::Block::Stone
        ));
    }
    if (actions.submit_hold && target && ray) {
        static_cast<void>(submitTrackedBlockAction(
            shared::BlockActionKind::BreakHold,
            *ray,
            target->cell,
            target->block
        ));
    }
}


void AndroidPlayerClient::releaseActiveBreakAction(bool const cancel_placement)
{
    m_input_actions.cancelBreakTargetIntent();
    if (cancel_placement) {
        m_block_place_input = {};
    }
    if (!m_block_break_input.active) {
        return;
    }
    m_block_break_input.active = false;
    m_block_break_input.target.reset();
    if (worldLoadingStatus().state == client::WorldLoadingState::Playing) {
        static_cast<void>(submitTrackedBlockAction(
            shared::BlockActionKind::BreakRelease,
            {},
            std::nullopt,
            shared::Block::Stone
        ));
    }
}

void AndroidPlayerClient::updateBlockInteractions(std::chrono::steady_clock::time_point const now)
{
    bool const active = worldLoadingStatus().state == client::WorldLoadingState::Playing
        && !m_world_map_open && m_has_focus && m_resumed;
    if (!active) {
        releaseActiveBreakAction();
        m_renderer->setBlockInteractionTarget(std::nullopt, 0.0F, now);
        while (consumeAcceptedBlockActionResult()) { }
        m_pending_block_actions.fill({});
        return;
    }
    updateBlockActionInput(m_break_pressed, m_break_was_pressed, m_place_pressed, m_place_was_pressed,
        false, false, false, false, now);
    m_break_was_pressed = m_break_pressed;
    m_place_was_pressed = m_place_pressed;
    while (auto const accepted = consumeAcceptedBlockActionResult()) {
        auto& pending = m_pending_block_actions[accepted->sequence % PENDING_BLOCK_ACTION_PRESENTATIONS];
        if (accepted->status != shared::BlockActionStatus::Accepted || accepted->sequence == 0U
            || pending.sequence != accepted->sequence || pending.join_epoch != accepted->session.join_epoch
            || accepted->session.join_epoch != worldLoadingJoinEpoch() || !pending.target) {
            continue;
        }
        if (pending.kind == shared::BlockActionKind::PlacePress) {
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Place, pending.material));
        } else if (pending.kind == shared::BlockActionKind::BreakHold && *pending.target == accepted->target
            && m_renderer->addAcceptedBlockBreak({accepted->session.join_epoch, accepted->server_tick,
                accepted->sequence, accepted->cell_version}, accepted->target, pending.material, now)) {
            static_cast<void>(playClientBlockSound(client::BlockSoundKind::Break, pending.material));
        }
        pending = {};
    }
    auto const target = resolveInteractionTarget();
    float progress = 0.0F;
    auto const* status = currentBlockActionStatus();
    if (target && status && m_block_break_input.active && m_block_break_input.target == target->cell
        && status->status == shared::BlockActionStatus::Progress && status->target == target->cell) {
        auto const& pending = m_pending_block_actions[status->sequence % PENDING_BLOCK_ACTION_PRESENTATIONS];
        if (pending.sequence == status->sequence && pending.join_epoch == status->session.join_epoch
            && pending.kind == shared::BlockActionKind::BreakHold && pending.target == status->target) {
            progress = client::detail::authoritativeBreakProgress(*status);
            if (progress > 0.0F && now >= m_next_chip_sound) {
                static_cast<void>(playClientBlockSound(client::BlockSoundKind::Chip, pending.material));
                m_next_chip_sound = now + std::chrono::milliseconds{120};
            }
        }
    }
    m_renderer->setBlockInteractionTarget(target, progress, now);
    if (!m_debug_hud_enabled) {
        return;
    }
    std::string mode = "Awaiting server permissions";
    if (status) {
        switch (status->mode) {
        case shared::GameMode::Spectator: mode = "Spectator"; break;
        case shared::GameMode::Creative: mode = "Creative"; break;
        case shared::GameMode::Survival: mode = "Survival"; break;
        }
        if (client::detail::hasCustomBlockActionGrants(*status)) {
            mode = "Custom (" + mode + ")";
        }
    }
    std::string const footer = mode + " | Block: " + std::string{client::uiBlockDisplayName(m_selected_block)}
        + " | Target: " + (target ? client::uiTargetBlockLabel(target->block) : "none");
    float const width = static_cast<float>(ANativeWindow_getWidth(m_app.window));
    float const height = static_cast<float>(ANativeWindow_getHeight(m_app.window));
    auto const controls = interactionControlLayout(static_cast<uint32_t>(width),
        static_cast<uint32_t>(height), m_density_scale);
    m_renderer->addGuiText(footer, {.position = {8.0F * m_density_scale, controls.footer_y, 0.0F},
        .scale = 0.4F * m_density_scale});
    m_renderer->addGuiText("+", {.position = {width * 0.5F - 8.0F, height * 0.5F - 16.0F, 0.0F}});
    static constexpr std::array<char const*, 4U> LABELS{"Break", "Place", "Block", "Mode"};
    for (size_t index = 0U; index < LABELS.size(); ++index) {
        auto const& bounds = controls.controls[index];
        if (bounds[2] == 0.0F) {
            continue;
        }
        m_renderer->addGuiText(LABELS[index], {
            .position = {bounds[0] + 8.0F * m_density_scale, bounds[1] + 12.0F * m_density_scale, 0.0F},
            .scale = controls.text_scale});
    }

}

void AndroidPlayerClient::setWorldMapOpen(bool const open)
{
    releaseActiveBreakAction();
    m_break_pressed = false;
    m_place_pressed = false;
    m_action_touch_pointers.fill(-1);
    m_action_ignored_pointers.reset();
    if (m_renderer) {
        m_renderer->clearBlockInteractionEffects();
    }
    m_world_map_open = open;
    invalidateWorldMapView();
    m_minimap_bounds = {};
    m_map_control_pointer = -1;
    m_map_control_owned = false;
    m_map_control_cancelled = false;
    m_map_close_pressed = false;
    m_map_pressed_action.reset();
    m_input.clear();
    m_ascend_input.clear();
    m_input_actions.reset();
    m_descend_pressed = false;
    m_map_pinch_distance = 0.0F;
}

void AndroidPlayerClient::applyMapAction(client::S2WorldMapAction const action)
{
    auto const player = predictedLocalPlayer();
    double const x = player ? shared::playerPositionX(*player) : m_world_map_state.center_x;
    double const y = player ? shared::playerPositionY(*player) : m_world_map_state.center_y;
    auto const old_layers = m_world_map_state.rasterLayers();
    if (client::applyS2WorldMapAction(m_world_map_state, action, x, y, false,
            static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window)),
            static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window)))) {
        if (old_layers != m_world_map_state.rasterLayers()) {
            m_world_map_image.reset();
        }
        invalidateWorldMapView();
    }
}

void AndroidPlayerClient::invalidateWorldMapView() noexcept
{
    // Keep the future until it is ready: destroying an unfinished std::async
    // future would wait on the native input thread.
    m_map_view_cancellation.request_stop();
    ++m_map_view_revision;
    m_map_view_ready = false;
    m_map_overlay = {};
}

bool AndroidPlayerClient::updateWorldMapView(
    std::shared_ptr<shared::TerrainWorld const> const& world,
    std::shared_ptr<shared::TerrainLodArtifact const> const& root,
    client::S2MapLayout const& layout,
    double const player_x,
    double const player_y,
    uint32_t const width,
    uint32_t const height
)
{
    if (m_map_view_width != width || m_map_view_height != height) {
        m_map_view_width = width;
        m_map_view_height = height;
        invalidateWorldMapView();
    }
    if (m_map_view_job.valid()
        && m_map_view_job.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        try {
            auto result = m_map_view_job.get();
            if (m_map_job_revision == m_map_view_revision && result.view
                && m_renderer->setMapImage(result.view->width, result.view->height, result.view->rgba)) {
                m_world_map_image = std::move(result.source);
                m_map_view_ready = true;
            }
        } catch (generation::GenerationCancelled const&) {
        } catch (std::exception const& error) {
            CORE_ERROR("Android map preparation failed: {}", error.what());
        }
    }
    if (!m_map_view_ready && !m_map_view_job.valid() && m_map_job_revision != m_map_view_revision) {
        auto source = m_world_map_image;
        bool const default_layers = m_world_map_state.rasterLayers() == client::S2WorldMapState{}.rasterLayers();
        if (default_layers && !source) {
            source = terrainLodMap();
            if (!source || source->source_identity != world->identity()
                || source->source_model_identity != root->model_identity) {
                return false;
            }
        }
        try {
            m_map_job_revision = m_map_view_revision;
            m_map_view_cancellation = std::stop_source{};
            m_map_view_job = std::async(std::launch::async, [
                world, root, source = std::move(source), state = m_world_map_state, layout, player_x, player_y,
                width, height, density = m_density_scale, cancellation = m_map_view_cancellation.get_token()
            ]() mutable {
                if (!source) {
                    auto raster = client::rasterizeS2WorldMap(*world, *root, state, cancellation);
                    if (!raster) {
                        return WorldMapViewResult{};
                    }
                    source = std::make_shared<client::S2MapImage const>(std::move(*raster));
                }
                if (cancellation.stop_requested()) {
                    return WorldMapViewResult{};
                }
                client::TextRenderer text;
                std::vector<client::GuiVectorVertex> triangles;
                auto const overlay = client::buildS2WorldMapOverlay(state, source.get(), world.get(),
                    player_x, player_y, width, height, density, text, triangles, 0.0F, true);
                auto view = client::composeS2WorldMapView(source.get(), layout, overlay, width, height, cancellation);
                return WorldMapViewResult{.source = std::move(source), .view = std::move(view)};
            });
        } catch (std::exception const& error) {
            CORE_ERROR("Could not start Android map preparation: {}", error.what());
        }
    }
    return m_map_view_ready;
}

void AndroidPlayerClient::updateMapPresentation()
{
    if (worldLoadingStatus().state != client::WorldLoadingState::Playing) {
        return;
    }
    auto const world = terrainLodWorld();
    auto const root = terrainLodRoot();
    auto const player = predictedLocalPlayer();
    if (!world || !root || !player) {
        invalidateWorldMapView();
        m_minimap_bounds = {};
        m_renderer->clearMapImage();
        static_cast<void>(m_renderer->setMapDraws({}));
        return;
    }
    double const x = shared::playerPositionX(*player);
    double const y = shared::playerPositionY(*player);
    if (m_map_identity != world->identity() || m_map_join_epoch != worldLoadingJoinEpoch()) {
        m_map_identity = world->identity();
        m_map_join_epoch = worldLoadingJoinEpoch();
        m_world_map_image.reset();
        m_minimap_lod.reset();
        m_world_map_state = {};
        m_world_map_state.center_x = x;
        m_world_map_state.center_y = y;
        invalidateWorldMapView();
    }
    uint32_t const width = static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window));
    uint32_t const height = static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window));
    auto const layout = client::layoutS2Map(m_world_map_state, width, height,
        m_world_map_open, !m_world_map_open && m_debug_hud_enabled, m_density_scale);
    std::vector<client::GuiVectorVertex> triangles;
    client::TextRenderer text;
    m_renderer->clearGuiText();
    if (m_world_map_open) {
        if (updateWorldMapView(world, root, layout, x, y, width, height)) {
            m_map_overlay = client::buildS2WorldMapOverlay(m_world_map_state,
                m_world_map_image.get(), world.get(), x, y, width, height,
                m_density_scale, text, triangles,
                std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count(), true);
            client::MapImageDraw const draw{.image = 0U,
                .destination = {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)}};
            static_cast<void>(m_renderer->setMapDraws(std::span{&draw, 1U}));
        } else {
            m_renderer->clearMapImage();
            static_cast<void>(m_renderer->setMapDraws({}));
            bool const failed = !m_map_view_job.valid() && m_map_job_revision == m_map_view_revision;
            m_renderer->addGuiText(failed ? "Map unavailable" : "Preparing map", {
                .position = {12.0F * m_density_scale, 12.0F * m_density_scale, 0.0F},
                .scale = m_density_scale});
        }
        m_renderer->addGuiText("Close map [M / Back]", {
            .position = {static_cast<float>(width) - 160.0F * m_density_scale,
                static_cast<float>(height) - 48.0F * m_density_scale, 0.0F},
            .scale = m_density_scale});
    } else if (layout.show_minimap) {
        // Cropping the immutable L0 does not regenerate or query fine terrain.
        // Refresh around the current position so the terrain and centered marker agree.
        auto preview = (!m_minimap_lod || std::abs(x - m_minimap_lod->origin_x
                - m_minimap_lod->extent / 2.0) > 256.0
                || std::abs(y - m_minimap_lod->origin_y - m_minimap_lod->extent / 2.0) > 256.0)
            ? client::buildS2MinimapPreview(*world, *root, x, y) : nullptr;
        if (preview) {
            m_minimap_lod = std::move(preview);
            m_minimap_image = client::rasterizeS2Minimap(*m_minimap_lod);
        }
        if (m_minimap_image) {
            static_cast<void>(m_renderer->setMapImage(
                m_minimap_image->width, m_minimap_image->height, m_minimap_image->rgba));
            auto draw = layout.minimap_draw;
            draw.image = 0U;
            double const half = m_minimap_lod->extent / 2.0;
            double const dx = std::remainder(x - m_minimap_lod->origin_x - half,
                static_cast<double>(generation::WORLD_SIZE_BLOCKS));
            double const dy = std::remainder(y - m_minimap_lod->origin_y - half,
                static_cast<double>(generation::WORLD_SIZE_BLOCKS));
            // Move the cheap UV crop every frame; only the larger source window is cached.
            draw.source = {static_cast<float>(0.1 + dx / m_minimap_lod->extent),
                static_cast<float>(0.1 + dy / m_minimap_lod->extent), 0.8F, 0.8F};
            static_cast<void>(m_renderer->setMapDraws(std::span{&draw, 1U}));
            m_minimap_bounds = layout.minimap_bounds;
            auto const facing = m_look_camera.forward();
            auto const marker = client::layoutS2MinimapMarker(*m_minimap_lod, x, y,
                client::minimapHeadingDegreesFromCameraForward(facing.x, facing.y), m_minimap_bounds);
            client::appendS2MinimapPointerTriangles(marker, m_minimap_bounds, m_density_scale, triangles);
            m_renderer->addGuiText("N       Map [M]", {
                .position = {m_minimap_bounds[0], m_minimap_bounds[1] - 20.0F * m_density_scale, 0.0F},
                .scale = m_density_scale});
            for (auto const& [label, u, v] : std::array<std::tuple<char const*, float, float>, 3U>{{
                     {"E", 1.0F, 0.5F}, {"S", 0.5F, 1.0F}, {"W", 0.0F, 0.5F}}}) {
                m_renderer->addGuiText(label, {
                    .position = {m_minimap_bounds[0] + u * m_minimap_bounds[2],
                        m_minimap_bounds[1] + v * m_minimap_bounds[3], 0.0F},
                    .scale = m_density_scale});
            }
        }
    } else {
        m_minimap_bounds = {};
        static_cast<void>(m_renderer->setMapDraws({}));
    }
    for (auto const& label : text.labels()) {
        m_renderer->addGuiText(label.value, label.placement, label.color);
    }
    m_renderer->setDebugHudEnabled(m_debug_hud_enabled && !m_world_map_open);
    static_cast<void>(m_renderer->setGuiVectorTriangles(triangles));
}

void AndroidPlayerClient::resetLoadingPresentation()
{
    m_generation_map_animator.reset();
    m_loading_map_identity.reset();
    m_loading_final_atlas.reset();
    m_loading_final_map.reset();
    m_loading_attempted_root.reset();
    m_loading_visible_elapsed = {};
    m_loading_last_update = {};
}

void AndroidPlayerClient::updateLoadingPresentation(client::WorldLoadingStatus const& status)
{
    if (status.state == client::WorldLoadingState::Playing) {
        updateClientCreationAudio(m_has_focus && m_resumed
                ? client::CreationAudioStage::Gameplay : client::CreationAudioStage::Silent,
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (m_loading_overlay_active) {
            m_renderer->clearMapImage();
            static_cast<void>(m_renderer->setMapDraws({}));
            m_renderer->clearGuiText();
            m_renderer->setDebugHudEnabled(m_debug_hud_enabled);
            resetLoadingPresentation();
            m_loading_overlay_active = false;
        }
        return;
    }
    auto const now = std::chrono::steady_clock::now();
    if (m_loading_map_identity != m_world_loading_identity) {
        resetLoadingPresentation();
        m_loading_map_identity = m_world_loading_identity;
    }
    if (m_loading_last_update != std::chrono::steady_clock::time_point{}) {
        m_loading_visible_elapsed += now - m_loading_last_update;
    }
    m_loading_last_update = now;
    m_loading_overlay_active = true;
    m_renderer->setDebugHudEnabled(false);
    auto const world = terrainLodWorld();
    auto const root = terrainLodRoot();
    bool const matching = world && root && m_terrain_session
        && m_terrain_session->join_epoch == worldLoadingJoinEpoch()
        && world->identity() == m_terrain_session->terrain
        && root->terrain_identity == world->identity()
        && world->atlas()->configuration == m_world_loading_identity.configuration;
    if (matching && m_loading_attempted_root != root) {
        m_loading_attempted_root = root;
        m_loading_final_atlas = world->atlas();
        auto map = terrainLodMap();
        if (map && map->source_identity == world->identity()
            && map->source_model_identity == root->model_identity) {
            m_loading_final_map = std::move(map);
        }
    }
    auto const animation = m_generation_map_animator.updateNative(
        status.map_snapshot, m_loading_final_atlas, m_loading_final_map, {
            .visible_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(m_loading_visible_elapsed),
            .reduced_motion = status.state == client::WorldLoadingState::Failed
                || status.state == client::WorldLoadingState::Cancelled,
            .ready = status.state == client::WorldLoadingState::ReadyForStart
                || status.state == client::WorldLoadingState::Starting,
        });
    auto const map_source = client::uiLoadingMapSource(status.state, m_world_loading_identity.configuration.model_tag);
    auto const runtime = m_renderer->runtimeInfo();
    auto const frame = client::buildUiFrame(worldLoadingJoinEpoch(), status, connectionAttemptStarted(),
        !m_terrain_session || (runtime.terrain_lod_active && runtime.terrain_lod_leaf_count != 0U), {},
        animation.image ? std::optional{animation} : std::nullopt, serverAddress(),
        m_world_loading_identity.configuration.seed, map_source);
    uint32_t const width = static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window));
    uint32_t const height = static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window));
    auto const layout = client::layoutUiLoading(frame, width, height, m_density_scale);
    m_renderer->clearGuiText();
    if (animation.image) {
        static_cast<void>(m_renderer->setMapImage(
            animation.image->width, animation.image->height, animation.image->rgba));
        client::MapImageDraw const draw{
            .image = 0U,
            .destination = {layout.map_viewport.x, layout.map_viewport.y,
                layout.map_viewport.width, layout.map_viewport.height},
        };
        static_cast<void>(m_renderer->setMapDraws(std::span{&draw, 1U}));
    } else {
        m_renderer->clearMapImage();
        static_cast<void>(m_renderer->setMapDraws({}));
    }
    client::CreationAudioStage audio_stage = client::CreationAudioStage::Silent;
    if (animation.image && layout.map_viewport.width > 0.0F && layout.map_viewport.height > 0.0F) {
        switch (animation.phase) {
        case client::GenerationMapAnimationPhase::Creation: audio_stage = client::CreationAudioStage::Molten; break;
        case client::GenerationMapAnimationPhase::Tectonics:
        case client::GenerationMapAnimationPhase::Relief: audio_stage = client::CreationAudioStage::Cooling; break;
        case client::GenerationMapAnimationPhase::Water: audio_stage = client::CreationAudioStage::Water; break;
        case client::GenerationMapAnimationPhase::Ice: audio_stage = client::CreationAudioStage::Ice; break;
        case client::GenerationMapAnimationPhase::Climate: audio_stage = client::CreationAudioStage::Living; break;
        case client::GenerationMapAnimationPhase::Arrival: audio_stage = client::CreationAudioStage::Ready; break;
        }
    }
    updateClientCreationAudio(audio_stage, std::chrono::duration<double>(now.time_since_epoch()).count());
    client::TextRenderer text;
    client::buildUiLoadingPresentation(frame, layout, text);
    for (auto const& label : text.labels()) {
        m_renderer->addGuiText(label.value, label.placement, label.color);
    }
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
        if (playing && key_code == AKEYCODE_M) {
            if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) == 0 && canRender()) {
                setWorldMapOpen(!m_world_map_open);
            }
            return 1;
        }
        if (playing && m_world_map_open) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                using Action = client::S2WorldMapAction;
                switch (key_code) {
                case AKEYCODE_BACK:
                case AKEYCODE_ESCAPE: setWorldMapOpen(false); break;
                case AKEYCODE_A:
                case AKEYCODE_DPAD_LEFT: applyMapAction(Action::PanLeft); break;
                case AKEYCODE_D:
                case AKEYCODE_DPAD_RIGHT: applyMapAction(Action::PanRight); break;
                case AKEYCODE_W:
                    applyMapAction((AKeyEvent_getMetaState(event) & AMETA_CTRL_ON) != 0
                        ? Action::ToggleWind : Action::PanUp);
                    break;
                case AKEYCODE_DPAD_UP: applyMapAction(Action::PanUp); break;
                case AKEYCODE_S:
                case AKEYCODE_DPAD_DOWN: applyMapAction(Action::PanDown); break;
                case AKEYCODE_PLUS:
                case AKEYCODE_EQUALS: applyMapAction(Action::ZoomIn); break;
                case AKEYCODE_MINUS: applyMapAction(Action::ZoomOut); break;
                case AKEYCODE_P: applyMapAction(Action::TogglePlates); break;
                case AKEYCODE_H: applyMapAction(Action::ToggleHeights); break;
                case AKEYCODE_T: applyMapAction(Action::ToggleTemperature); break;
                case AKEYCODE_B: applyMapAction(Action::ToggleBiomes); break;
                case AKEYCODE_C: applyMapAction(Action::ToggleCurrents); break;
                case AKEYCODE_O: applyMapAction(Action::ToggleMoisture); break;
                case AKEYCODE_Q: applyMapAction(Action::TogglePrecipitation); break;
                case AKEYCODE_0: applyMapAction(Action::Overview); break;
                case AKEYCODE_R: applyMapAction(Action::Reset); break;
                case AKEYCODE_L: applyMapAction(Action::ToggleLegend); break;
                default: break;
                }
            }
            return 1;
        }
        // A key still held across a surface or focus loss can send repeat
        // events after the input state was cleared. Wait for a fresh press.
        if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) != 0) {
            return 1;
        }
        if (playing && !m_world_map_open && (key_code == AKEYCODE_B || key_code == AKEYCODE_G)) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                updateBlockActionInput(m_break_pressed, m_break_pressed, m_place_pressed, m_place_pressed,
                    key_code == AKEYCODE_B, key_code == AKEYCODE_G, false, false,
                    std::chrono::steady_clock::now());
            }
            return 1;
        }
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

    if (playing && event_type == AINPUT_EVENT_TYPE_MOTION && !canRender()) {
        return 0;
    }
    if (playing && !m_world_map_open && event_type == AINPUT_EVENT_TYPE_MOTION) {
        uint32_t const source = static_cast<uint32_t>(AInputEvent_getSource(event));
        int32_t const action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        if ((source & static_cast<uint32_t>(AINPUT_SOURCE_MOUSE)) == AINPUT_SOURCE_MOUSE) {
            int32_t const buttons = AMotionEvent_getButtonState(event);
            m_break_pressed = (buttons & AMOTION_EVENT_BUTTON_PRIMARY) != 0;
            m_place_pressed = (buttons & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
        } else if ((source & static_cast<uint32_t>(AINPUT_SOURCE_TOUCHSCREEN)) != 0U) {
            size_t const index = static_cast<size_t>((AMotionEvent_getAction(event)
                & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            int32_t const pointer = AMotionEvent_getPointerId(event, index);
            if (action == AMOTION_EVENT_ACTION_CANCEL) {
                m_action_touch_pointers.fill(-1);
                m_action_ignored_pointers.reset();
                m_break_pressed = false;
                m_place_pressed = false;
                releaseActiveBreakAction();
            }
            if (pointer >= 0 && pointer < static_cast<int32_t>(m_action_ignored_pointers.size())
                && m_action_ignored_pointers.test(static_cast<size_t>(pointer))) {
                if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP) {
                    m_action_ignored_pointers.reset(static_cast<size_t>(pointer));
                }
                return 1;
            }
            for (size_t control = 0U; control < m_action_touch_pointers.size(); ++control) {
                if (m_action_touch_pointers[control] == pointer) {
                    if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP) {
                        m_action_touch_pointers[control] = -1;
                        if (control == 0U) { m_break_pressed = false; }
                        if (control == 1U) { m_place_pressed = false; }
                    }
                    return 1;
                }
            }
            if (m_debug_hud_enabled
                && (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)) {
                float const x = AMotionEvent_getX(event, index);
                float const y = AMotionEvent_getY(event, index);
                float const width = static_cast<float>(ANativeWindow_getWidth(m_app.window));
                float const height = static_cast<float>(ANativeWindow_getHeight(m_app.window));
                auto const layout = interactionControlLayout(static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height), m_density_scale);
                for (size_t control = 0U; control < m_action_touch_pointers.size(); ++control) {
                    auto const& bounds = layout.controls[control];
                    if (x < bounds[0] || x >= bounds[0] + bounds[2]
                        || y < bounds[1] || y >= bounds[1] + bounds[3]) {
                        continue;
                    }
                    if (m_action_touch_pointers[control] >= 0) {
                        if (pointer >= 0 && pointer < static_cast<int32_t>(m_action_ignored_pointers.size())) {
                            m_action_ignored_pointers.set(static_cast<size_t>(pointer));
                        }
                        return 1;
                    }
                    m_action_touch_pointers[control] = pointer;
                    if (control == 0U) { m_break_pressed = true; }
                    if (control == 1U) { m_place_pressed = true; }
                    if (control >= 2U) {
                        updateBlockActionInput(m_break_pressed, m_break_pressed, m_place_pressed,
                            m_place_pressed, control == 2U, control == 3U, false, false,
                            std::chrono::steady_clock::now());
                    }
                    return 1;
                }
            }
        }
    }
    if (playing && event_type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t const action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        float const x = AMotionEvent_getX(event, 0U);
        float const y = AMotionEvent_getY(event, 0U);
        if (!m_world_map_open && action == AMOTION_EVENT_ACTION_DOWN
            && x >= m_minimap_bounds[0] && y >= m_minimap_bounds[1]
            && x < m_minimap_bounds[0] + m_minimap_bounds[2]
            && y < m_minimap_bounds[1] + m_minimap_bounds[3]) {
            setWorldMapOpen(true);
            m_map_control_owned = true;
            m_map_control_cancelled = true;
            m_map_control_pointer = AMotionEvent_getPointerId(event, 0U);
            return 1;
        }
        auto const contains = [](std::array<float, 4U> const& bounds, float px, float py) {
            return px >= bounds[0] && py >= bounds[1]
                && px < bounds[0] + bounds[2] && py < bounds[1] + bounds[3];
        };
        std::array<float, 4U> const close_bounds{
            static_cast<float>(ANativeWindow_getWidth(m_app.window)) - 160.0F * m_density_scale,
            static_cast<float>(ANativeWindow_getHeight(m_app.window)) - 48.0F * m_density_scale,
            160.0F * m_density_scale, 48.0F * m_density_scale};
        if (m_map_control_owned) {
            if (action == AMOTION_EVENT_ACTION_POINTER_DOWN
                || AMotionEvent_getPointerCount(event) > 1U) {
                m_map_control_cancelled = true;
            }
            if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
                bool const activate = action == AMOTION_EVENT_ACTION_UP && !m_map_control_cancelled
                    && AMotionEvent_getPointerId(event, 0U) == m_map_control_pointer;
                if (activate && m_world_map_open) {
                    if (m_map_close_pressed && contains(close_bounds, x, y)) {
                        setWorldMapOpen(false);
                    } else if (m_map_pressed_action && contains(m_map_pressed_action->bounds, x, y)) {
                        applyMapAction(m_map_pressed_action->action);
                    }
                }
                m_map_control_owned = false;
                m_map_pressed_action.reset();
                m_map_close_pressed = false;
            }
            return 1;
        }
        if (m_world_map_open) {
            if (action == AMOTION_EVENT_ACTION_DOWN) {
                m_map_close_pressed = contains(close_bounds, x, y);
                m_map_pressed_action.reset();
                if (!m_map_close_pressed) {
                    for (uint32_t index = 0U; index < m_map_overlay.action_region_count; ++index) {
                        auto const& region = m_map_overlay.action_regions[index];
                        if (contains(region.bounds, x, y)) {
                            m_map_pressed_action = region;
                            break;
                        }
                    }
                }
                if (m_map_close_pressed || m_map_pressed_action) {
                    m_map_control_owned = true;
                    m_map_control_cancelled = false;
                    m_map_control_pointer = AMotionEvent_getPointerId(event, 0U);
                    return 1;
                }
            }
            if (action == AMOTION_EVENT_ACTION_SCROLL) {
                applyMapAction(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0U) > 0.0F
                    ? client::S2WorldMapAction::ZoomIn : client::S2WorldMapAction::ZoomOut);
            } else if (AMotionEvent_getPointerCount(event) >= 2U) {
                float const distance = std::hypot(x - AMotionEvent_getX(event, 1U),
                    y - AMotionEvent_getY(event, 1U));
                if (m_map_pinch_distance > 0.0F && std::abs(distance - m_map_pinch_distance) > 16.0F) {
                    applyMapAction(distance > m_map_pinch_distance
                        ? client::S2WorldMapAction::ZoomIn : client::S2WorldMapAction::ZoomOut);
                }
                m_map_pinch_distance = distance;
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
                if (std::abs(x - m_map_touch_x) > 8.0F) {
                    applyMapAction(x > m_map_touch_x
                        ? client::S2WorldMapAction::PanLeft : client::S2WorldMapAction::PanRight);
                }
                if (std::abs(y - m_map_touch_y) > 8.0F) {
                    applyMapAction(y > m_map_touch_y
                        ? client::S2WorldMapAction::PanUp : client::S2WorldMapAction::PanDown);
                }
            }
            m_map_touch_x = x;
            m_map_touch_y = y;
            return 1;
        }
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
        __android_log_print(ANDROID_LOG_INFO, "MinecraftCloneLifecycle", "Android native window initialized");
        createWindowResources();
        break;
    case APP_CMD_TERM_WINDOW:
        __android_log_print(ANDROID_LOG_INFO, "MinecraftCloneLifecycle", "Android native window terminated");
        destroyWindowResources();
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        if (m_app.window != nullptr) {
            uint32_t const width = static_cast<uint32_t>(ANativeWindow_getWidth(m_app.window));
            uint32_t const height = static_cast<uint32_t>(ANativeWindow_getHeight(m_app.window));
            m_input.setSurfaceWidth(width);
            m_input.setSurfaceHeight(height);
            invalidateWorldMapView();
            m_minimap_bounds = {};
            if (m_renderer != nullptr) {
                m_renderer->recreate(width, height);
            }
        }
        break;
    case APP_CMD_RESUME:
        __android_log_print(ANDROID_LOG_INFO, "MinecraftCloneLifecycle", "Android activity resumed");
        m_resumed = true;
        break;
    case APP_CMD_PAUSE:
    case APP_CMD_STOP:
        m_resumed = false;
        [[fallthrough]];
    case APP_CMD_LOST_FOCUS:
        __android_log_print(ANDROID_LOG_INFO, "MinecraftCloneLifecycle",
            "Android rendering paused by app command %d", command);
        releaseActiveBreakAction();
        m_break_pressed = false;
        m_place_pressed = false;
        m_action_touch_pointers.fill(-1);
        m_action_ignored_pointers.reset();
        m_has_focus = false;
        updateClientCreationAudio(client::CreationAudioStage::Silent,
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        setWorldMapOpen(false);
        m_loading_last_update = {};
        m_input.clear();
        m_loading_transition_keys.reset();
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
    m_block_break_input = {};
    setWorldMapOpen(false);
    m_map_identity.reset();
    m_world_map_image.reset();
    if (m_renderer) {
        m_renderer->clearBlockInteractionEffects();
    }
    clearPendingBlockActions();
    m_break_pressed = false;
    m_place_pressed = false;
    m_action_touch_pointers.fill(-1);
    m_action_ignored_pointers.reset();
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
    m_loading_last_update = {};
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
    setWorldMapOpen(false);
    updateClientCreationAudio(client::CreationAudioStage::Silent,
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    m_loading_last_update = {};
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
    m_loading_transition_keys.reset();
    m_loading_touch_pointer_mask = 0U;
    m_ascend_input.clear();
    m_input_actions.reset();
    m_descend_pressed = false;
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
    updateClientCreationAudio(client::CreationAudioStage::Silent,
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
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
