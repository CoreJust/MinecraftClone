#include <client/LoadingBackdrop.hpp>
#include <client/PlayerClient.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/UiPresentation.hpp>
#include <client/WorldLoading.hpp>
#include <client/detail/ScreenshotPng.hpp>
#include <server/GameServer.hpp>
#include <server/WorldPreparation.hpp>

#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <generation/Generation.hpp>
#include <core/net/Address.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace client {

struct PlayerClientFocusTestAccess final {
    static bool publishGeneration(PlayerClient& player_client, generation::GenerationProgress const& progress)
    {
        return player_client.m_world_loading.publishGenerationProgress(
            player_client.m_world_loading_identity,
            progress,
            std::chrono::steady_clock::now()
        );
    }

    static std::chrono::milliseconds generationVisibleElapsed(PlayerClient const& player_client)
    {
        return player_client.m_generation_map_visible_elapsed;
    }

    static void restoreOrdinaryStartupAfterCaptureOptIn(PlayerClient& player_client)
    {
        player_client.m_benchmark.reset();
        player_client.m_debug_hud_enabled = true;
        player_client.m_renderer.setDebugHudEnabled(true);
    }

    static FrameCaptureState captureState(PlayerClient const& player_client)
    {
        return player_client.m_renderer.captureState();
    }

    static bool lastPresentationSucceeded(PlayerClient const& player_client)
    {
        return player_client.m_last_presentation_succeeded;
    }

    static void setHudTextStyle(PlayerClient& player_client, TextStyle const style) noexcept
    {
        player_client.m_hud_text_style = style;
    }

    static TextStyle hudTextStyle(PlayerClient const& player_client) noexcept
    {
        return player_client.m_hud_text_style;
    }

    static void requestFrameCapture(PlayerClient& player_client)
    {
        player_client.m_renderer.requestFrameCapture();
    }

    static std::optional<RendererFrameCapture> takeFrameCapture(PlayerClient& player_client)
    {
        return player_client.m_renderer.takeFrameCapture();
    }

    static void stop(PlayerClient& player_client) noexcept
    {
        player_client.m_running = false;
    }

    static GLFWwindow* nativeWindow(PlayerClient const& player_client) noexcept
    {
        return player_client.m_window.nativeHandle();
    }

    static void seedCursorSample(PlayerClient& player_client) noexcept
    {
        player_client.m_has_cursor_position = true;
    }

    static bool hasCursorSample(PlayerClient const& player_client) noexcept
    {
        return player_client.m_has_cursor_position;
    }

    static void servicePlatformEvents(PlayerClient& player_client)
    {
        player_client.servicePlatformEvents();
    }

    static void renderFrame(PlayerClient& player_client)
    {
        player_client.render();
    }

    static bool beginPlayingWorld(PlayerClient& player_client, bool const start = true)
    {
        using Clock = std::chrono::steady_clock;
        if (!player_client.beginLoadingRequest(Clock::now())) {
            return false;
        }

        WorldLoadingIdentity const identity = player_client.m_world_loading_identity;
        shared::TerrainIdentity const terrain_identity{
            .seed = identity.configuration.seed,
            .configuration_identity = generation::configurationIdentity(identity.configuration),
        };
        if (!player_client.m_world_loading.publishAtlasCompletion(identity, terrain_identity, Clock::now())) {
            return false;
        }

        static constexpr shared::PlayerPosition SAFE_SPAWN{ .x = 512, .y = 512, .z = 512 };
        if (!player_client.m_world_loading.publishRefinedSpawn(
                identity,
                SAFE_SPAWN,
                SpawnValidation::Safe,
                Clock::now()
            )) {
            return false;
        }
        for (shared::ChunkCoordinate const key : WorldLoading::requiredChunkKeys(
                 SAFE_SPAWN,
                 WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
             )) {
            shared::Chunk const chunk{ key };
            if (!player_client.m_world_loading.publishMaterializedChunk(identity, chunk, Clock::now())) {
                return false;
            }
        }

        if (!start) {
            player_client.m_terrain_session = shared::TerrainSessionIdentity{
                .join_epoch = identity.join_epoch,
                .terrain = terrain_identity,
            };
            return true;
        }

        if (!player_client.m_world_loading.markReadyPromptFrameRendered(identity, Clock::now())
            || player_client.m_world_loading.acceptStartInput(identity, Clock::now()) != LoadingInputResult::Started) {
            return false;
        }
        return player_client.m_world_loading.acknowledgeStart(identity, true, {}, Clock::now());
    }

    static bool uploadPromptLod(PlayerClient& player_client, uint32_t const leaves)
    {
        auto scene = std::make_shared<TerrainLodScene>();
        scene->identity = *player_client.activeTerrainIdentity();
        scene->faces.push_back({ .x = 512, .y = 512, .z = 512 });
        scene->groups.push_back({ .range = { .first_instance = 0U, .instance_count = 1U }, .extent = 16U });
        scene->leaves.resize(leaves);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 5 };
        // Planning is asynchronous; retry the same candidate until it publishes.
        while (std::chrono::steady_clock::now() < deadline) {
            if (player_client.m_renderer.upsertTerrainLodScene(scene, deadline)) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    static void resetLoadingConnection(PlayerClient& player_client)
    {
        player_client.resetConnectionState();
    }

    static void recreatePromptSwapchain(PlayerClient& player_client)
    {
        RendererRuntimeInfo const runtime = player_client.m_renderer.runtimeInfo();
        player_client.m_renderer.recreate(runtime.width + 1U, runtime.height + 1U);
    }

    static std::optional<RendererFrameIdentity> promptFrame(PlayerClient const& player_client)
    {
        return player_client.m_loading_prompt_frame;
    }

    static void seedScreenshotScene(PlayerClient& player_client)
    {
        std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
        player_client.m_local_character = '@';
        player_client.m_world.spawnPlayer(1U, '@', shared::PlayerPosition{ .x = 16, .y = 16, .z = 0 });
        player_client.m_world.spawnPlayer(
            2U,
            '#',
            shared::PlayerPosition{ .x = 18, .y = 16, .z = 0 },
            5U
        );
        for (shared::Player const& player : player_client.m_world.players()) {
            player_client.m_player_presentation.update(player, now);
        }
    }

    static bool beginInitialLoading(PlayerClient& player_client)
    {
        return player_client.beginLoadingRequest(std::chrono::steady_clock::now());
    }

    static bool renderLoadingStatusFrame(
        PlayerClient& player_client,
        WorldLoadingStatus const& status,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        player_client.updateLoadingPresentation(status);
        if (!player_client.m_window.nextFrame()) {
            return false;
        }
        return player_client.m_renderer.render({}, deadline);
    }

    static RendererFrameProgress frameProgress(PlayerClient const& player_client) noexcept
    {
        return player_client.m_renderer.frameProgress();
    }

    static double lookYaw(PlayerClient const& player_client) noexcept
    {
        return player_client.m_look_camera.pose().angles.yaw_degrees;
    }

    static bool updateLoadingInputSnapshot(
        PlayerClient& player_client,
        std::span<int const> pressed_keys,
        std::span<int const> pressed_mouse_buttons
    )
    {
        return player_client.updateLoadingInputSnapshot(pressed_keys, pressed_mouse_buttons);
    }

    static bool updatePlatformInputSnapshot(PlayerClient& player_client, std::span<int const> pressed_keys)
    {
        return player_client.updatePlatformInputSnapshot(pressed_keys, {}, 0.0, 0.0);
    }

    static bool injectInputSnapshot(
        PlayerClient& player_client,
        std::span<int const> pressed_keys,
        std::span<int const> pressed_mouse_buttons
    )
    {
        return player_client.updatePlatformInputSnapshot(pressed_keys, pressed_mouse_buttons, 64.0, 64.0);
    }

    static std::optional<shared::BlockTarget> interactionTarget(PlayerClient const& player_client)
    {
        return player_client.resolveInteractionTarget();
    }

    static std::optional<shared::ServerBlockActionResultMessage> blockActionStatus(PlayerClient const& player_client)
    {
        return player_client.blockActionStatus();
    }

    static shared::Block selectedBlock(PlayerClient const& player_client) noexcept
    {
        return player_client.m_selected_block;
    }

    static uint32_t nextBlockActionSequence(PlayerClient const& player_client) noexcept
    {
        return player_client.m_client_block_action_sequence;
    }

    static bool breakInputActive(PlayerClient const& player_client) noexcept
    {
        return player_client.m_block_break_input.active;
    }

    static bool seedBlockEffects(PlayerClient& player_client)
    {
        shared::BlockTarget const target{.cell = {512, 514, 513}, .block = shared::Block::Stone};
        player_client.m_renderer.clearBlockInteractionEffects();
        player_client.m_interaction_target = target;
        player_client.m_block_break_input.active = true;
        player_client.m_block_break_input.target = target.cell;
        player_client.rememberBlockAction(7U, shared::BlockActionKind::BreakHold, target.cell, target.block);
        player_client.m_renderer.setBlockInteractionTarget(target, 0.5F);
        return player_client.m_renderer.addAcceptedBlockBreak({1U, 1U, 1U, 1U}, target.cell,
            target.block, std::chrono::steady_clock::now());
    }

    static bool hasStoredBlockTarget(PlayerClient const& player_client)
    {
        return player_client.m_interaction_target.has_value();
    }

    static bool hasPendingBreakReceipt(PlayerClient const& player_client)
    {
        return std::any_of(player_client.m_pending_block_actions.begin(),
            player_client.m_pending_block_actions.end(), [](auto const& pending) {
                return pending.sequence != 0U && pending.kind == shared::BlockActionKind::BreakHold;
            });
    }

    static bool acceptSeededParticleReceipt(PlayerClient& player_client)
    {
        return player_client.m_renderer.addAcceptedBlockBreak({1U, 1U, 1U, 1U}, {512, 514, 513},
            shared::Block::Stone, std::chrono::steady_clock::now());
    }

    static void updateBlockEffects(PlayerClient& player_client)
    {
        player_client.updateBlockInteractionPresentation(std::chrono::steady_clock::now());
    }

    static void setWorldMapOpen(PlayerClient& player_client, bool const open)
    {
        player_client.setWorldMapOpen(open, 512.0, 512.0);
    }

    static bool worldMapOpen(PlayerClient const& player_client) noexcept
    {
        return player_client.m_s2_world_map_open;
    }

    static bool canonicalMinimapReady(PlayerClient const& player_client)
    {
        auto const world = player_client.terrainLodWorld();
        return world && player_client.m_s2_minimap_image && player_client.m_s2_minimap_lod
            && player_client.m_s2_map_join_epoch == player_client.worldLoadingJoinEpoch()
            && player_client.m_s2_minimap_lod->terrain_identity == world->identity();
    }

    static bool worldMapReady(PlayerClient const& player_client)
    {
        return player_client.m_s2_world_map_open && player_client.m_s2_world_map_image
            && !player_client.m_s2_world_map_dirty;
    }

    static void openWorldMap(PlayerClient& player_client)
    {
        auto const player = player_client.predictedLocalPlayer();
        player_client.setWorldMapOpen(
            true,
            player ? shared::playerPositionX(*player) : 0.0,
            player ? shared::playerPositionY(*player) : 0.0
        );
    }

    static void closeWorldMap(PlayerClient& player_client)
    {
        auto const player = player_client.predictedLocalPlayer();
        player_client.setWorldMapOpen(
            false,
            player ? shared::playerPositionX(*player) : 0.0,
            player ? shared::playerPositionY(*player) : 0.0
        );
    }

    static bool worldMapLayerEnabled(PlayerClient const& player_client, S2WorldMapLayer layer) noexcept
    {
        return player_client.m_s2_world_map_state.layers[static_cast<size_t>(layer)];
    }

    static shared::Direction gameplayInput(PlayerClient& player_client)
    {
        return player_client.input();
    }

    static bool hasStartTransitionInputs(PlayerClient const& player_client) noexcept
    {
        return player_client.hasStartTransitionInputs();
    }

    static bool updateScreenshotCaptureKey(PlayerClient& player_client, bool const pressed) noexcept
    {
        return player_client.m_screenshot_capture_key_latch.update(pressed);
    }

    static void captureStartTransitionInputs(PlayerClient& player_client)
    {
        player_client.captureStartTransitionInputs();
    }

    static std::optional<RendererFrameCapture> captureLoadingPresentation(
        PlayerClient& player_client,
        WorldLoadingStatus const& status
    )
    {
        static constexpr uint32_t MAX_CAPTURE_FRAMES = 24U;
        static constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds{ 10 };

        player_client.updateLoadingPresentation(status);
        player_client.m_renderer.requestFrameCapture();
        auto const deadline = std::chrono::steady_clock::now() + CAPTURE_TIMEOUT;
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            if (!player_client.m_window.nextFrame()) {
                return std::nullopt;
            }
            static_cast<void>(player_client.m_renderer.render({}, deadline));
            if (std::optional<RendererFrameCapture> const capture = player_client.m_renderer.takeFrameCapture();
                capture.has_value()) {
                return capture;
            }
        }
        return std::nullopt;
    }

