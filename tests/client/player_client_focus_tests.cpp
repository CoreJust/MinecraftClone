#include <client/PlayerClient.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/WorldLoading.hpp>
#include <client/detail/ScreenshotPng.hpp>

#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace client {

struct PlayerClientFocusTestAccess final {
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

    static bool updateScreenshotCaptureKey(PlayerClient& player_client, bool const pressed) noexcept
    {
        return player_client.m_screenshot_capture_key_latch.update(pressed);
    }

    static void captureStartTransitionInputs(PlayerClient& player_client)
    {
        player_client.captureStartTransitionInputs();
    }

    static bool hasStartTransitionInputs(PlayerClient const& player_client) noexcept
    {
        return player_client.hasStartTransitionInputs();
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

TEST(PlayerClientFocusTest, NormalPlayingFramesDoNotCollectCompletionDiagnostics)
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
    ASSERT_TRUE(presented);
    EXPECT_GT(runtime.submitted_frame_count, 0U);
    EXPECT_FALSE(runtime.diagnostic_presentation_acquire.captured);
    EXPECT_FALSE(client::PlayerClientFocusTestAccess::frameProgress(player_client).completed.has_value());
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

    auto const verifyReadableFrame = [](client::RendererFrameCapture const& capture) {
        ASSERT_GT(capture.width, 0U);
        ASSERT_GT(capture.height, 0U);
        uint64_t const capture_width = capture.width;
        uint64_t const capture_height = capture.height;
        ASSERT_LE(capture_width, std::numeric_limits<uint64_t>::max() / capture_height / 4U);
        uint64_t const expected_byte_count = capture_width * capture_height * 4U;
        EXPECT_EQ(capture.rgba8.size(), expected_byte_count);

        uint32_t accent_pixel_count = 0U;
        uint32_t const roi_width = std::min(capture.width, 1'000U);
        uint32_t const roi_height = std::min(capture.height, 120U);
        for (uint32_t y = 0U; y < roi_height; ++y) {
            for (uint32_t x = 0U; x < roi_width; ++x) {
                uint64_t const pixel = (static_cast<uint64_t>(y) * capture.width + x) * 4U;
                uint8_t const red = capture.rgba8[pixel];
                uint8_t const green = capture.rgba8[pixel + 1U];
                uint8_t const blue = capture.rgba8[pixel + 2U];
                if (red > 190U && green > 125U && blue < 180U
                    && static_cast<uint16_t>(red) > static_cast<uint16_t>(green) + 20U
                    && static_cast<uint16_t>(green) > static_cast<uint16_t>(blue) + 25U) {
                    ++accent_pixel_count;
                }
            }
        }
        EXPECT_GT(accent_pixel_count, 30U)
            << "the loading title should leave visible high-contrast pixels in the title region";
    };
    auto const verifyCenteredReadyPrompt = [](client::RendererFrameCapture const& capture) {
        uint32_t bright_pixel_count = 0U;
        uint64_t pixel_x_sum = 0U;
        uint64_t pixel_y_sum = 0U;
        uint32_t title_pixel_count = 0U;
        uint64_t title_pixel_x_sum = 0U;
        uint64_t title_pixel_y_sum = 0U;
        uint32_t min_x = capture.width;
        uint32_t max_x = 0U;
        uint32_t min_y = capture.height;
        uint32_t max_y = 0U;
        uint32_t const left = capture.width / 6U;
        uint32_t const right = capture.width - left;
        uint32_t const top = capture.height / 4U;
        uint32_t const bottom = capture.height - top;
        for (uint32_t y = top; y < bottom; ++y) {
            for (uint32_t x = left; x < right; ++x) {
                uint64_t const pixel = (static_cast<uint64_t>(y) * capture.width + x) * 4U;
                uint8_t const red = capture.rgba8[pixel];
                uint8_t const green = capture.rgba8[pixel + 1U];
                uint8_t const blue = capture.rgba8[pixel + 2U];
                if (red > 190U && green > 125U && blue < 180U
                    && static_cast<uint16_t>(red) > static_cast<uint16_t>(green) + 20U
                    && static_cast<uint16_t>(green) > static_cast<uint16_t>(blue) + 25U) {
                    ++title_pixel_count;
                    title_pixel_x_sum += x;
                    title_pixel_y_sum += y;
                }
                if (red < 230U || green < 230U || blue < 230U) {
                    continue;
                }
                ++bright_pixel_count;
                pixel_x_sum += x;
                pixel_y_sum += y;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
        ASSERT_GT(title_pixel_count, 30U)
            << "the ready heading should use the same visible gold accent as the loading title";
        ASSERT_GT(bright_pixel_count, 300U)
            << "the ready prompt should use a bright, high-contrast face near the center";
        double const title_center_x = static_cast<double>(title_pixel_x_sum) / title_pixel_count;
        double const title_center_y = static_cast<double>(title_pixel_y_sum) / title_pixel_count;
        EXPECT_NEAR(title_center_x, static_cast<double>(capture.width) / 2.0, capture.width * 0.1);
        EXPECT_NEAR(title_center_y, static_cast<double>(capture.height) / 2.0, capture.height * 0.12);
        double const center_x = static_cast<double>(pixel_x_sum) / bright_pixel_count;
        double const center_y = static_cast<double>(pixel_y_sum) / bright_pixel_count;
        EXPECT_NEAR(center_x, static_cast<double>(capture.width) / 2.0, capture.width * 0.1)
            << "the ready prompt should be horizontally centered; bounds=" << min_x << ".." << max_x;
        EXPECT_NEAR(center_y, static_cast<double>(capture.height) / 2.0, capture.height * 0.12)
            << "the ready prompt should be vertically centered; bounds=" << min_y << ".." << max_y;
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
    verifyReadableFrame(*preparing_capture);
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
    verifyCenteredReadyPrompt(*ready_capture);
    EXPECT_EQ(client::PlayerClientFocusTestAccess::validationErrorCount(player_client), 0U)
        << "validation enabled=" << client::PlayerClientFocusTestAccess::runtimeInfo(player_client).validation_enabled;
}

TEST(PlayerClientFocusTest, InitialLoadingSuppressesFallbackGridBeforeMapSnapshot)
{
    static constexpr uint32_t ROI_LEFT_PERCENT = 60U;
    static constexpr uint32_t ROI_TOP_PERCENT = 50U;
    static constexpr uint8_t MAXIMUM_BACKDROP_CHANNEL = 175U;

    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginInitialLoading(player_client));
    client::WorldLoadingStatus const status = player_client.worldLoadingStatus();
    ASSERT_EQ(status.state, client::WorldLoadingState::Creating);
    ASSERT_EQ(status.map_snapshot, nullptr);

    std::optional<client::RendererFrameCapture> const capture =
        client::PlayerClientFocusTestAccess::captureLoadingPresentation(player_client, status);
    ASSERT_TRUE(capture.has_value());

    uint32_t const roi_left = capture->width * ROI_LEFT_PERCENT / 100U;
    uint32_t const roi_top = capture->height * ROI_TOP_PERCENT / 100U;
    uint32_t dark_scene_pixels = 0U;
    for (uint32_t y = roi_top; y < capture->height; ++y) {
        for (uint32_t x = roi_left; x < capture->width; ++x) {
            uint64_t const pixel = (static_cast<uint64_t>(y) * capture->width + x) * 4U;
            if (capture->rgba8[pixel] < MAXIMUM_BACKDROP_CHANNEL
                && capture->rgba8[pixel + 1U] < MAXIMUM_BACKDROP_CHANNEL
                && capture->rgba8[pixel + 2U] < MAXIMUM_BACKDROP_CHANNEL) {
                ++dark_scene_pixels;
            }
        }
    }
    EXPECT_EQ(dark_scene_pixels, 0U)
        << "the initial loading backdrop should not expose the fallback 3D grid or platform";
}

#if defined(__APPLE__)
TEST(PlayerClientFocusTest, CopiedFrameHasNoToastAndSuccessShowsCenteredToast)
{
    static constexpr uint32_t ROI_LEFT_PERCENT = 20U;
    static constexpr uint32_t ROI_RIGHT_PERCENT = 80U;
    static constexpr uint32_t ROI_BOTTOM_PERCENT = 35U;
    static constexpr uint8_t MINIMUM_BRIGHT_CHANNEL = 205U;
    static constexpr uint32_t MINIMUM_TOAST_PIXELS = 80U;

    client::PlayerClient player_client;
    ASSERT_TRUE(client::PlayerClientFocusTestAccess::beginPlayingWorld(player_client));
    client::PlayerClientFocusTestAccess::disableDebugHud(player_client);
    std::optional<client::RendererFrameCapture> const copied_frame =
        client::PlayerClientFocusTestAccess::capturePlayerFrame(player_client);
    ASSERT_TRUE(copied_frame.has_value());
    auto const countBrightToastPixels = [](client::RendererFrameCapture const& capture) {
        uint32_t const roi_left = capture.width * ROI_LEFT_PERCENT / 100U;
        uint32_t const roi_right = capture.width * ROI_RIGHT_PERCENT / 100U;
        uint32_t const roi_bottom = capture.height * ROI_BOTTOM_PERCENT / 100U;
        uint32_t count = 0U;
        for (uint32_t y = 0U; y < roi_bottom; ++y) {
            for (uint32_t x = roi_left; x < roi_right; ++x) {
                uint64_t const pixel = (static_cast<uint64_t>(y) * capture.width + x) * 4U;
                if (capture.rgba8[pixel] >= MINIMUM_BRIGHT_CHANNEL
                    && capture.rgba8[pixel + 1U] >= MINIMUM_BRIGHT_CHANNEL
                    && capture.rgba8[pixel + 2U] >= MINIMUM_BRIGHT_CHANNEL) {
                    ++count;
                }
            }
        }
        return count;
    };
    EXPECT_LT(countBrightToastPixels(*copied_frame), MINIMUM_TOAST_PIXELS)
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

    uint32_t const roi_left = toast_frame->width * ROI_LEFT_PERCENT / 100U;
    uint32_t const roi_right = toast_frame->width * ROI_RIGHT_PERCENT / 100U;
    uint32_t const roi_bottom = toast_frame->height * ROI_BOTTOM_PERCENT / 100U;
    uint32_t toast_pixels = 0U;
    uint64_t pixel_x_sum = 0U;
    uint64_t pixel_y_sum = 0U;
    for (uint32_t y = 0U; y < roi_bottom; ++y) {
        for (uint32_t x = roi_left; x < roi_right; ++x) {
            uint64_t const pixel = (static_cast<uint64_t>(y) * toast_frame->width + x) * 4U;
            uint8_t const red = toast_frame->rgba8[pixel];
            uint8_t const green = toast_frame->rgba8[pixel + 1U];
            uint8_t const blue = toast_frame->rgba8[pixel + 2U];
            if (red >= MINIMUM_BRIGHT_CHANNEL
                && green >= MINIMUM_BRIGHT_CHANNEL
                && blue >= MINIMUM_BRIGHT_CHANNEL) {
                ++toast_pixels;
                pixel_x_sum += x;
                pixel_y_sum += y;
            }
        }
    }
    EXPECT_EQ(toast_pixels, countBrightToastPixels(*toast_frame));
    ASSERT_GT(toast_pixels, MINIMUM_TOAST_PIXELS)
        << "a successful clipboard copy should show a readable toast near the top center";
    double const toast_center_x = static_cast<double>(pixel_x_sum) / toast_pixels;
    double const toast_center_y = static_cast<double>(pixel_y_sum) / toast_pixels;
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