    static std::optional<RendererFrameCapture> capturePlayerFrame(PlayerClient& player_client)
    {
        static constexpr uint32_t MAX_CAPTURE_FRAMES = 24U;
        static constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds{ 10 };

        player_client.m_renderer.requestFrameCapture();
        auto const deadline = std::chrono::steady_clock::now() + CAPTURE_TIMEOUT;
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            player_client.render();
            if (std::optional<RendererFrameCapture> const capture = player_client.m_renderer.takeFrameCapture();
                capture.has_value()) {
                return capture;
            }
        }
        return std::nullopt;
    }

    static void setScreenshotFeedback(PlayerClient& player_client, std::string message, bool const successful)
    {
        player_client.setScreenshotFeedback(std::move(message), successful);
    }

    static void queueScreenshotCapture(PlayerClient& player_client) noexcept
    {
        player_client.m_screenshot_capture_pending = true;
    }

    static void setScreenshotPasteboardName(PlayerClient& player_client, std::string name)
    {
        player_client.m_screenshot_test_pasteboard_name = std::move(name);
    }

    static bool captureScreenshotToClipboard(PlayerClient& player_client, std::string& error)
    {
        static constexpr uint32_t MAX_CAPTURE_FRAMES = 24U;
        static constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds{ 10 };

        player_client.setScreenshotFeedback({}, false);
        queueScreenshotCapture(player_client);
        std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + CAPTURE_TIMEOUT;
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            player_client.render();
            if (player_client.m_screenshot_feedback_successful) {
                error.clear();
                return true;
            }
            if (!player_client.m_screenshot_capture_pending && !player_client.m_screenshot_capture_requested) {
                break;
            }
        }
        error = player_client.m_screenshot_feedback.empty()
            ? "screenshot clipboard capture did not complete"
            : player_client.m_screenshot_feedback;
        return false;
    }

    static bool setLookAngles(PlayerClient& player_client, CameraAngles const angles) noexcept
    {
        return player_client.m_look_camera.setAngles(angles);
    }

    static CameraAngles renderedCameraAngles(PlayerClient const& player_client) noexcept
    {
        return player_client.m_camera.pose().angles;
    }

    static std::string const& screenshotFeedback(PlayerClient const& player_client) noexcept
    {
        return player_client.m_screenshot_feedback;
    }

    static bool screenshotFeedbackSuccessful(PlayerClient const& player_client) noexcept
    {
        return player_client.m_screenshot_feedback_successful;
    }

    static void disableDebugHud(PlayerClient& player_client) noexcept
    {
        player_client.m_renderer.setDebugHudEnabled(false);
    }

    static uint32_t validationErrorCount(PlayerClient const& player_client) noexcept
    {
        return player_client.m_renderer.validationErrorCount();
    }

    static RendererRuntimeInfo runtimeInfo(PlayerClient const& player_client)
    {
        return player_client.m_renderer.runtimeInfo();
    }
};

} // namespace client

namespace {

uint64_t pixelHash(std::span<uint8_t const> bytes)
{
    uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (uint8_t const byte : bytes) {
        hash = (hash ^ byte) * 1'099'511'628'211ULL;
    }
    return hash;
}

uint64_t snapshotContentHash(generation::GenerationSnapshot const& snapshot)
{
    uint64_t hash = static_cast<uint64_t>(snapshot.stage) ^ snapshot.resolution;
    auto append = [&hash](auto const& field) {
        hash = (hash ^ field.size()) * 1'099'511'628'211ULL;
        for (std::byte const byte : std::as_bytes(std::span{field})) {
            hash = (hash ^ std::to_integer<uint8_t>(byte)) * 1'099'511'628'211ULL;
        }
    };
    append(snapshot.elevation);
    append(snapshot.temperature);
    append(snapshot.precipitation);
    append(snapshot.water_level);
    append(snapshot.ocean_mask);
    append(snapshot.lake_mask);
    append(snapshot.river_mask);
    return hash;
}

float loadingContentScale(client::PlayerClient const& player_client)
{
    float x = 1.0F;
    float y = 1.0F;
    glfwGetWindowContentScale(client::PlayerClientFocusTestAccess::nativeWindow(player_client), &x, &y);
    return std::max(x, y);
}

client::UiRect mapCaptureBounds(client::RendererFrameCapture const& frame, float const dpi_scale)
{
    client::UiLoadingLayout const layout = client::layoutUiLoading(
        client::UiFrame{.screen = client::UiScreen::Generating}, frame.width, frame.height, dpi_scale);
    auto const& viewport = layout.map_viewport;
    return {.x = viewport.x + viewport.width * 0.1F, .y = viewport.y + viewport.height * 0.1F,
        .width = viewport.width * 0.8F, .height = viewport.height * 0.8F};
}

client::RendererFrameCapture cropMapInterior(client::RendererFrameCapture const& frame, float const dpi_scale)
{
    uint32_t constexpr OUTPUT_SIDE = 256U;
    auto const bounds = mapCaptureBounds(frame, dpi_scale);
    uint32_t const left = static_cast<uint32_t>(std::max(0.0F, bounds.x));
    uint32_t const top = static_cast<uint32_t>(std::max(0.0F, bounds.y));
    uint32_t const crop_width = static_cast<uint32_t>(bounds.width);
    uint32_t const crop_height = static_cast<uint32_t>(bounds.height);
    client::RendererFrameCapture crop{
        .width = OUTPUT_SIDE, .height = OUTPUT_SIDE, .srgb_encoded = frame.srgb_encoded,
        .rgba8 = std::vector<uint8_t>(OUTPUT_SIDE * OUTPUT_SIDE * 4U),
    };
    if (crop_width == 0U || crop_height == 0U || left + crop_width > frame.width
        || top + crop_height > frame.height
        || frame.rgba8.size() != static_cast<uint64_t>(frame.width) * frame.height * 4U) {
        return {};
    }
    for (uint32_t y = 0U; y < OUTPUT_SIDE; ++y) {
        for (uint32_t x = 0U; x < OUTPUT_SIDE; ++x) {
            uint32_t const source_x = left + x * crop_width / OUTPUT_SIDE;
            uint32_t const source_y = top + y * crop_height / OUTPUT_SIDE;
            size_t const source = (static_cast<size_t>(source_y) * frame.width + source_x) * 4U;
            size_t const destination = (static_cast<size_t>(y) * OUTPUT_SIDE + x) * 4U;
            std::copy_n(frame.rgba8.begin() + static_cast<ptrdiff_t>(source), 4U,
                crop.rgba8.begin() + static_cast<ptrdiff_t>(destination));
        }
    }
    return crop;
}

bool loadingCanvasVisible(client::RendererFrameCapture const& frame)
{
    if (frame.width == 0U || frame.height == 0U || frame.rgba8.empty()) {
        return false;
    }
    // Outside the right map viewport: the accepted loading canvas is flat and dark,
    // unlike the ordinary sky/terrain backdrop. Keep clear of footer labels.
    size_t const top = (static_cast<size_t>(frame.height / 5U) * frame.width + frame.width * 97U / 100U) * 4U;
    size_t const bottom = (static_cast<size_t>(frame.height * 4U / 5U) * frame.width
        + frame.width * 97U / 100U) * 4U;
    for (size_t channel = 0U; channel < 3U; ++channel) {
        if (frame.rgba8[top + channel] > 120U
            || std::abs(static_cast<int>(frame.rgba8[top + channel]) - frame.rgba8[bottom + channel]) > 2) {
            return false;
        }
    }
    return true;
}

struct MapSpatialVariation final {
    size_t unique_rgb_colors = 0U;
    uint32_t differing_horizontal_neighbors = 0U;

    bool geographic() const noexcept
    {
        return unique_rgb_colors >= 8U && differing_horizontal_neighbors >= 128U;
    }
};

MapSpatialVariation mapSpatialVariation(client::RendererFrameCapture const& crop)
{
    std::set<uint32_t> colors;
    MapSpatialVariation result;
    for (uint32_t y = 0U; y < crop.height; ++y) {
        uint32_t previous = 0U;
        for (uint32_t x = 0U; x < crop.width; ++x) {
            size_t const index = (static_cast<size_t>(y) * crop.width + x) * 4U;
            uint32_t const rgb = (static_cast<uint32_t>(crop.rgba8[index]) << 16U)
                | (static_cast<uint32_t>(crop.rgba8[index + 1U]) << 8U) | crop.rgba8[index + 2U];
            colors.insert(rgb);
            result.differing_horizontal_neighbors += x != 0U && previous != rgb ? 1U : 0U;
            previous = rgb;
        }
    }
    result.unique_rgb_colors = colors.size();
    return result;
}

std::optional<client::PlayerClientBenchmarkOptions> startupFrameCaptureOptions()
{
#if defined(_WIN32)
    return client::PlayerClientBenchmarkOptions{.enable_diagnostic_frame_capture = true};
#else
    return std::nullopt;
#endif
}

std::filesystem::path startupEvidenceDirectory()
{
    char const* configured = std::getenv("MC_STARTUP_CAPTURE_DIR");
    std::filesystem::path result;
    if (configured != nullptr) {
        result = configured;
    } else {
#if defined(_WIN32)
        result = std::filesystem::current_path() / "minecraft-startup-reproduction";
#else
        result = std::filesystem::temp_directory_path() / "minecraft-startup-reproduction";
#endif
    }
    std::filesystem::create_directories(result);
    return result;
}

bool saveFullFrame(std::filesystem::path const& filename, client::RendererFrameCapture const& frame)
{
    std::string error;
    auto const png = client::detail::encodeRgba8Png(frame.width, frame.height, frame.rgba8, error);
    if (!png) {
        ADD_FAILURE() << error;
        return false;
    }
    std::ofstream image{filename, std::ios::binary};
    image.write(reinterpret_cast<char const*>(png->data()), static_cast<std::streamsize>(png->size()));
    return image.good();
}

bool saveMapCapture(std::filesystem::path const& prefix, client::RendererFrameCapture const& crop)
{
    std::string error;
    auto const png = client::detail::encodeRgba8Png(crop.width, crop.height, crop.rgba8, error);
    if (!png) {
        ADD_FAILURE() << error;
        return false;
    }
    std::ofstream image{prefix.string() + ".png", std::ios::binary};
    image.write(reinterpret_cast<char const*>(png->data()), static_cast<std::streamsize>(png->size()));
    std::ofstream rgba{prefix.string() + ".rgba", std::ios::binary};
    rgba.write(reinterpret_cast<char const*>(crop.rgba8.data()), static_cast<std::streamsize>(crop.rgba8.size()));
    return image.good() && rgba.good();
}

std::shared_ptr<generation::GenerationSnapshot const> controlledGenerationSnapshot()
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->resolution = 256U;
    snapshot->elevation.resize(256U * 256U);
    for (uint32_t y = 0U; y < 256U; ++y) {
        for (uint32_t x = 0U; x < 256U; ++x) {
            snapshot->elevation[y * 256U + x] = x < 128U
                ? -1'800.0F + static_cast<float>(x * 7U + y * 2U)
                : 300.0F + static_cast<float>((x - 128U) * 17U + y * 3U);
        }
    }
    return snapshot;
}

bool waitForFocusState(client::PlayerClient& player_client, bool const focused)
{
    static constexpr std::chrono::milliseconds FOCUS_TIMEOUT{ 2'000 };
    static constexpr std::chrono::milliseconds POLL_INTERVAL{ 10 };
    std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + FOCUS_TIMEOUT;
    GLFWwindow* const window = client::PlayerClientFocusTestAccess::nativeWindow(player_client);
    do {
        client::PlayerClientFocusTestAccess::servicePlatformEvents(player_client);
        if ((glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE) == focused) {
            return true;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

#if defined(__APPLE__)
struct DecodedTestPng final {
    uint32_t width = 0U;
    uint32_t height = 0U;
    std::vector<uint8_t> rgba8;
};

std::optional<DecodedTestPng> decodeStoredTestPng(std::span<uint8_t const> png, std::string& error)
{
    static constexpr std::array<uint8_t, 8U> PNG_SIGNATURE{ 137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U };
    if (png.size() < PNG_SIGNATURE.size()
        || !std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), png.begin())) {
        error = "PNG signature is invalid";
        return std::nullopt;
    }

    auto const readBigEndian32 = [png](uint64_t const offset) {
        return (static_cast<uint32_t>(png[offset]) << 24U)
            | (static_cast<uint32_t>(png[offset + 1U]) << 16U)
            | (static_cast<uint32_t>(png[offset + 2U]) << 8U)
            | static_cast<uint32_t>(png[offset + 3U]);
    };
    DecodedTestPng decoded;
    std::vector<uint8_t> compressed;
    bool found_header = false;
    for (uint64_t offset = PNG_SIGNATURE.size(); offset < png.size();) {
        if (png.size() - offset < 12U) {
            error = "PNG chunk is truncated";
            return std::nullopt;
        }
        uint32_t const chunk_size = readBigEndian32(offset);
        if (chunk_size > png.size() - offset - 12U) {
            error = "PNG chunk extends beyond the image";
            return std::nullopt;
        }
        std::span<uint8_t const> const type_bytes = png.subspan(offset + 4U, 4U);
        std::string const type(type_bytes.begin(), type_bytes.end());
        std::span<uint8_t const> const data = png.subspan(offset + 8U, chunk_size);
        if (type == "IHDR") {
            if (found_header || data.size() != 13U || data[8U] != 8U || data[9U] != 6U
                || data[10U] != 0U || data[11U] != 0U || data[12U] != 0U) {
                error = "PNG header is unsupported";
                return std::nullopt;
            }
            decoded.width = readBigEndian32(offset + 8U);
            decoded.height = readBigEndian32(offset + 12U);
            found_header = true;
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), data.begin(), data.end());
        }
        offset += 12U + chunk_size;
    }
    if (!found_header || decoded.width == 0U || decoded.height == 0U || compressed.size() < 6U
        || compressed[0U] != 0x78U || compressed[1U] != 0x01U) {
        error = "PNG image data is missing or unsupported";
        return std::nullopt;
    }

    std::vector<uint8_t> scanlines;
    uint64_t compressed_offset = 2U;
    bool final_block = false;
    while (!final_block) {
        if (compressed.size() - compressed_offset < 9U) {
            error = "PNG stored-deflate block is truncated";
            return std::nullopt;
        }
        uint8_t const block_header = compressed[compressed_offset++];
        final_block = (block_header & 1U) != 0U;
        if ((block_header & 0x06U) != 0U) {
            error = "PNG uses a non-stored deflate block";
            return std::nullopt;
        }
        uint16_t const block_size = static_cast<uint16_t>(compressed[compressed_offset]
            | (static_cast<uint16_t>(compressed[compressed_offset + 1U]) << 8U));
        uint16_t const inverse_block_size = static_cast<uint16_t>(compressed[compressed_offset + 2U]
            | (static_cast<uint16_t>(compressed[compressed_offset + 3U]) << 8U));
        compressed_offset += 4U;
        if (static_cast<uint16_t>(~block_size) != inverse_block_size
            || block_size > compressed.size() - compressed_offset - 4U) {
            error = "PNG stored-deflate block length is invalid";
            return std::nullopt;
        }
        std::span<uint8_t const> const block = std::span<uint8_t const>{ compressed }
            .subspan(compressed_offset, block_size);
        scanlines.insert(scanlines.end(), block.begin(), block.end());
        compressed_offset += block_size;
    }
    if (compressed.size() - compressed_offset != 4U) {
        error = "PNG stored-deflate stream has an invalid trailer";
        return std::nullopt;
    }

    uint64_t const row_bytes = static_cast<uint64_t>(decoded.width) * 4U;
    uint64_t const filtered_row_bytes = row_bytes + 1U;
    if (decoded.height > std::numeric_limits<uint64_t>::max() / filtered_row_bytes) {
        error = "PNG scanline dimensions overflow";
        return std::nullopt;
    }
    uint64_t const scanline_bytes = filtered_row_bytes * decoded.height;
    if (scanline_bytes != scanlines.size()) {
        error = "PNG scanline size does not match its dimensions";
        return std::nullopt;
    }
    decoded.rgba8.reserve(row_bytes * decoded.height);
    for (uint32_t row = 0U; row < decoded.height; ++row) {
        uint64_t const row_offset = static_cast<uint64_t>(row) * filtered_row_bytes;
        if (scanlines[row_offset] != 0U) {
            error = "PNG row filter is unsupported";
            return std::nullopt;
        }
        std::span<uint8_t const> const pixels = std::span<uint8_t const>{ scanlines }
            .subspan(row_offset + 1U, row_bytes);
        decoded.rgba8.insert(decoded.rgba8.end(), pixels.begin(), pixels.end());
    }
    error.clear();
    return decoded;
}
#endif

} // namespace

TEST(PlayerClientFocusTest, CursorCaptureTracksFocusAndDropsStaleCursorSamples)
{
    client::PlayerClient player_client;
    core::platform::glfw::GlfwWindow focus_sink{
        core::platform::glfw::WindowDescriptor{
            .width = 64U,
            .height = 64U,
            .title = "MinecraftClone focus sink",
        },
    };
    GLFWwindow* const player_window = client::PlayerClientFocusTestAccess::nativeWindow(player_client);
    EXPECT_EQ(glfwGetInputMode(player_window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);

    glfwFocusWindow(player_window);
    ASSERT_TRUE(waitForFocusState(player_client, true));
    EXPECT_EQ(glfwGetInputMode(player_window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);

    client::PlayerClientFocusTestAccess::seedCursorSample(player_client);
    glfwFocusWindow(focus_sink.nativeHandle());
    ASSERT_TRUE(waitForFocusState(player_client, false));
    EXPECT_EQ(glfwGetInputMode(player_window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));

    client::PlayerClientFocusTestAccess::seedCursorSample(player_client);
    glfwFocusWindow(player_window);
    ASSERT_TRUE(waitForFocusState(player_client, true));
    EXPECT_EQ(glfwGetInputMode(player_window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));

    glfwSetCursorPos(player_window, 100.0, 100.0);
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    double const yaw_before_cursor_motion = client::PlayerClientFocusTestAccess::lookYaw(player_client);
    glfwSetCursorPos(player_window, 200.0, 100.0);
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    EXPECT_DOUBLE_EQ(client::PlayerClientFocusTestAccess::lookYaw(player_client), yaw_before_cursor_motion);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));
}

TEST(PlayerClientFocusTest, PlayingCursorCaptureRecoversAfterPlatformModeResetWithoutModifierInput)
{
    client::PlayerClient player_client;
    core::platform::glfw::GlfwWindow focus_sink{
        core::platform::glfw::WindowDescriptor{
            .width = 64U,
            .height = 64U,
            .title = "MinecraftClone playing focus sink",
        },
    };
    GLFWwindow* const window = client::PlayerClientFocusTestAccess::nativeWindow(player_client);
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client));
    glfwFocusWindow(window);
    ASSERT_TRUE(waitForFocusState(player_client, true));
    ASSERT_EQ(glfwGetInputMode(window, GLFW_CURSOR), GLFW_CURSOR_DISABLED);

    // The platform can restore its normal cursor while the game's desired capture remains active.
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    client::PlayerClientFocusTestAccess::seedCursorSample(player_client);
    client::PlayerClientFocusTestAccess::servicePlatformEvents(player_client);
    EXPECT_EQ(glfwGetInputMode(window, GLFW_CURSOR), GLFW_CURSOR_DISABLED);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));

    client::PlayerClientFocusTestAccess::seedCursorSample(player_client);
    client::PlayerClientFocusTestAccess::servicePlatformEvents(player_client);
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));

    glfwFocusWindow(focus_sink.nativeHandle());
    ASSERT_TRUE(waitForFocusState(player_client, false));
    EXPECT_EQ(glfwGetInputMode(window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));
    client::PlayerClientFocusTestAccess::seedCursorSample(player_client);
    glfwFocusWindow(window);
    ASSERT_TRUE(waitForFocusState(player_client, true));
    EXPECT_EQ(glfwGetInputMode(window, GLFW_CURSOR), GLFW_CURSOR_DISABLED);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasCursorSample(player_client));
}

TEST(PlayerClientFocusTest, ContinuousLookRequiresPlayingAndWindowFocus)
{
    for (client::WorldLoadingState const state : {
             client::WorldLoadingState::Idle,
             client::WorldLoadingState::Creating,
             client::WorldLoadingState::PreparingAtlas,
             client::WorldLoadingState::PreparingSpawn,
             client::WorldLoadingState::ReadyForStart,
             client::WorldLoadingState::Starting,
             client::WorldLoadingState::Failed,
             client::WorldLoadingState::Cancelled,
         }) {
        EXPECT_FALSE(client::playerClientContinuousLookEnabled(state, true));
        EXPECT_FALSE(client::playerClientContinuousLookEnabled(state, false));
    }
    EXPECT_TRUE(client::playerClientContinuousLookEnabled(client::WorldLoadingState::Playing, true));
    EXPECT_FALSE(client::playerClientContinuousLookEnabled(client::WorldLoadingState::Playing, false));
    EXPECT_FALSE(client::playerClientContinuousLookEnabled(client::WorldLoadingState::Playing, true, true));
    EXPECT_FALSE(client::playerClientMovementInputEnabled(true));
    EXPECT_TRUE(client::playerClientMovementInputEnabled(false));
    EXPECT_FALSE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::ReadyForStart,
        true,
        true,
        true
    ));
}

TEST(PlayerClientFocusTest, BlockBreakInputStartsRenewsAndReleasesOnlyOneAuthorizedHold)
{
    using Clock = std::chrono::steady_clock;
    client::detail::BlockBreakInputState state;
    shared::WorldCoordinate const target{ .x = 12, .y = 34, .z = 56 };
    Clock::time_point const started_at{};

    auto const started = client::detail::updateBlockBreakInput(state, {
        .held = true,
        .fresh_press = true,
        .permitted = true,
        .target = target,
        .now = started_at,
    });
    EXPECT_TRUE(started.submit_hold);
    EXPECT_FALSE(started.submit_release);
    ASSERT_TRUE(state.active);
    EXPECT_EQ(state.target, target);

    auto const before_renewal = client::detail::updateBlockBreakInput(state, {
        .held = true,
        .fresh_press = false,
        .permitted = true,
        .target = target,
        .now = started_at + std::chrono::milliseconds{99},
    });
    EXPECT_FALSE(before_renewal.submit_hold);
    EXPECT_FALSE(before_renewal.submit_release);

    auto const renewed = client::detail::updateBlockBreakInput(state, {
        .held = true,
        .fresh_press = false,
        .permitted = true,
        .target = target,
        .now = started_at + std::chrono::milliseconds{100},
    });
    EXPECT_TRUE(renewed.submit_hold);
    EXPECT_FALSE(renewed.submit_release);

    auto const released = client::detail::updateBlockBreakInput(state, {
        .held = false,
        .fresh_press = false,
        .permitted = false,
        .now = started_at + std::chrono::milliseconds{110},
    });
    EXPECT_FALSE(released.submit_hold);
    EXPECT_TRUE(released.submit_release);
    EXPECT_FALSE(state.active);

    auto const still_released = client::detail::updateBlockBreakInput(state, {
        .held = false,
        .fresh_press = false,
        .permitted = false,
        .now = started_at + std::chrono::milliseconds{120},
    });
    EXPECT_FALSE(still_released.submit_hold);
    EXPECT_FALSE(still_released.submit_release);
}

TEST(PlayerClientFocusTest, BlockBreakInputRequiresFreshPressAndRestartsOnTargetChange)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point const now{};
    client::detail::BlockBreakInputState state;
    shared::WorldCoordinate const first{ .x = 1, .y = 2, .z = 3 };
    shared::WorldCoordinate const second{ .x = 4, .y = 5, .z = 6 };

    auto const held_across_gate = client::detail::updateBlockBreakInput(state, {
        .held = true,
        .fresh_press = false,
        .permitted = true,
        .target = first,
        .now = now,
    });
    EXPECT_FALSE(held_across_gate.submit_hold);
    EXPECT_FALSE(state.active);

    auto const started = client::detail::updateBlockBreakInput(state, {
        .held = true,
        .fresh_press = true,
        .permitted = true,
        .target = first,
        .now = now,
    });
    ASSERT_TRUE(started.submit_hold);

    auto const retargeted = client::detail::updateBlockBreakInput(state, {
        .held = true,
        .fresh_press = false,
        .permitted = true,
        .target = second,
        .now = now + std::chrono::milliseconds{20},
    });
    EXPECT_TRUE(retargeted.submit_release);
    EXPECT_TRUE(retargeted.submit_hold);
    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.target, second);
    EXPECT_EQ(state.next_renewal, now + std::chrono::milliseconds{120});
}

TEST(PlayerClientFocusTest, PlacementMaterialCycleUsesOnlyServerAllowedSolids)
{
    constexpr uint8_t ALLOWED = static_cast<uint8_t>((1U << static_cast<uint8_t>(shared::Block::Dirt))
        | (1U << static_cast<uint8_t>(shared::Block::Ice)));

    EXPECT_EQ(client::detail::nextPlaceableBlock(ALLOWED, shared::Block::Dirt), shared::Block::Ice);
    EXPECT_EQ(client::detail::nextPlaceableBlock(ALLOWED, shared::Block::Ice), shared::Block::Dirt);
    EXPECT_EQ(client::detail::nextPlaceableBlock(ALLOWED, shared::Block::Stone), shared::Block::Dirt);
    EXPECT_FALSE(client::detail::nextPlaceableBlock(0U, shared::Block::Stone).has_value());
    EXPECT_FALSE(client::detail::nextPlaceableBlock(
        static_cast<uint8_t>(1U << static_cast<uint8_t>(shared::Block::Air)),
        shared::Block::Stone
    ).has_value());
}

TEST(PlayerClientFocusTest, BreakProgressIsDerivedOnlyFromBoundedServerProgressResults)
{
    shared::ServerBlockActionResultMessage status{
        .status = shared::BlockActionStatus::Progress,
        .completed_intervals = 5U,
        .required_intervals = 10U,
    };
    EXPECT_FLOAT_EQ(client::detail::authoritativeBreakProgress(status), 0.5F);

    status.completed_intervals = 20U;
    EXPECT_FLOAT_EQ(client::detail::authoritativeBreakProgress(status), 1.0F);
    status.required_intervals = 0U;
    EXPECT_FLOAT_EQ(client::detail::authoritativeBreakProgress(status), 0.0F);
    status.status = shared::BlockActionStatus::Accepted;
    status.required_intervals = 10U;
    EXPECT_FLOAT_EQ(client::detail::authoritativeBreakProgress(status), 0.0F);
}

TEST(PlayerClientFocusTest, CustomModeLabelRequiresAnExtraGrantBeyondBasePreset)
{
    auto make_status = [](shared::GameMode const mode) {
        shared::EffectiveInteractionRights const rights = shared::effectiveInteractionRights(mode);
        shared::ServerBlockActionResultMessage status{ .mode = mode, .effective_actions = rights.actions.bits };
        status.effective_actions |= static_cast<uint8_t>(
            1U << static_cast<uint8_t>(shared::InteractionAction::ChangeMode)
        );
        for (shared::Block const material : {
            shared::Block::Stone, shared::Block::Grass, shared::Block::Dirt,
            shared::Block::Bedrock, shared::Block::Snow, shared::Block::Ice,
        }) {
            uint8_t const bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(material));
            if (rights.canBreak(material)) {
                status.break_materials |= bit;
            }
            if (rights.canPlace(material)) {
                status.place_materials |= bit;
            }
        }
        return status;
    };

    shared::ServerBlockActionResultMessage survival = make_status(shared::GameMode::Survival);
    EXPECT_FALSE(client::detail::hasCustomBlockActionGrants(survival));
    survival.effective_actions |= static_cast<uint8_t>(
        1U << static_cast<uint8_t>(shared::InteractionAction::Flight)
    );
    EXPECT_TRUE(client::detail::hasCustomBlockActionGrants(survival));

    survival = make_status(shared::GameMode::Survival);
    survival.break_materials |= static_cast<uint8_t>(1U << static_cast<uint8_t>(shared::Block::Bedrock));
    EXPECT_TRUE(client::detail::hasCustomBlockActionGrants(survival));

    survival = make_status(shared::GameMode::Survival);
    survival.effective_actions &= static_cast<uint8_t>(~(
        1U << static_cast<uint8_t>(shared::InteractionAction::Break)
    ));
    survival.break_materials = 0xffU;
    EXPECT_FALSE(client::detail::hasCustomBlockActionGrants(survival));

    EXPECT_FALSE(client::detail::hasCustomBlockActionGrants(make_status(shared::GameMode::Creative)));
}

TEST(PlayerClientFocusTest, WorldMapOwnsInputAndClosingWithHeldKeysRequiresFreshRelease)
{
    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client));
    GLFWwindow* const window = client::PlayerClientFocusTestAccess::nativeWindow(player_client);
    glfwFocusWindow(window);
    ASSERT_TRUE(waitForFocusState(player_client, true));

    static constexpr std::array<int, 1U> MAP_KEY{GLFW_KEY_M};
    static constexpr std::array<int, 2U> MAP_AND_W_KEYS{GLFW_KEY_M, GLFW_KEY_W};
    static constexpr std::array<int, 1U> W_KEY{GLFW_KEY_W};
    static constexpr std::array<int, 0U> NO_KEYS{};

    static_cast<void>(client::PlayerClientFocusTestAccess::updatePlatformInputSnapshot(player_client, MAP_KEY));
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::worldMapOpen(player_client));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::worldMapLayerEnabled(
        player_client,
        client::S2WorldMapLayer::Wind
    ));

    static_cast<void>(client::PlayerClientFocusTestAccess::updatePlatformInputSnapshot(player_client, MAP_AND_W_KEYS));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::worldMapLayerEnabled(
        player_client,
        client::S2WorldMapLayer::Wind
    ));
    shared::Direction const while_map_open =
        client::PlayerClientFocusTestAccess::gameplayInput(player_client);
    EXPECT_EQ(while_map_open.x, 0U);
    EXPECT_EQ(while_map_open.y, 0U);
    EXPECT_EQ(while_map_open.z, 0U);

    static_cast<void>(client::PlayerClientFocusTestAccess::updatePlatformInputSnapshot(player_client, W_KEY));
    static_cast<void>(client::PlayerClientFocusTestAccess::updatePlatformInputSnapshot(player_client, MAP_AND_W_KEYS));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::worldMapOpen(player_client));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));
    shared::Direction const after_close_while_held =
        client::PlayerClientFocusTestAccess::gameplayInput(player_client);
    EXPECT_EQ(after_close_while_held.x, 0U);
    EXPECT_EQ(after_close_while_held.y, 0U);
    EXPECT_EQ(after_close_while_held.z, 0U);

    static_cast<void>(client::PlayerClientFocusTestAccess::updatePlatformInputSnapshot(player_client, W_KEY));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));
    static_cast<void>(client::PlayerClientFocusTestAccess::updatePlatformInputSnapshot(player_client, NO_KEYS));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));
}

TEST(PlayerClientFocusTest, ScreenshotPngEncoderWritesRgbaChunksWithValidChecksums)
{
    constexpr std::array<uint8_t, 8U> PIXELS{ 255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U };
    std::string error;
    std::optional<std::vector<uint8_t>> const png = client::detail::encodeRgba8Png(2U, 1U, PIXELS, error);
    ASSERT_TRUE(png.has_value()) << error;
    constexpr std::array<uint8_t, 8U> SIGNATURE{ 137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U };
    ASSERT_GE(png->size(), SIGNATURE.size());
    EXPECT_TRUE(std::equal(SIGNATURE.begin(), SIGNATURE.end(), png->begin()));

    auto const readBigEndian32 = [&png](uint64_t const offset) {
        return (static_cast<uint32_t>((*png)[offset]) << 24U)
            | (static_cast<uint32_t>((*png)[offset + 1U]) << 16U)
            | (static_cast<uint32_t>((*png)[offset + 2U]) << 8U)
            | static_cast<uint32_t>((*png)[offset + 3U]);
    };
    auto const checksum = [](std::span<uint8_t const> const bytes) {
        uint32_t value = 0xFFFF'FFFFU;
        for (uint8_t const byte : bytes) {
            value ^= byte;
            for (uint32_t bit = 0U; bit < 8U; ++bit) {
                value = (value >> 1U) ^ (0xEDB8'8320U & (0U - (value & 1U)));
            }
        }
        return ~value;
    };
    std::vector<std::string> chunk_types;
    uint64_t const png_size = png->size();
    for (uint64_t offset = SIGNATURE.size(); offset < png_size;) {
        ASSERT_LE(offset + 12U, png_size);
        uint64_t const chunk_size = readBigEndian32(offset);
        ASSERT_LE(chunk_size, png_size - offset - 12U);
        std::span<uint8_t const> const chunk{ *png };
        std::span<uint8_t const> const type_and_data = chunk.subspan(offset + 4U, 4U + chunk_size);
        EXPECT_EQ(checksum(type_and_data), readBigEndian32(offset + 8U + chunk_size));
        std::span<uint8_t const> const type = chunk.subspan(offset + 4U, 4U);
        chunk_types.emplace_back(type.begin(), type.end());
        if (chunk_types.back() == "IHDR") {
            EXPECT_EQ(chunk_size, 13U);
            EXPECT_EQ(readBigEndian32(offset + 8U), 2U);
            EXPECT_EQ(readBigEndian32(offset + 12U), 1U);
            EXPECT_EQ((*png)[offset + 16U], 8U);
            EXPECT_EQ((*png)[offset + 17U], 6U);
        }
        offset += 12U + chunk_size;
    }
    EXPECT_EQ(chunk_types, (std::vector<std::string>{ "IHDR", "IDAT", "IEND" }));

    error.clear();
    EXPECT_FALSE(client::detail::encodeRgba8Png(0U, 1U, PIXELS, error).has_value());
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(client::detail::encodeRgba8Png(2U, 1U, std::span<uint8_t const>{ PIXELS }.first(4U), error)
        .has_value());
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(client::detail::encodeRgba8Png(
        std::numeric_limits<uint32_t>::max(),
        1U,
        std::span<uint8_t const>{},
        error
    ).has_value());
    EXPECT_FALSE(error.empty());
}

#if defined(__APPLE__)
TEST(PlayerClientFocusTest, ScreenshotPngRoundTripsThroughPrivatePasteboard)
{
    static constexpr std::array<uint8_t, 8U> PIXELS{
        255U, 0U, 0U, 255U,
        0U, 255U, 0U, 255U,
    };
    std::string error;
    std::optional<std::vector<uint8_t>> const png = client::detail::encodeRgba8Png(2U, 1U, PIXELS, error);
    ASSERT_TRUE(png.has_value()) << error;

    std::string const pasteboard_name = "MinecraftClone.ScreenshotTest."
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    ASSERT_TRUE(client::detail::copyScreenshotPngToClipboard(*png, error, pasteboard_name)) << error;
    std::optional<std::vector<uint8_t>> const pasted_png =
        client::detail::takeScreenshotPngFromPasteboard(pasteboard_name, error);
    ASSERT_TRUE(pasted_png.has_value()) << error;
    EXPECT_EQ(*pasted_png, *png);

    uint32_t width = 0U;
    uint32_t height = 0U;
    std::vector<uint8_t> idat;
    uint64_t const pasted_png_size = pasted_png->size();
    for (uint64_t offset = 8U; offset < pasted_png_size;) {
        ASSERT_LE(offset + 12U, pasted_png_size);
        uint64_t const chunk_size = (static_cast<uint32_t>((*pasted_png)[offset]) << 24U)
            | (static_cast<uint32_t>((*pasted_png)[offset + 1U]) << 16U)
            | (static_cast<uint32_t>((*pasted_png)[offset + 2U]) << 8U)
            | static_cast<uint32_t>((*pasted_png)[offset + 3U]);
        ASSERT_LE(chunk_size, pasted_png_size - offset - 12U);
        std::span<uint8_t const> const chunk{ *pasted_png };
        std::span<uint8_t const> const type_bytes = chunk.subspan(offset + 4U, 4U);
        std::string const type(type_bytes.begin(), type_bytes.end());
        if (type == "IHDR") {
            ASSERT_EQ(chunk_size, 13U);
            width = (static_cast<uint32_t>((*pasted_png)[offset + 8U]) << 24U)
                | (static_cast<uint32_t>((*pasted_png)[offset + 9U]) << 16U)
                | (static_cast<uint32_t>((*pasted_png)[offset + 10U]) << 8U)
                | static_cast<uint32_t>((*pasted_png)[offset + 11U]);
            height = (static_cast<uint32_t>((*pasted_png)[offset + 12U]) << 24U)
                | (static_cast<uint32_t>((*pasted_png)[offset + 13U]) << 16U)
                | (static_cast<uint32_t>((*pasted_png)[offset + 14U]) << 8U)
                | static_cast<uint32_t>((*pasted_png)[offset + 15U]);
        } else if (type == "IDAT") {
            std::span<uint8_t const> const data = chunk.subspan(offset + 8U, chunk_size);
            idat.insert(idat.end(), data.begin(), data.end());
        }
        offset += 12U + chunk_size;
    }
    EXPECT_EQ(width, 2U);
    EXPECT_EQ(height, 1U);
    ASSERT_EQ(idat.size(), 20U);
    EXPECT_EQ(idat[0], 0x78U);
    EXPECT_EQ(idat[1], 0x01U);
    ASSERT_EQ(idat[2] & 0x06U, 0U);
    EXPECT_EQ(idat[2] & 0x01U, 1U);
    uint16_t const raw_size = static_cast<uint16_t>(idat[3] | (static_cast<uint16_t>(idat[4]) << 8U));
    uint16_t const inverse_raw_size = static_cast<uint16_t>(
        idat[5] | (static_cast<uint16_t>(idat[6]) << 8U)
    );
    EXPECT_EQ(raw_size, 9U);
    EXPECT_EQ(static_cast<uint16_t>(raw_size ^ inverse_raw_size), std::numeric_limits<uint16_t>::max());
    EXPECT_EQ(
        std::vector<uint8_t>(idat.begin() + 7, idat.begin() + 7 + raw_size),
        (std::vector<uint8_t>{ 0U, 255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U })
    );
}
#endif

TEST(PlayerClientFocusTest, ScreenshotKeyIsOneShotAndDoesNotStartReadyWorld)
{
    static constexpr std::array<int, 1U> SCREENSHOT_KEY{ GLFW_KEY_F2 };
    static constexpr std::array<int, 0U> NO_KEYS{};
    static constexpr std::array<int, 0U> NO_MOUSE_BUTTONS{};
    client::PlayerClient player_client;

    EXPECT_TRUE(client::PlayerClientFocusTestAccess::updateScreenshotCaptureKey(player_client, true));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateScreenshotCaptureKey(player_client, true));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateScreenshotCaptureKey(player_client, false));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::updateScreenshotCaptureKey(player_client, true));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        SCREENSHOT_KEY,
        NO_MOUSE_BUTTONS
    ));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        NO_KEYS,
        NO_MOUSE_BUTTONS
    ));
}

#if defined(__APPLE__)
TEST(PlayerClientFocusTest, CanRequestScreenshotAgainAfterConsumingCaptureWithoutRecreation)
{
    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client));
    client::PlayerClientFocusTestAccess::seedScreenshotScene(player_client);
    client::PlayerClientFocusTestAccess::disableDebugHud(player_client);
    std::string const pasteboard_name = "MinecraftClone.RepeatScreenshotTest."
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    client::PlayerClientFocusTestAccess::setScreenshotPasteboardName(player_client, pasteboard_name);
    std::string error;
    auto const captureAndRead = [&]() -> std::optional<DecodedTestPng> {
        if (!client::PlayerClientFocusTestAccess::captureScreenshotToClipboard(player_client, error)) {
            return std::nullopt;
        }
        std::optional<std::vector<uint8_t>> const png =
            client::detail::takeScreenshotPngFromPasteboard(pasteboard_name, error);
        if (!png.has_value()) {
            return std::nullopt;
        }
        return decodeStoredTestPng(*png, error);
    };

    ASSERT_TRUE(client::PlayerClientFocusTestAccess::setLookAngles(
        player_client,
        client::CameraAngles{ .yaw_degrees = 0.0, .pitch_degrees = -20.0 }
    ));
    std::optional<DecodedTestPng> const first_capture = captureAndRead();
    ASSERT_TRUE(first_capture.has_value()) << error;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::screenshotFeedbackSuccessful(player_client))
        << client::PlayerClientFocusTestAccess::screenshotFeedback(player_client);
    EXPECT_NEAR(client::PlayerClientFocusTestAccess::renderedCameraAngles(player_client).yaw_degrees, 0.0, 0.01);
    EXPECT_GT(first_capture->width, 0U);
    EXPECT_GT(first_capture->height, 0U);
    EXPECT_FALSE(first_capture->rgba8.empty());
    uint64_t const swapchain_generation =
        client::PlayerClientFocusTestAccess::runtimeInfo(player_client).current_swapchain_generation;

    ASSERT_TRUE(client::PlayerClientFocusTestAccess::setLookAngles(
        player_client,
        client::CameraAngles{ .yaw_degrees = 90.0, .pitch_degrees = -20.0 }
    ));
    std::optional<DecodedTestPng> const second_capture = captureAndRead();
    ASSERT_TRUE(second_capture.has_value()) << error;
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::screenshotFeedbackSuccessful(player_client))
        << client::PlayerClientFocusTestAccess::screenshotFeedback(player_client);
    EXPECT_NEAR(client::PlayerClientFocusTestAccess::renderedCameraAngles(player_client).yaw_degrees, 90.0, 0.01);
    EXPECT_EQ(second_capture->width, first_capture->width);
    EXPECT_EQ(second_capture->height, first_capture->height);
    EXPECT_EQ(
        client::PlayerClientFocusTestAccess::runtimeInfo(player_client).current_swapchain_generation,
        swapchain_generation
    );

    uint64_t changed_pixels = 0U;
    uint64_t const pixel_count = first_capture->rgba8.size() / 4U;
    ASSERT_EQ(second_capture->rgba8.size(), first_capture->rgba8.size());
    for (uint64_t pixel = 0U; pixel < pixel_count; ++pixel) {
        uint64_t const offset = pixel * 4U;
        if (first_capture->rgba8[offset] != second_capture->rgba8[offset]
            || first_capture->rgba8[offset + 1U] != second_capture->rgba8[offset + 1U]
            || first_capture->rgba8[offset + 2U] != second_capture->rgba8[offset + 2U]) {
            ++changed_pixels;
        }
    }
    EXPECT_GT(changed_pixels, pixel_count / 100U)
        << "the second private-pasteboard PNG should contain pixels from the changed camera view";
}
#endif

TEST(PlayerClientFocusTest, BenchmarkRetainsFenceConfirmedFramesAfterPlayingTransition)
{
    static constexpr uint32_t MAX_RENDER_ATTEMPTS = 32U;
    static constexpr std::chrono::seconds RENDER_TIMEOUT{ 5 };

    client::PlayerClient player_client{
        shared::WorldMode::Flight,
        std::nullopt,
        client::PlayerClientBenchmarkOptions{ .capture_frame_completion = true },
    };
    client::WorldLoadingStatus const ready_status{
        .state = client::WorldLoadingState::ReadyForStart,
    };
    client::WorldLoadingStatus const playing_status{
        .state = client::WorldLoadingState::Playing,
    };
    std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + RENDER_TIMEOUT;

    ASSERT_TRUE(client::PlayerClientFocusTestAccess::renderLoadingStatusFrame(
        player_client,
        ready_status,
        deadline
    ));
    client::RendererFrameProgress const loading_progress =
        client::PlayerClientFocusTestAccess::frameProgress(player_client);
    ASSERT_TRUE(loading_progress.submitted.has_value());
    uint64_t const last_loading_sequence = loading_progress.submitted->sequence;

    client::RendererFrameProgress playing_progress;
    for (uint32_t attempt = 0U;
         attempt < MAX_RENDER_ATTEMPTS && std::chrono::steady_clock::now() < deadline;
         ++attempt) {
        static_cast<void>(client::PlayerClientFocusTestAccess::renderLoadingStatusFrame(
            player_client,
            playing_status,
            deadline
        ));
        playing_progress = client::PlayerClientFocusTestAccess::frameProgress(player_client);
        if (playing_progress.completed.has_value()
            && playing_progress.completed->sequence > last_loading_sequence) {
            break;
        }
    }

    ASSERT_TRUE(playing_progress.completed.has_value());
    EXPECT_GT(playing_progress.completed->sequence, last_loading_sequence);
}

TEST(PlayerClientFocusTest, NormalPlayingFramesObserveCompletionsWithoutRetainingHistory)
{
    static constexpr uint32_t MAX_RENDER_ATTEMPTS = 8U;
    static constexpr std::chrono::seconds RENDER_TIMEOUT{ 5 };

    client::PlayerClient player_client;
    client::WorldLoadingStatus const playing_status{
        .state = client::WorldLoadingState::Playing,
    };
    std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + RENDER_TIMEOUT;
    bool presented = false;
    for (uint32_t attempt = 0U;
         attempt < MAX_RENDER_ATTEMPTS && std::chrono::steady_clock::now() < deadline;
         ++attempt) {
        presented = client::PlayerClientFocusTestAccess::renderLoadingStatusFrame(
            player_client,
            playing_status,
            deadline
        ) || presented;
    }

    client::RendererRuntimeInfo const runtime = client::PlayerClientFocusTestAccess::runtimeInfo(player_client);
    client::RendererFrameProgress const progress = client::PlayerClientFocusTestAccess::frameProgress(player_client);
    ASSERT_TRUE(presented);
    EXPECT_GT(runtime.submitted_frame_count, 0U);
    EXPECT_TRUE(runtime.diagnostic_presentation_acquire.captured);
    ASSERT_TRUE(progress.completed.has_value());
    EXPECT_TRUE(progress.completed_observed_at.has_value());
    EXPECT_EQ(progress.completed->swapchain_generation, runtime.current_swapchain_generation);
    EXPECT_TRUE(runtime.retired_frame_completions.empty());
}

TEST(PlayerClientFocusTest, OrdinaryPlayingFramesKeepPresentingAfterRepeatedScreenshotHints)
{
    static constexpr uint32_t FRAME_ATTEMPTS = 600U;
    static constexpr uint64_t MINIMUM_SUBMITTED_FRAMES = 500U;
    static constexpr std::chrono::seconds TEST_TIMEOUT{ 15 };

    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client));
    ASSERT_EQ(player_client.worldLoadingStatus().state, client::WorldLoadingState::Playing);

    std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + TEST_TIMEOUT;
    for (uint32_t attempt = 0U;
         attempt < FRAME_ATTEMPTS && std::chrono::steady_clock::now() < deadline;
         ++attempt) {
        client::PlayerClientFocusTestAccess::renderFrame(player_client);
    }

    client::RendererRuntimeInfo const runtime = client::PlayerClientFocusTestAccess::runtimeInfo(player_client);
    EXPECT_GT(runtime.submitted_frame_count, MINIMUM_SUBMITTED_FRAMES)
        << "ordinary Playing frames should keep presenting after sustained screenshot-hint display";
}

TEST(PlayerClientFocusTest, MouseStartPressIsConsumedUntilRelease)
{
    static constexpr int32_t START_KEY = GLFW_KEY_SPACE;
    static constexpr int32_t START_MOUSE_BUTTON = GLFW_MOUSE_BUTTON_LEFT;
    std::array<int, 1U> const pressed_keys{ START_KEY };
    std::array<int, 1U> const pressed_mouse_buttons{ START_MOUSE_BUTTON };
    std::array<int, 0U> const no_keys{};
    std::array<int, 0U> const no_mouse_buttons{};

    EXPECT_FALSE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::ReadyForStart,
        false,
        true
    ));
    EXPECT_FALSE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::PreparingSpawn,
        true,
        true
    ));
    EXPECT_TRUE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::ReadyForStart,
        true,
        true
    ));

    client::PlayerClient player_client;
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        pressed_keys,
        pressed_mouse_buttons
    ));
    client::PlayerClientFocusTestAccess::captureStartTransitionInputs(player_client);
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));

    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        pressed_keys,
        pressed_mouse_buttons
    ));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));

    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        no_keys,
        no_mouse_buttons
    ));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        no_keys,
        pressed_mouse_buttons
    ));
}

TEST(PlayerClientFocusTest, MouseOnlyStartPressCreatesAnEdgeAndIsConsumedUntilRelease)
{
    static constexpr int32_t START_MOUSE_BUTTON = GLFW_MOUSE_BUTTON_LEFT;
    std::array<int, 0U> const no_keys{};
    std::array<int, 0U> const no_mouse_buttons{};
    std::array<int, 1U> const pressed_mouse_buttons{ START_MOUSE_BUTTON };

    client::PlayerClient player_client;
    bool const mouse_press_is_new = client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        no_keys,
        pressed_mouse_buttons
    );
    EXPECT_TRUE(mouse_press_is_new);
    EXPECT_TRUE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::ReadyForStart,
        true,
        mouse_press_is_new
    ));
    client::PlayerClientFocusTestAccess::captureStartTransitionInputs(player_client);
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));

    bool const held_mouse_is_new = client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        no_keys,
        pressed_mouse_buttons
    );
    EXPECT_FALSE(held_mouse_is_new);
    EXPECT_FALSE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::ReadyForStart,
        true,
        held_mouse_is_new
    ));
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));

    EXPECT_FALSE(client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        no_keys,
        no_mouse_buttons
    ));
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));

    bool const repressed_mouse_is_new = client::PlayerClientFocusTestAccess::updateLoadingInputSnapshot(
        player_client,
        no_keys,
        pressed_mouse_buttons
    );
    EXPECT_TRUE(repressed_mouse_is_new);
    EXPECT_TRUE(client::playerClientLoadingStartInputEligible(
        client::WorldLoadingState::ReadyForStart,
        true,
        repressed_mouse_is_new
    ));
    client::PlayerClientFocusTestAccess::captureStartTransitionInputs(player_client);
    EXPECT_TRUE(client::PlayerClientFocusTestAccess::hasStartTransitionInputs(player_client));
}

TEST(PlayerClientFocusTest, NormalRenderShowsGenerationMapBeforeReady)
{
    client::PlayerClient player_client{shared::WorldMode::Flight, std::nullopt, startupFrameCaptureOptions()};
    client::PlayerClientFocusTestAccess::restoreOrdinaryStartupAfterCaptureOptIn(player_client);
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginInitialLoading(player_client));
    auto const backdrop = client::PlayerClientFocusTestAccess::capturePlayerFrame(player_client);
    ASSERT_TRUE(backdrop);
    auto const snapshot = controlledGenerationSnapshot();
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::publishGeneration(player_client, {
        .stage = generation::GenerationStage::Tectonics, .completed = 1U, .total = 12U, .snapshot = snapshot,
    }));
    ASSERT_EQ(player_client.worldLoadingStatus().state, client::WorldLoadingState::PreparingAtlas);
    auto const generation = client::PlayerClientFocusTestAccess::capturePlayerFrame(player_client);
    ASSERT_TRUE(generation);
    auto const backdrop_crop = cropMapInterior(*backdrop, loadingContentScale(player_client));
    auto const generation_crop = cropMapInterior(*generation, loadingContentScale(player_client));
    auto const directory = startupEvidenceDirectory();
    EXPECT_TRUE(saveMapCapture(directory / "controlled-backdrop", backdrop_crop));
    EXPECT_TRUE(saveMapCapture(directory / "controlled-pre-ready", generation_crop));
    std::ofstream state{directory / "controlled-pre-ready.json"};
    state << "{\"state\":" << static_cast<uint32_t>(player_client.worldLoadingStatus().state)
        << ",\"backdrop_hash\":" << pixelHash(backdrop_crop.rgba8)
        << ",\"generation_hash\":" << pixelHash(generation_crop.rgba8) << "}\n";
    EXPECT_TRUE(loadingCanvasVisible(*generation)) << "loading canvas must replace the sky/terrain backdrop";
    auto const variation = mapSpatialVariation(generation_crop);
    EXPECT_TRUE(variation.geographic()) << "map-only ROI must contain geographic spatial variation: colors="
        << variation.unique_rgb_colors << ", differing neighbors=" << variation.differing_horizontal_neighbors;
    EXPECT_NE(pixelHash(generation_crop.rgba8), pixelHash(backdrop_crop.rgba8))
        << "actual PlayerClient::render must draw the map before ReadyForStart, outside the text overlay";
}

TEST(PlayerClientFocusTest, RetainedGenerationSnapshotDoesNotRestartVisibleAnimationClock)
{
    client::PlayerClient player_client{shared::WorldMode::Flight, std::nullopt, startupFrameCaptureOptions()};
    client::PlayerClientFocusTestAccess::restoreOrdinaryStartupAfterCaptureOptIn(player_client);
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginInitialLoading(player_client));
    auto const snapshot = controlledGenerationSnapshot();
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::publishGeneration(player_client, {
        .stage = generation::GenerationStage::Tectonics, .completed = 1U, .total = 12U, .snapshot = snapshot,
    }));
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    auto const before = client::PlayerClientFocusTestAccess::generationVisibleElapsed(player_client);
    ASSERT_GE(before, std::chrono::milliseconds{250});
    auto const retained = std::make_shared<generation::GenerationSnapshot const>(*snapshot);
    ASSERT_NE(retained.get(), snapshot.get());
    ASSERT_EQ(snapshotContentHash(*retained), snapshotContentHash(*snapshot));
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::publishGeneration(player_client, {
        .stage = generation::GenerationStage::Tectonics, .completed = 2U, .total = 12U, .snapshot = retained,
    }));
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    auto const after = client::PlayerClientFocusTestAccess::generationVisibleElapsed(player_client);
    std::ofstream state{startupEvidenceDirectory() / "retained-snapshot-clock.json"};
    state << "{\"before_visible_ms\":" << before.count() << ",\"after_visible_ms\":" << after.count()
        << ",\"content_hash\":" << snapshotContentHash(*snapshot) << ",\"source_before\":"
        << reinterpret_cast<uintptr_t>(snapshot.get()) << ",\"source_after\":"
        << reinterpret_cast<uintptr_t>(retained.get()) << "}\n";
    EXPECT_GE(after, before) << "freshly decoded retained content must not restart the same animation";
}

TEST(PlayerClientFocusTest, NormalLocalStartupRecordsMovingGenerationMapBeforeReady)
{
    using Clock = std::chrono::steady_clock;
    auto const accepted_at = Clock::now();
    client::PlayerClient player_client{shared::WorldMode::Flight, std::nullopt, startupFrameCaptureOptions()};
    client::PlayerClientFocusTestAccess::restoreOrdinaryStartupAfterCaptureOptIn(player_client);
    auto const configuration = shared::World::canonicalConfiguration();
    generation::GenerationConfiguration generation_configuration{};
    generation_configuration.seed = configuration.seed;
    auto preparation = std::make_shared<server::WorldPreparation>(
        generation_configuration, configuration.algorithm_version, accepted_at);
    server::GameServer server{0U, {}, shared::WorldMode::Flight, configuration,
        shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation, server::GameServer::BindMode::Loopback};
    ASSERT_TRUE(server.isValid());
    std::exception_ptr server_error;
    std::atomic_bool server_failed{false};
    std::jthread server_thread{[&](std::stop_token const& cancellation) {
        std::atomic_bool stop_requested{false};
        std::stop_callback callback{cancellation, [&] { stop_requested.store(true); }};
        try {
            server.run(stop_requested);
        } catch (...) {
            server_error = std::current_exception();
            server_failed.store(true);
        }
    }};
    auto const directory = startupEvidenceDirectory() / "normal-startup";
    std::filesystem::create_directories(directory);
    std::ofstream events{directory / "frames.jsonl"};
    events << "{\"event\":\"configuration\",\"seed\":" << generation_configuration.seed
        << ",\"epochs\":" << generation_configuration.epochs
        << ",\"capture_hz\":4,\"max_frames\":120,\"roi_contract\":\"UiLoadingLayout.map_viewport inset 10 percent\""
        << ",\"windows_capture_only_constructor_opt_in\":"
#if defined(_WIN32)
        << "true"
#else
        << "false"
#endif
        << ",\"runtime_benchmark_mode\":false}\n";
    std::optional<client::WorldLoadingState> capture_request_state;
    std::optional<client::RendererFrameIdentity> capture_request_submitted;
    int64_t capture_request_ms = 0;
    uint32_t captures = 0U;
    uint32_t pre_ready_captures = 0U;
    uint32_t geographic_pre_ready_captures = 0U;
    uint32_t retained_source_replacements = 0U;
    uint32_t clock_regressions = 0U;
    std::set<uint64_t> pre_ready_map_hashes;
    generation::GenerationSnapshot const* previous_source = nullptr;
    uint64_t source_hash = 0U;
    std::chrono::milliseconds previous_visible{};
    std::chrono::milliseconds maximum_pre_ready_visible{};
    auto next_capture = Clock::now();
    std::optional<Clock::time_point> ready_at;
    struct RenderStateSummary final {
        uint64_t loops = 0U;
        uint64_t successful_presentations = 0U;
        uint64_t new_submissions = 0U;
        std::chrono::nanoseconds render_total{};
        std::chrono::nanoseconds render_max{};
    };
    std::map<uint32_t, RenderStateSummary> render_states;
    uint64_t previous_submitted_sequence = 0U;
    client::GameClientBenchmarkHooks hooks{};
    hooks.deadline = accepted_at + std::chrono::seconds{35};
    hooks.on_loop = [&](client::GameClientLoopSample const& sample) {
        auto const now = Clock::now();
        auto const status = player_client.worldLoadingStatus();
        auto& render_state = render_states[static_cast<uint32_t>(status.state)];
        ++render_state.loops;
        render_state.successful_presentations += sample.presentation_succeeded ? 1U : 0U;
        render_state.render_total += sample.render_duration;
        render_state.render_max = std::max(render_state.render_max, sample.render_duration);
        auto const submitted = client::PlayerClientFocusTestAccess::frameProgress(player_client).submitted;
        if (submitted && submitted->sequence != previous_submitted_sequence) {
            ++render_state.new_submissions;
            previous_submitted_sequence = submitted->sequence;
        }
        auto const visible = client::PlayerClientFocusTestAccess::generationVisibleElapsed(player_client);
        auto const request_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - accepted_at).count();
        bool const pre_ready = status.state == client::WorldLoadingState::PreparingAtlas
            || status.state == client::WorldLoadingState::PreparingSpawn;
        auto write_observation = [&](char const* event) {
            events << "{\"event\":\"" << event << "\",\"observed_request_ms\":" << request_ms
                << ",\"state\":" << static_cast<uint32_t>(status.state)
                << ",\"join_epoch\":" << player_client.worldLoadingJoinEpoch()
                << ",\"progress_stage\":" << static_cast<uint32_t>(status.generation_stage)
                << ",\"progress_completed\":" << status.generation_completed
                << ",\"progress_total\":" << status.generation_total
                << ",\"source_ptr\":" << reinterpret_cast<uintptr_t>(status.map_snapshot.get())
                << ",\"source_hash\":" << source_hash
                << ",\"snapshot_stage\":" << (status.map_snapshot ? static_cast<int>(status.map_snapshot->stage) : -1)
                << ",\"visible_elapsed_ms\":" << visible.count()
                << ",\"relief_phase\":" << std::fmod(static_cast<double>(visible.count()), 4'000.0) / 4'000.0;
        };
        if (status.map_snapshot.get() != previous_source) {
            uint64_t const new_hash = status.map_snapshot ? snapshotContentHash(*status.map_snapshot) : 0U;
            if (status.map_snapshot && previous_source != nullptr && new_hash == source_hash) {
                ++retained_source_replacements;
                if (visible < previous_visible) {
                    ++clock_regressions;
                }
            }
            previous_source = status.map_snapshot.get();
            source_hash = new_hash;
            write_observation("source");
            events << "}\n";
        }
        if (auto frame = client::PlayerClientFocusTestAccess::takeFrameCapture(player_client); frame) {
            auto const crop = cropMapInterior(*frame, loadingContentScale(player_client));
            uint64_t const hash = pixelHash(crop.rgba8);
            EXPECT_TRUE(saveMapCapture(directory / ("map-" + std::to_string(captures)), crop));
            EXPECT_TRUE(saveFullFrame(directory / ("full-" + std::to_string(captures) + ".png"), *frame));
            write_observation("capture");
            auto const bounds = mapCaptureBounds(*frame, loadingContentScale(player_client));
            auto const variation = mapSpatialVariation(crop);
            auto const progress = client::PlayerClientFocusTestAccess::frameProgress(player_client);
            events << ",\"index\":" << captures << ",\"map_crop_hash\":" << hash
                << ",\"request_state\":" << (capture_request_state ? static_cast<int>(*capture_request_state) : -1)
                << ",\"request_ms\":" << capture_request_ms
                << ",\"request_submitted_sequence\":"
                << (capture_request_submitted ? capture_request_submitted->sequence : 0U)
                << ",\"observed_completed_sequence\":" << (progress.completed ? progress.completed->sequence : 0U)
                << ",\"observed_completed_swapchain\":"
                << (progress.completed ? progress.completed->swapchain_generation : 0U)
                << ",\"dpi_scale\":" << loadingContentScale(player_client)
                << ",\"frame_width\":" << frame->width << ",\"frame_height\":" << frame->height
                << ",\"crop_left\":" << bounds.x
                << ",\"crop_top\":" << bounds.y
                << ",\"crop_width\":" << bounds.width
                << ",\"crop_height\":" << bounds.height
                << ",\"loading_canvas_visible\":" << (loadingCanvasVisible(*frame) ? "true" : "false")
                << ",\"unique_rgb_colors\":" << variation.unique_rgb_colors
                << ",\"differing_horizontal_neighbors\":" << variation.differing_horizontal_neighbors
                << ",\"saved_side\":256}\n";
            ++captures;
            bool const requested_pre_ready = capture_request_state
                && (*capture_request_state == client::WorldLoadingState::PreparingAtlas
                    || *capture_request_state == client::WorldLoadingState::PreparingSpawn);
            if (requested_pre_ready && pre_ready && status.map_snapshot) {
                ++pre_ready_captures;
                if (variation.geographic() && loadingCanvasVisible(*frame)) {
                    ++geographic_pre_ready_captures;
                    pre_ready_map_hashes.insert(hash);
                }
            }
            capture_request_state.reset();
        }
        if (pre_ready) {
            maximum_pre_ready_visible = std::max(maximum_pre_ready_visible, visible);
        }
        if (status.state == client::WorldLoadingState::ReadyForStart && !ready_at) {
            ready_at = now;
        }
        if (!capture_request_state && captures < 120U && now >= next_capture) {
            capture_request_state = status.state;
            capture_request_submitted = client::PlayerClientFocusTestAccess::frameProgress(player_client).submitted;
            capture_request_ms = request_ms;
            client::PlayerClientFocusTestAccess::requestFrameCapture(player_client);
            next_capture = now + std::chrono::milliseconds{250};
        }
        previous_visible = visible;
        if (server_failed.load() || captures >= 120U || now - accepted_at >= std::chrono::seconds{30}
            || (ready_at && now - *ready_at >= std::chrono::seconds{1})) {
            client::PlayerClientFocusTestAccess::stop(player_client);
        }
    };
    player_client.run(core::Address::localhost(server.port()), '@', &hooks, accepted_at);
    server_thread.request_stop();
    server_thread.join();
    preparation->cancel();
    for (auto const& [state, render_state] : render_states) {
        events << "{\"event\":\"render_state_summary\",\"state\":" << state
            << ",\"loops\":" << render_state.loops
            << ",\"successful_presentations\":" << render_state.successful_presentations
            << ",\"new_submissions\":" << render_state.new_submissions
            << ",\"render_total_ns\":" << render_state.render_total.count()
            << ",\"render_max_ns\":" << render_state.render_max.count() << "}\n";
    }
    events << "{\"event\":\"summary\",\"captures\":" << captures
        << ",\"pre_ready_captures\":" << pre_ready_captures
        << ",\"geographic_pre_ready_captures\":" << geographic_pre_ready_captures
        << ",\"pre_ready_unique_map_hashes\":" << pre_ready_map_hashes.size()
        << ",\"retained_source_replacements\":" << retained_source_replacements
        << ",\"clock_regressions\":" << clock_regressions
        << ",\"maximum_pre_ready_visible_ms\":" << maximum_pre_ready_visible.count()
        << ",\"ready_observed\":" << (ready_at ? "true" : "false") << "}\n";
    ASSERT_FALSE(server_failed.load());
    EXPECT_GE(pre_ready_captures, 3U);
    EXPECT_GE(geographic_pre_ready_captures, 3U);
    EXPECT_GT(pre_ready_map_hashes.size(), 1U) << "map geometry crop must move before Ready, independently of text";
    EXPECT_EQ(clock_regressions, 0U);
    EXPECT_GE(maximum_pre_ready_visible, std::chrono::milliseconds{500});
}

TEST(PlayerClientFocusTest, ActualStartupCapturesLoadingReadyGameAndWorldMapAtTwoWindowSizes)
{
    using Clock = std::chrono::steady_clock;
    char const* const selection_value = std::getenv("MC_UI_CAPTURE_VIEWPORT");
    std::string const selection = selection_value != nullptr ? selection_value : "both";
    ASSERT_TRUE(selection_value == nullptr || selection == "1280x676" || selection == "844x346")
        << "MC_UI_CAPTURE_VIEWPORT accepts only 1280x676 or 844x346; unset selects both";
    for (auto const dimensions : {std::pair{1280, 676}, std::pair{844, 346}}) {
        std::string const viewport_name = std::to_string(dimensions.first) + "x" + std::to_string(dimensions.second);
        if (selection != "both" && selection != viewport_name) {
            continue;
        }
        SCOPED_TRACE(viewport_name);
        auto const accepted_at = Clock::now();
        client::PlayerClient player_client{
            shared::WorldMode::Flight, std::nullopt, startupFrameCaptureOptions()};
        client::PlayerClientFocusTestAccess::restoreOrdinaryStartupAfterCaptureOptIn(player_client);
        auto* window = client::PlayerClientFocusTestAccess::nativeWindow(player_client);
        glfwRestoreWindow(window);
        glfwSetWindowSize(window, dimensions.first, dimensions.second);
        glfwFocusWindow(window);
        // Settle the real window/swapchain resize before requesting a startup readback.
        client::PlayerClientFocusTestAccess::renderFrame(player_client);
        auto const directory = startupEvidenceDirectory() / "approved-views"
            / (std::to_string(dimensions.first) + "x" + std::to_string(dimensions.second));
        std::filesystem::create_directories(directory);
        std::ofstream events{directory / "frames.jsonl"};
        events << "{\"event\":\"configuration\",\"runtime_benchmark_mode\":false,"
            << "\"viewport_selection\":\"" << selection << "\",\"windows_capture_only_constructor_opt_in\":"
#if defined(_WIN32)
            << "true"
#else
            << "false"
#endif
            << ",\"deadline_seconds\":60}\n";
        auto save = [&](char const* label, client::RendererFrameCapture const& frame) {
            EXPECT_TRUE(saveFullFrame(directory / (std::string{label} + ".png"), frame));
            auto const progress = client::PlayerClientFocusTestAccess::frameProgress(player_client);
            auto const text_style = client::PlayerClientFocusTestAccess::hudTextStyle(player_client);
            int logical_width = 0;
            int logical_height = 0;
            glfwGetWindowSize(window, &logical_width, &logical_height);
            events << "{\"capture\":\"" << label << "\",\"completed_sequence\":"
                << (progress.completed ? progress.completed->sequence : 0U)
                << ",\"completed_swapchain_generation\":"
                << (progress.completed ? progress.completed->swapchain_generation : 0U)
                << ",\"state\":"
                << static_cast<uint32_t>(player_client.worldLoadingStatus().state)
                << ",\"join_epoch\":" << player_client.worldLoadingJoinEpoch()
                << ",\"elapsed_ms\":"
                << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - accepted_at).count()
                << ",\"width\":" << frame.width << ",\"height\":" << frame.height
                << ",\"logical_width\":" << logical_width << ",\"logical_height\":" << logical_height
                << ",\"window_maximized\":"
                << (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE ? "true" : "false")
                << ",\"dpi_scale\":" << loadingContentScale(player_client)
                << ",\"hud_text_style\":\""
                << (text_style == client::TextStyle::Adaptive ? "adaptive" : "fixed") << "\""
                << ",\"canonical_minimap_ready\":"
                << (client::PlayerClientFocusTestAccess::canonicalMinimapReady(player_client) ? "true" : "false")
                << ",\"world_map_open\":"
                << (client::PlayerClientFocusTestAccess::worldMapOpen(player_client) ? "true" : "false") << "}\n";
        };
        auto const preparing = client::PlayerClientFocusTestAccess::capturePlayerFrame(player_client);
        ASSERT_TRUE(preparing);
        ASSERT_EQ(player_client.worldLoadingStatus().state, client::WorldLoadingState::Idle);
        save("preparing-idle", *preparing);

        auto const configuration = shared::World::canonicalConfiguration();
        generation::GenerationConfiguration generation_configuration{};
        generation_configuration.seed = configuration.seed;
        auto preparation = std::make_shared<server::WorldPreparation>(
            generation_configuration, configuration.algorithm_version, accepted_at);
        server::GameServer server{0U, {}, shared::WorldMode::Flight, configuration,
            shared::HEIGHT_TILE_INTEREST_RADIUS, nullptr, preparation, server::GameServer::BindMode::Loopback};
        ASSERT_TRUE(server.isValid());
        std::atomic_bool server_failed{false};
        std::jthread server_thread{[&](std::stop_token const& cancellation) {
            std::atomic_bool stop_requested{false};
            std::stop_callback callback{cancellation, [&] { stop_requested.store(true); }};
            try {
                server.run(stop_requested);
            } catch (...) {
                server_failed.store(true);
            }
        }};
        enum class View { Generation, Ready, Game, AdaptiveGame, WorldMap, MaximizedGame, Complete };
        View wanted = View::Generation;
        std::optional<View> pending;
        bool start_sent = false;
        bool maximized_game_captured = false;
        auto const finish_world_map_capture = [&] {
            if (dimensions.first == 1280) {
                client::PlayerClientFocusTestAccess::closeWorldMap(player_client);
                glfwMaximizeWindow(window);
                events << "{\"input\":\"close_map_and_maximize_window\","
                    << "\"synthetic_test_input\":true}\n";
                wanted = View::MaximizedGame;
            } else {
                wanted = View::Complete;
            }
        };
        auto next_pending_diagnostic = Clock::now();
        std::chrono::nanoseconds pending_render_total{};
        client::GameClientBenchmarkHooks hooks{};
        hooks.deadline = accepted_at + std::chrono::seconds{60};
        hooks.start_input_override = [&](uint64_t) {
            auto const status = player_client.worldLoadingStatus();
            if (wanted == View::Game && !start_sent
                && status.state == client::WorldLoadingState::ReadyForStart
                && status.ready_prompt_frame_rendered_at) {
                start_sent = true;
                events << "{\"input\":\"start_input_override\",\"synthetic_test_input\":true}\n";
                return true;
            }
            return false;
        };
        hooks.on_loop = [&](client::GameClientLoopSample const& sample) {
            auto const status = player_client.worldLoadingStatus();
            if (pending) {
                pending_render_total += sample.render_duration;
                if (Clock::now() >= next_pending_diagnostic) {
                    auto const progress = client::PlayerClientFocusTestAccess::frameProgress(player_client);
                    auto const runtime = client::PlayerClientFocusTestAccess::runtimeInfo(player_client);
                    events << "{\"event\":\"pending_readback\",\"view\":" << static_cast<uint32_t>(*pending)
                        << ",\"state\":" << static_cast<uint32_t>(status.state)
                        << ",\"capture_state\":" << static_cast<uint32_t>(
                            client::PlayerClientFocusTestAccess::captureState(player_client))
                        << ",\"runtime_submitted\":" << runtime.submitted_frame_count
                        << ",\"submitted_sequence\":" << (progress.submitted ? progress.submitted->sequence : 0U)
                        << ",\"completed_sequence\":" << (progress.completed ? progress.completed->sequence : 0U)
                        << ",\"swapchain_generation\":" << runtime.current_swapchain_generation
                        << ",\"last_presentation_succeeded\":"
                        << (client::PlayerClientFocusTestAccess::lastPresentationSucceeded(player_client) ? "true" : "false")
                        << ",\"pending_render_total_ns\":" << pending_render_total.count() << "}\n";
                    events.flush();
                    next_pending_diagnostic = Clock::now() + std::chrono::seconds{1};
                }
            }
            bool const generating = (status.state == client::WorldLoadingState::PreparingAtlas
                || status.state == client::WorldLoadingState::PreparingSpawn) && status.map_snapshot;
            bool const ready = status.state == client::WorldLoadingState::ReadyForStart
                && status.ready_prompt_frame_rendered_at.has_value();
            bool const game = status.state == client::WorldLoadingState::Playing
                && client::PlayerClientFocusTestAccess::canonicalMinimapReady(player_client);
            bool const map = game && client::PlayerClientFocusTestAccess::worldMapReady(player_client);
            int framebuffer_width = 0;
            int framebuffer_height = 0;
            glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
            auto const runtime = client::PlayerClientFocusTestAccess::runtimeInfo(player_client);
            bool const maximized_game = game && !client::PlayerClientFocusTestAccess::worldMapOpen(player_client)
                && glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE
                && framebuffer_width > 0 && framebuffer_height > 0
                && runtime.width == static_cast<uint32_t>(framebuffer_width)
                && runtime.height == static_cast<uint32_t>(framebuffer_height);
            auto eligible = [&](View view) {
                return (view == View::Generation && generating) || (view == View::Ready && ready)
                    || ((view == View::Game || view == View::AdaptiveGame) && game
                        && glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_FALSE)
                    || (view == View::WorldMap && map)
                    || (view == View::MaximizedGame && maximized_game);
            };
            if (auto frame = client::PlayerClientFocusTestAccess::takeFrameCapture(player_client); frame) {
                bool const current_size = frame->width == static_cast<uint32_t>(framebuffer_width)
                    && frame->height == static_cast<uint32_t>(framebuffer_height);
                if (pending && eligible(*pending) && current_size) {
                    switch (*pending) {
                    case View::Generation: save("generation", *frame); wanted = View::Ready; break;
                    case View::Ready: save("ready", *frame); wanted = View::Game; break;
                    case View::Game:
                        save("game-minimap", *frame);
                        client::PlayerClientFocusTestAccess::setHudTextStyle(
                            player_client, client::TextStyle::Adaptive);
                        events << "{\"input\":\"set_hud_text_style\",\"style\":\"adaptive\","
                            << "\"synthetic_test_input\":true}\n";
                        wanted = View::AdaptiveGame;
                        break;
                    case View::AdaptiveGame:
                        save("game-minimap-adaptive", *frame);
                        client::PlayerClientFocusTestAccess::setHudTextStyle(
                            player_client, client::TextStyle::Fixed);
                        events << "{\"input\":\"set_hud_text_style\",\"style\":\"fixed\","
                            << "\"synthetic_test_input\":true}\n";
                        client::PlayerClientFocusTestAccess::openWorldMap(player_client);
                        events << "{\"input\":\"setWorldMapOpen\",\"synthetic_test_input\":true}\n";
                        wanted = View::WorldMap;
                        break;
                    case View::WorldMap:
                        save("world-map", *frame);
                        finish_world_map_capture();
                        break;
                    case View::MaximizedGame:
                        save("game-maximized-rich", *frame);
                        maximized_game_captured = true;
                        wanted = View::Complete;
                        break;
                    case View::Complete: break;
                    }
                }
                pending.reset();
            }
            if (!pending && eligible(wanted)) {
                pending = wanted;
                pending_render_total = {};
                next_pending_diagnostic = Clock::now();
                auto const progress = client::PlayerClientFocusTestAccess::frameProgress(player_client);
                events << "{\"event\":\"capture_request\",\"view\":" << static_cast<uint32_t>(wanted)
                    << ",\"state\":" << static_cast<uint32_t>(status.state)
                    << ",\"submitted_sequence\":" << (progress.submitted ? progress.submitted->sequence : 0U)
                    << ",\"swapchain_generation\":"
                    << (progress.submitted ? progress.submitted->swapchain_generation : 0U) << "}\n";
                client::PlayerClientFocusTestAccess::requestFrameCapture(player_client);
            }
            if (wanted == View::Complete || server_failed.load()) {
                client::PlayerClientFocusTestAccess::stop(player_client);
            }
        };
        player_client.run(core::Address::localhost(server.port()), '@', &hooks, accepted_at);
        server_thread.request_stop();
        server_thread.join();
        preparation->cancel();
        EXPECT_FALSE(server_failed.load());
        EXPECT_TRUE(start_sent);
        if (dimensions.first == 1280) {
            EXPECT_TRUE(maximized_game_captured) << "actual maximized rich HUD frame is required";
        }
        EXPECT_EQ(wanted, View::Complete) << "all real readiness-gated views must be captured within 60 seconds";
    }
}

TEST(PlayerClientFocusTest, LoadingPresentationRendersActualStageAndReadyFrames)
{
    using Clock = std::chrono::steady_clock;

    generation::GenerationConfiguration configuration;
    configuration.seed = 88'005'555U;
    configuration.epochs = 1U;
    configuration.erosion_steps = 0U;
    configuration.initial_climate_iterations = 1U;
    configuration.climate_iterations = 1U;
    configuration.currents = false;
    configuration.coastal_elaboration = false;
    client::WorldLoadingIdentity const identity{
        .join_epoch = 1U,
        .configuration = configuration,
    };
    client::WorldLoading loading{ client::WorldLoadingPlatform::Mac };
    Clock::time_point const request_accepted_at = Clock::now();
    ASSERT_TRUE(loading.begin(identity, request_accepted_at));

    std::optional<generation::GenerationProgress> first_snapshot_progress;
    bool stage_progress_published = false;
    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
        configuration,
        {},
        [&](generation::GenerationProgress const& progress) {
            if (!first_snapshot_progress && progress.snapshot) {
                first_snapshot_progress = progress;
                stage_progress_published = loading.publishGenerationProgress(
                    identity,
                    progress,
                    Clock::now()
                );
            }
        }
    );
    ASSERT_NE(atlas, nullptr);
    ASSERT_TRUE(first_snapshot_progress.has_value());
    ASSERT_TRUE(stage_progress_published);

    client::WorldLoadingStatus const preparing_status = loading.status(Clock::now());
    ASSERT_EQ(preparing_status.state, client::WorldLoadingState::PreparingAtlas);
    ASSERT_NE(preparing_status.map_snapshot, nullptr);

    ASSERT_TRUE(loading.publishAtlas(identity, atlas, Clock::now()));
    static constexpr shared::PlayerPosition SAFE_SPAWN{ .x = 512, .y = 512, .z = 512 };
    ASSERT_TRUE(loading.publishRefinedSpawn(
        identity,
        SAFE_SPAWN,
        client::SpawnValidation::Safe,
        Clock::now()
    ));

    shared::TerrainWorld terrain{ atlas };
    std::map<std::pair<int32_t, int32_t>, shared::SurfaceTile> surface_tiles;
    std::vector<shared::ChunkCoordinate> const near_keys = client::WorldLoading::requiredChunkKeys(
        SAFE_SPAWN,
        client::WorldLoading::INITIAL_READY_SPHERE_RADIUS_BLOCKS
    );
    for (shared::ChunkCoordinate const coordinate : near_keys) {
        auto const tile_key = std::pair{ coordinate.x, coordinate.y };
        auto tile = surface_tiles.find(tile_key);
        if (tile == surface_tiles.end()) {
            shared::SurfaceTile sampled_tile = terrain.sampleSurface({
                .x = coordinate.x,
                .y = coordinate.y,
                .z = 0,
            });
            tile = surface_tiles.emplace(tile_key, std::move(sampled_tile)).first;
        }
        shared::Chunk const chunk = terrain.materializeChunk(tile->second, coordinate);
        ASSERT_TRUE(loading.publishMaterializedChunk(identity, chunk, Clock::now()));
    }

    client::WorldLoadingStatus const ready_status = loading.status(Clock::now());
    ASSERT_EQ(ready_status.state, client::WorldLoadingState::ReadyForStart);
    ASSERT_TRUE(ready_status.atlas_ready);
    ASSERT_TRUE(ready_status.spawn_safe);
    ASSERT_TRUE(ready_status.initial_sphere_ready);
    ASSERT_NE(ready_status.map_snapshot, nullptr);

    client::PlayerClient player_client{ shared::WorldMode::Flat };

    auto const textPixelCount = [](client::RendererFrameCapture const& capture,
        client::UiTextPlacement const& placement, size_t const text_length, bool const accent) {
        uint32_t count = 0U;
        uint32_t const left = static_cast<uint32_t>(std::max(0.0F, placement.x));
        uint32_t const top = static_cast<uint32_t>(std::max(0.0F, placement.y));
        uint32_t const right = std::min(capture.width,
            left + static_cast<uint32_t>(static_cast<float>(text_length) * 16.0F * placement.scale));
        uint32_t const bottom = std::min(capture.height,
            top + static_cast<uint32_t>(32.0F * placement.scale));
        for (uint32_t y = top; y < bottom; ++y) {
            for (uint32_t x = left; x < right; ++x) {
                size_t const pixel = (static_cast<size_t>(y) * capture.width + x) * 4U;
                uint8_t const red = capture.rgba8[pixel];
                uint8_t const green = capture.rgba8[pixel + 1U];
                uint8_t const blue = capture.rgba8[pixel + 2U];
                bool const readable = accent ? red > 180U && green > 120U && blue < 180U
                    : red > 170U && green > 170U && blue > 130U;
                count += readable ? 1U : 0U;
            }
        }
        return count;
    };
    auto const verifyReadableFrame = [&](client::RendererFrameCapture const& capture,
        client::WorldLoadingStatus const& status) {
        ASSERT_GT(capture.width, 0U);
        ASSERT_GT(capture.height, 0U);
        uint64_t const capture_width = capture.width;
        uint64_t const capture_height = capture.height;
        ASSERT_LE(capture_width, std::numeric_limits<uint64_t>::max() / capture_height / 4U);
        EXPECT_EQ(capture.rgba8.size(), capture_width * capture_height * 4U);
        auto const frame = client::buildUiFrame(1U, status, false, true);
        auto const layout = client::layoutUiLoading(frame, capture.width, capture.height,
            loadingContentScale(player_client));
        EXPECT_GT(textPixelCount(capture, layout.title, frame.title.size(), false), 30U)
            << "accepted cream loading title must be readable in its public layout region";
    };
    auto const verifyReadyPrompt = [&](client::RendererFrameCapture const& capture) {
        verifyReadableFrame(capture, ready_status);
        auto const frame = client::buildUiFrame(1U, ready_status, false, true);
        ASSERT_TRUE(frame.show_start_prompt);
        auto const layout = client::layoutUiLoading(frame, capture.width, capture.height,
            loadingContentScale(player_client));
        EXPECT_GT(textPixelCount(capture, layout.action, std::string_view{"Press any key to start"}.size(), true), 30U)
            << "accepted gold start action must be readable in its public layout region";
    };
    auto const saveOptInCapture = [](client::RendererFrameCapture const& capture, std::string_view const label) {
        char const* const prefix = std::getenv("MC_LOADING_PRESENTATION_CAPTURE_PREFIX");
        if (prefix == nullptr || prefix[0] == '\0') {
            return std::optional<std::string>{};
        }
        std::filesystem::path const output_path = std::string{ prefix } + "-" + std::string{ label } + ".ppm";
        auto const written = testsupport::writePpm(
            {
                .width = capture.width,
                .height = capture.height,
                .srgb_encoded = capture.srgb_encoded,
                .pixels = capture.rgba8,
            },
            output_path,
            "MC-AI-0496 production loading presentation capture"
        );
        if (!written.has_value()) {
            return std::optional<std::string>{ written.error() };
        }
        return std::optional<std::string>{};
    };

    std::optional<client::RendererFrameCapture> const preparing_capture =
        client::PlayerClientFocusTestAccess::captureLoadingPresentation(player_client, preparing_status);
    ASSERT_TRUE(preparing_capture.has_value());
    verifyReadableFrame(*preparing_capture, preparing_status);
    if (std::optional<std::string> const error = saveOptInCapture(*preparing_capture, "preparing");
        error.has_value()) {
        FAIL() << *error;
    }

    std::optional<client::RendererFrameCapture> const ready_capture =
        client::PlayerClientFocusTestAccess::captureLoadingPresentation(player_client, ready_status);
    ASSERT_TRUE(ready_capture.has_value());
    if (std::optional<std::string> const error = saveOptInCapture(*ready_capture, "ready");
        error.has_value()) {
        FAIL() << *error;
    }
    verifyReadyPrompt(*ready_capture);
    EXPECT_EQ(client::PlayerClientFocusTestAccess::validationErrorCount(player_client), 0U)
        << "validation enabled=" << client::PlayerClientFocusTestAccess::runtimeInfo(player_client).validation_enabled;
}

TEST(PlayerClientFocusTest, InitialLoadingSuppressesFallbackGridBeforeMapSnapshot)
{
    static constexpr std::array<float, 5U> SAMPLE_X{{ 0.72F, 0.78F, 0.84F, 0.90F, 0.96F }};
    static constexpr std::array<float, 6U> SAMPLE_Y{{ 0.18F, 0.28F, 0.38F, 0.48F, 0.58F, 0.68F }};
    static constexpr int CHANNEL_TOLERANCE = 3;

    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginInitialLoading(player_client));
    client::WorldLoadingStatus const status = player_client.worldLoadingStatus();
    ASSERT_EQ(status.state, client::WorldLoadingState::Creating);
    ASSERT_EQ(status.map_snapshot, nullptr);

    std::optional<client::RendererFrameCapture> const capture =
        client::PlayerClientFocusTestAccess::captureLoadingPresentation(player_client, status);
    ASSERT_TRUE(capture.has_value());
    client::GenerationMapImageHandle const backdrop = client::loadingBackdropImage();
    ASSERT_NE(backdrop, nullptr);
    ASSERT_EQ(backdrop->rgba.size(), static_cast<size_t>(backdrop->width) * backdrop->height);
    ASSERT_EQ(capture->rgba8.size(), static_cast<size_t>(capture->width) * capture->height * 4U);

    float const scale = std::max(
        static_cast<float>(capture->width) / static_cast<float>(backdrop->width),
        static_cast<float>(capture->height) / static_cast<float>(backdrop->height)
    );
    float const crop_width = static_cast<float>(capture->width)
        / (static_cast<float>(backdrop->width) * scale);
    float const crop_height = static_cast<float>(capture->height)
        / (static_cast<float>(backdrop->height) * scale);
    float const crop_left = (1.0F - crop_width) * 0.5F;
    float const crop_top = (1.0F - crop_height) * 0.5F;
    auto const matchesSharedBackdrop = [&](bool const vertically_flipped) {
        uint32_t matching_samples = 0U;
        uint32_t sample_count = 0U;
        for (float const relative_y : SAMPLE_Y) {
            uint32_t const y = std::min(
                static_cast<uint32_t>(relative_y * static_cast<float>(capture->height)),
                capture->height - 1U
            );
            for (float const relative_x : SAMPLE_X) {
                uint32_t const x = std::min(
                    static_cast<uint32_t>(relative_x * static_cast<float>(capture->width)),
                    capture->width - 1U
                );
                float const source_x = crop_left
                    + (static_cast<float>(x) + 0.5F) / static_cast<float>(capture->width) * crop_width;
                float screen_y = (static_cast<float>(y) + 0.5F) / static_cast<float>(capture->height);
                if (vertically_flipped) {
                    screen_y = 1.0F - screen_y;
                }
                float const source_y = crop_top + screen_y * crop_height;
                uint32_t const backdrop_x = std::min(
                    static_cast<uint32_t>(source_x * static_cast<float>(backdrop->width)),
                    backdrop->width - 1U
                );
                uint32_t const backdrop_y = std::min(
                    static_cast<uint32_t>(source_y * static_cast<float>(backdrop->height)),
                    backdrop->height - 1U
                );
                client::GenerationMapRgba8 const& expected = backdrop->rgba[
                    static_cast<size_t>(backdrop_y) * backdrop->width + backdrop_x
                ];
                uint64_t const actual_pixel = (static_cast<uint64_t>(y) * capture->width + x) * 4U;
                bool matches = true;
                for (size_t channel = 0U; channel < 3U; ++channel) {
                    matches = matches
                        && std::abs(static_cast<int>(capture->rgba8[actual_pixel + channel])
                            - static_cast<int>(expected[channel])) <= CHANNEL_TOLERANCE;
                }
                matching_samples += matches ? 1U : 0U;
                ++sample_count;
            }
        }
        return std::pair{ matching_samples, sample_count };
    };

    auto const direct_matches = matchesSharedBackdrop(false);
    auto const flipped_matches = matchesSharedBackdrop(true);
    EXPECT_GE(std::max(direct_matches.first, flipped_matches.first), direct_matches.second - 2U)
        << "the initial loading frame should display the shared backdrop without the fallback 3D grid";
}

#if defined(__APPLE__)
TEST(PlayerClientFocusTest, CopiedFrameHasNoToastAndSuccessShowsCenteredToast)
{
    static constexpr uint8_t MINIMUM_GREEN_CHANNEL = 175U;
    static constexpr uint8_t MINIMUM_GREEN_MARGIN = 12U;
    static constexpr uint32_t MINIMUM_TOAST_PIXELS = 80U;

    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client));
    client::PlayerClientFocusTestAccess::disableDebugHud(player_client);
    std::optional<client::RendererFrameCapture> const copied_frame =
        client::PlayerClientFocusTestAccess::capturePlayerFrame(player_client);
    ASSERT_TRUE(copied_frame.has_value());
    struct ToastPixels final {
        uint32_t count = 0U;
        uint64_t x_sum = 0U;
        uint64_t y_sum = 0U;
    };
    auto const findGreenToastPixels = [](client::RendererFrameCapture const& capture) {
        uint32_t const margin = std::min(32U, capture.width / 4U);
        uint32_t const roi_left = margin > 4U ? margin - 4U : 0U;
        uint32_t const roi_right = capture.width - roi_left;
        uint32_t const toast_y = std::max(24U, capture.height * 12U / 100U);
        uint32_t const roi_top = toast_y > 4U ? toast_y - 4U : 0U;
        uint32_t const roi_bottom = std::min(capture.height, toast_y + 56U);
        ToastPixels pixels;
        for (uint32_t y = roi_top; y < roi_bottom; ++y) {
            for (uint32_t x = roi_left; x < roi_right; ++x) {
                uint64_t const pixel = (static_cast<uint64_t>(y) * capture.width + x) * 4U;
                uint8_t const red = capture.rgba8[pixel];
                uint8_t const green = capture.rgba8[pixel + 1U];
                uint8_t const blue = capture.rgba8[pixel + 2U];
                if (green >= MINIMUM_GREEN_CHANNEL
                    && red >= MINIMUM_GREEN_CHANNEL
                    && blue >= MINIMUM_GREEN_CHANNEL
                    && green >= red + MINIMUM_GREEN_MARGIN
                    && green >= blue + MINIMUM_GREEN_MARGIN) {
                    ++pixels.count;
                    pixels.x_sum += x;
                    pixels.y_sum += y;
                }
            }
        }
        return pixels;
    };
    EXPECT_LT(findGreenToastPixels(*copied_frame).count, MINIMUM_TOAST_PIXELS)
        << "the screenshot frame must be captured before its success notification";

    std::string const pasteboard_name = "MinecraftClone.ScreenshotToastTest."
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string error;
    std::optional<std::vector<uint8_t>> const png = client::detail::encodeRgba8Png(
        copied_frame->width,
        copied_frame->height,
        copied_frame->rgba8,
        error
    );
    ASSERT_TRUE(png.has_value()) << error;
    ASSERT_TRUE(client::detail::copyScreenshotPngToClipboard(*png, error, pasteboard_name)) << error;
    std::optional<std::vector<uint8_t>> const pasted_png =
        client::detail::takeScreenshotPngFromPasteboard(pasteboard_name, error);
    ASSERT_TRUE(pasted_png.has_value()) << error;
    EXPECT_EQ(*pasted_png, *png);

    client::PlayerClientFocusTestAccess::setScreenshotFeedback(
        player_client,
        "Screenshot copied to clipboard",
        true
    );
    std::optional<client::RendererFrameCapture> const toast_frame =
        client::PlayerClientFocusTestAccess::capturePlayerFrame(player_client);
    ASSERT_TRUE(toast_frame.has_value());

    ToastPixels const toast_pixels = findGreenToastPixels(*toast_frame);
    ASSERT_GT(toast_pixels.count, MINIMUM_TOAST_PIXELS)
        << "a successful clipboard copy should show a readable toast near the top center";
    double const toast_center_x = static_cast<double>(toast_pixels.x_sum) / toast_pixels.count;
    double const toast_center_y = static_cast<double>(toast_pixels.y_sum) / toast_pixels.count;
    EXPECT_NEAR(toast_center_x, static_cast<double>(toast_frame->width) / 2.0, toast_frame->width * 0.1);
    EXPECT_LT(toast_center_y, static_cast<double>(toast_frame->height) * 0.25);
}
#endif

TEST(PlayerClientFocusTest, TerrainPromptRequiresNonemptyActiveSceneThenMatchingFence)
{
    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client, false));
    ASSERT_EQ(player_client.worldLoadingStatus().state, client::WorldLoadingState::ReadyForStart);
    for (uint32_t frame = 0U; frame < 6U; ++frame) {
        client::PlayerClientFocusTestAccess::renderFrame(player_client);
    }
    EXPECT_FALSE(player_client.worldLoadingStatus().ready_prompt_frame_rendered_at.has_value());
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::promptFrame(player_client).has_value());

    ASSERT_TRUE(client::PlayerClientFocusTestAccess::uploadPromptLod(player_client, 0U));
    for (uint32_t frame = 0U; frame < 6U; ++frame) {
        client::PlayerClientFocusTestAccess::renderFrame(player_client);
    }
    EXPECT_FALSE(player_client.worldLoadingStatus().ready_prompt_frame_rendered_at.has_value());
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::promptFrame(player_client).has_value());

    ASSERT_TRUE(client::PlayerClientFocusTestAccess::uploadPromptLod(player_client, 1U));
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::promptFrame(player_client).has_value());
    EXPECT_FALSE(player_client.worldLoadingStatus().ready_prompt_frame_rendered_at.has_value());
    auto const old_prompt = *client::PlayerClientFocusTestAccess::promptFrame(player_client);
    client::PlayerClientFocusTestAccess::recreatePromptSwapchain(player_client);
    client::PlayerClientFocusTestAccess::renderFrame(player_client);
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::promptFrame(player_client).has_value());
    EXPECT_NE(client::PlayerClientFocusTestAccess::promptFrame(player_client)->swapchain_generation,
        old_prompt.swapchain_generation);
    EXPECT_FALSE(player_client.worldLoadingStatus().ready_prompt_frame_rendered_at.has_value());
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 5 };
    while (!player_client.worldLoadingStatus().ready_prompt_frame_rendered_at
        && std::chrono::steady_clock::now() < deadline) {
        client::PlayerClientFocusTestAccess::renderFrame(player_client);
    }
    EXPECT_TRUE(player_client.worldLoadingStatus().ready_prompt_frame_rendered_at.has_value());
    client::PlayerClientFocusTestAccess::resetLoadingConnection(player_client);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::promptFrame(player_client).has_value());
    EXPECT_FALSE(player_client.worldLoadingStatus().ready_prompt_frame_rendered_at.has_value());
}


TEST(PlayerClientFocusTest, FocusAndMapTransitionsClearBlockEffectsAndPendingReceipts)
{
    using Access = client::PlayerClientFocusTestAccess;
    client::PlayerClient player_client;
    core::platform::glfw::GlfwWindow focus_sink{
        core::platform::glfw::WindowDescriptor{
            .width = 64U, .height = 64U, .title = "Block effects focus sink",
        },
    };
    ASSERT_TRUE(Access::beginPlayingWorld(player_client));
    auto* window = Access::nativeWindow(player_client);
    glfwFocusWindow(window);
    ASSERT_TRUE(waitForFocusState(player_client, true));
    ASSERT_TRUE(Access::seedBlockEffects(player_client));
    ASSERT_FALSE(Access::acceptSeededParticleReceipt(player_client));
    glfwFocusWindow(focus_sink.nativeHandle());
    ASSERT_TRUE(waitForFocusState(player_client, false));
    EXPECT_FALSE(Access::breakInputActive(player_client));
    EXPECT_FALSE(Access::hasStoredBlockTarget(player_client));
    EXPECT_FALSE(Access::hasPendingBreakReceipt(player_client));
    EXPECT_TRUE(Access::acceptSeededParticleReceipt(player_client));
    Access::updateBlockEffects(player_client);
    EXPECT_FALSE(Access::hasStoredBlockTarget(player_client));
    EXPECT_TRUE(Access::acceptSeededParticleReceipt(player_client));

    glfwFocusWindow(window);
    ASSERT_TRUE(waitForFocusState(player_client, true));
    ASSERT_TRUE(Access::seedBlockEffects(player_client));
    ASSERT_FALSE(Access::acceptSeededParticleReceipt(player_client));
    // Map opening must clear effects before the next presentation update.
    Access::setWorldMapOpen(player_client, true);
    EXPECT_FALSE(Access::breakInputActive(player_client));
    EXPECT_FALSE(Access::hasStoredBlockTarget(player_client));
    EXPECT_FALSE(Access::hasPendingBreakReceipt(player_client));
    EXPECT_TRUE(Access::acceptSeededParticleReceipt(player_client));
    Access::updateBlockEffects(player_client);
    EXPECT_TRUE(Access::acceptSeededParticleReceipt(player_client));
    Access::setWorldMapOpen(player_client, false);
    EXPECT_FALSE(Access::breakInputActive(player_client));
}
