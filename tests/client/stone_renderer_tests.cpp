#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <core/graphics/vulkan/Vulkan.hpp>
#include <core/platform/glfw/GlfwWindow.hpp>

#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#ifndef MC_STONE_RENDERER_FAIL_UNSUPPORTED
#define MC_STONE_RENDERER_FAIL_UNSUPPORTED 0
#endif

namespace {

constexpr uint32_t WIDTH = 640U;
constexpr uint32_t HEIGHT = 480U;
constexpr auto RENDER_TIMEOUT = std::chrono::seconds{ 10 };

class ScopedStoneSetting final {
public:
    ScopedStoneSetting(char const* const variable, char const* const value)
        : m_variable(variable)
    {
        if (char const* const previous = std::getenv(m_variable)) {
            m_previous = previous;
        }
        if (assign(value) != 0) {
            throw std::runtime_error("cannot select the diagnostic stone quad mode");
        }
    }

    ~ScopedStoneSetting() { static_cast<void>(assign(m_previous ? m_previous->c_str() : nullptr)); }
    ScopedStoneSetting(ScopedStoneSetting const&) = delete;
    ScopedStoneSetting& operator=(ScopedStoneSetting const&) = delete;

private:
    [[nodiscard]]
    int assign(char const* const value) const
    {
#if defined(_WIN32)
        return _putenv_s(m_variable, value == nullptr ? "" : value);
#else
        return value == nullptr ? unsetenv(m_variable) : setenv(m_variable, value, 1);
#endif
    }

    char const* const m_variable;
    std::optional<std::string> m_previous;
};

constexpr client::CameraPose ABOVE_CAMERA{
    .position = { 8.0, -20.0, 18.0 },
    .angles = { .pitch_degrees = -25.0 },
};
constexpr client::CameraPose BELOW_CAMERA{
    .position = { 8.0, -20.0, -12.0 },
    .angles = { .pitch_degrees = 25.0 },
};

[[nodiscard]]
shared::ChunkMesh deterministicChunk()
{
    shared::Chunk const chunk = shared::Chunk::makeStoneFixture({}, 42U);
    shared::ChunkMesher mesher;
    return mesher.update(chunk);
}

[[nodiscard]]
shared::ChunkMesh emptyChunkMesh()
{
    shared::ChunkMesher mesher;
    shared::Chunk const chunk;
    return mesher.update(chunk);
}

[[nodiscard]]
shared::ChunkMesh singleStoneMesh()
{
    shared::Chunk chunk;
    EXPECT_TRUE(chunk.setBlock({ .x = 8U, .y = 8U, .z = 4U }, shared::Block::Stone));
    shared::ChunkMesher mesher;
    return mesher.update(chunk);
}

[[nodiscard]]
uint64_t stonePixelCount(client::RendererFrameCapture const& capture)
{
    uint64_t count = 0U;
    for (uint64_t offset = 0U; offset < capture.rgba8.size(); offset += 4U) {
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        uint8_t const red_green_delta = red > green ? red - green : green - red;
        uint8_t const green_blue_delta = green > blue ? green - blue : blue - green;
        if (red >= 50U && red <= 160U && red_green_delta <= 2U && green_blue_delta <= 2U) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]]
uint64_t skyPixelCount(client::RendererFrameCapture const& capture)
{
    uint64_t count = 0U;
    for (uint64_t offset = 0U; offset < capture.rgba8.size(); offset += 4U) {
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        if (blue > green + 20U && green > red + 20U) {
            ++count;
        }
    }
    return count;
}

void expectVisibleTerrain(client::RendererFrameCapture const& capture)
{
    ASSERT_EQ(capture.width, WIDTH);
    ASSERT_EQ(capture.height, HEIGHT);
    ASSERT_FALSE(capture.srgb_encoded);
    ASSERT_EQ(capture.rgba8.size(), static_cast<uint64_t>(WIDTH) * HEIGHT * 4U);
    EXPECT_GT(stonePixelCount(capture), 100U)
        << "offscreen capture contains no visible textured stone terrain";
    EXPECT_GT(skyPixelCount(capture), 100U)
        << "offscreen capture contains no sky around the terrain";
}

[[nodiscard]]
client::RendererFrameCapture render(
    client::VulkanOffscreenRenderer& renderer
)
{
    return renderer.render(
        std::span<client::PlayerRenderData const>{},
        std::chrono::steady_clock::now() + RENDER_TIMEOUT
    );
}

void writeOptionalCapture(client::RendererFrameCapture const& capture)
{
    char const* const capture_path = std::getenv("MC_STONE_RENDERER_CAPTURE_PATH");
    if (capture_path == nullptr || capture_path[0] == '\0') {
        return;
    }
    std::expected<std::monostate, std::string> const written = testsupport::writePpm(
        {
            .width = capture.width,
            .height = capture.height,
            .srgb_encoded = capture.srgb_encoded,
            .pixels = capture.rgba8,
        },
        std::filesystem::path{ capture_path },
        "MC-AI-0158 deterministic S5 stone offscreen capture"
    );
    ASSERT_TRUE(written.has_value()) << written.error();
}

class StoneRendererAcceptanceTest : public testing::Test {
protected:
    void SetUp() override
    {
        try {
            m_renderer.emplace(m_shader_assets, true);
            ASSERT_TRUE(m_renderer->validationEnabled());
        } catch (core::graphics::vulkan::VulkanError const& error) {
#if MC_STONE_RENDERER_FAIL_UNSUPPORTED
            FAIL() << "true offscreen renderer is unavailable: " << error.what();
#else
            GTEST_SKIP() << "true offscreen renderer is unavailable: " << error.what();
#endif
        } catch (std::exception const& error) {
            FAIL() << "true offscreen renderer failed: " << error.what();
        }
    }

    client::InstalledShaderAssets m_shader_assets;
    std::optional<client::VulkanOffscreenRenderer> m_renderer;
};

TEST_F(StoneRendererAcceptanceTest, DeterministicChunkRendersFromAboveAndBelow)
{
    shared::ChunkMesh const mesh = deterministicChunk();
    ASSERT_FALSE(mesh.faces.empty());
    m_renderer->setChunkMesh(mesh);

    m_renderer->setCamera(ABOVE_CAMERA);
    client::RendererFrameCapture const above = render(*m_renderer);
    expectVisibleTerrain(above);
    writeOptionalCapture(above);

    m_renderer->setCamera(BELOW_CAMERA);
    client::RendererFrameCapture const below = render(*m_renderer);
    expectVisibleTerrain(below);
    EXPECT_EQ(m_renderer->validationErrorCount(), 0U);
}

TEST_F(StoneRendererAcceptanceTest, IndexedAndSixVertexQuadsHaveByteIdenticalOffscreenOutput)
{
    std::array const cameras{
        ABOVE_CAMERA,
        BELOW_CAMERA,
        client::CameraPose{ .position = { -12.0, 8.0, 8.0 }, .angles = { .yaw_degrees = 90.0 } },
        client::CameraPose{ .position = { 28.0, 8.0, 8.0 }, .angles = { .yaw_degrees = -90.0 } },
        client::CameraPose{ .position = { 8.0, 28.0, 8.0 }, .angles = { .yaw_degrees = 180.0 } },
    };
    shared::ChunkMesh const mesh = deterministicChunk();
    auto const capture_mode = [&](char const* const mode) {
        ScopedStoneSetting const selected{ "MC_DIAGNOSTIC_INDEXED_STONE_QUADS", mode };
        client::VulkanOffscreenRenderer renderer{ m_shader_assets, true };
        renderer.setChunkMesh(mesh);
        std::vector<client::RendererFrameCapture> captures;
        for (client::CameraPose const camera : cameras) {
            renderer.setCamera(camera);
            captures.push_back(render(renderer));
        }
        EXPECT_EQ(renderer.validationErrorCount(), 0U);
        return captures;
    };
    auto const six_vertex = capture_mode("0");
    auto const indexed = capture_mode("1");
    ASSERT_EQ(six_vertex.size(), indexed.size());
    for (size_t view = 0U; view < indexed.size(); ++view) {
        EXPECT_GT(stonePixelCount(indexed[view]), 0U) << view;
        EXPECT_EQ(indexed[view].rgba8, six_vertex[view].rgba8) << view;
    }
}

TEST_F(StoneRendererAcceptanceTest, DirectAndIndirectSubmissionPreserveIndexedAndSixVertexOutput)
{
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = WIDTH,
            .height = HEIGHT,
            .title = "MinecraftClone stone submission parity",
        },
    };
    auto const context = client::VulkanRenderer::createPresentationContext(
        window, { .require_validation = true, .enable_frame_capture = true }
    );
    if (!context->info().multi_draw_indirect_enabled || !context->info().draw_indirect_first_instance_enabled) {
        GTEST_SKIP() << "presentation device does not enable optional indirect stone draws";
    }
    std::array<shared::HeightTileSurfaceMesh, 3> meshes;
    for (uint32_t tile = 0U; tile < meshes.size(); ++tile) {
        meshes[tile].coordinate = { .x = static_cast<int32_t>(tile), .y = 0 };
        meshes[tile].quads = { {
            .x = static_cast<int32_t>(tile * 16U),
            .z = static_cast<int32_t>(tile * 2U),
            .u_extent = 16U,
            .v_extent = 16U,
        } };
    }
    // Keep the intervening arena range live but behind the camera, so visible commands cannot merge.
    meshes[1].quads[0].y = -100;
    auto const capture_mode = [&](char const* const quad_mode, char const* const direct_mode) {
        ScopedStoneSetting const quads{ "MC_DIAGNOSTIC_INDEXED_STONE_QUADS", quad_mode };
        ScopedStoneSetting const direct{ "MC_DIAGNOSTIC_DIRECT_STONE_DRAWS", direct_mode };
        client::VulkanRenderer renderer{
            context, m_shader_assets, { .require_validation = true, .enable_frame_capture = true }
        };
        renderer.setDebugHudEnabled(false);
        renderer.setCamera({ .position = { 24.0, -40.0, 25.0 }, .angles = { .pitch_degrees = -25.0 } });
        auto const deadline = std::chrono::steady_clock::now() + RENDER_TIMEOUT;
        EXPECT_TRUE(renderer.upsertHeightTileMeshes(meshes, deadline));
        renderer.requestFrameCapture();
        for (uint32_t frame = 0U; frame < 32U && std::chrono::steady_clock::now() < deadline; ++frame) {
            if (!window.nextFrame()) {
                break;
            }
            static_cast<void>(renderer.render(std::span<client::PlayerRenderData const>{}, deadline));
            if (renderer.captureState() == client::FrameCaptureState::Completed) {
                break;
            }
        }
        EXPECT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
        client::RendererRuntimeInfo const info = renderer.runtimeInfo();
        EXPECT_EQ(info.height_tile_mesh_count, 3U);
        EXPECT_EQ(info.diagnostic_submitted_stone_quad_count, 2U);
        EXPECT_EQ(info.diagnostic_stone_draw_count, 2U);
        EXPECT_EQ(info.diagnostic_indexed_stone_quads, quad_mode[0] == '1');
        EXPECT_EQ(info.diagnostic_stone_indirect, direct_mode[0] != '1');
        RecordProperty("terrain_timestamp_reason", std::string{ info.diagnostic_gpu_timestamp_reason });
        EXPECT_TRUE(info.diagnostic_gpu_timestamps_enabled || !info.diagnostic_gpu_terrain_duration.has_value());
        EXPECT_EQ(renderer.validationErrorCount(), 0U);
        return renderer.takeFrameCapture();
    };
    auto const six_direct = capture_mode("0", "1");
    auto const six_indirect = capture_mode("0", "0");
    auto const indexed_direct = capture_mode("1", "1");
    auto const indexed_indirect = capture_mode("1", "0");
    ASSERT_TRUE(six_direct.has_value());
    for (auto const& capture : { six_indirect, indexed_direct, indexed_indirect }) {
        ASSERT_TRUE(capture.has_value());
        EXPECT_GT(stonePixelCount(*capture), 0U);
        EXPECT_EQ(capture->rgba8, six_direct->rgba8);
    }
}

TEST_F(StoneRendererAcceptanceTest, RepeatedAndUpdatedMeshesHaveExpectedFrameBehavior)
{
    shared::ChunkMesh const mesh = deterministicChunk();
    shared::ChunkMesh const same_mesh = deterministicChunk();
    shared::ChunkMesh const empty_mesh = emptyChunkMesh();
    shared::ChunkMesh const changed_mesh = singleStoneMesh();
    ASSERT_EQ(mesh.content_identity, same_mesh.content_identity);
    ASSERT_FALSE(mesh.faces.empty());
    ASSERT_TRUE(empty_mesh.faces.empty());
    ASSERT_EQ(changed_mesh.faces.size(), 6U);

    m_renderer->setCamera(ABOVE_CAMERA);
    m_renderer->setChunkMesh(mesh);
    client::RendererFrameCapture const first = render(*m_renderer);
    client::RendererFrameCapture const repeated = render(*m_renderer);
    expectVisibleTerrain(first);
    EXPECT_EQ(first.rgba8, repeated.rgba8);

    m_renderer->setChunkMesh(mesh);
    client::RendererFrameCapture const same_mesh_again = render(*m_renderer);
    EXPECT_EQ(first.rgba8, same_mesh_again.rgba8);

    m_renderer->setChunkMesh(empty_mesh);
    client::RendererFrameCapture const cleared = render(*m_renderer);
    EXPECT_EQ(stonePixelCount(cleared), 0U);
    EXPECT_GT(skyPixelCount(cleared), static_cast<uint64_t>(WIDTH) * HEIGHT - 100U);

    m_renderer->setChunkMesh(changed_mesh);
    client::RendererFrameCapture const changed = render(*m_renderer);
    EXPECT_GT(stonePixelCount(changed), 0U);
    EXPECT_NE(changed.rgba8, cleared.rgba8);
    EXPECT_EQ(m_renderer->validationErrorCount(), 0U);
}

TEST_F(StoneRendererAcceptanceTest, RemotePlayerAltitudeChangesItsVisiblePosition)
{
    m_renderer->setChunkMesh(deterministicChunk());
    m_renderer->setCamera({ .position = { 8.0, -20.0, 12.0 }, .angles = {} });
    std::array<client::PlayerRenderData, 1> players{
        client::PlayerRenderData{ .x = 8.0F, .y = 4.0F, .color = { 1.0F, 0.0F, 0.0F, 1.0F }, .z = 10.0F },
    };
    auto const red_center = [](client::RendererFrameCapture const& frame) -> std::optional<double> {
        uint64_t count = 0U;
        uint64_t row_sum = 0U;
        for (uint64_t offset = 0U; offset < frame.rgba8.size(); offset += 4U) {
            if (frame.rgba8[offset] > 80U && frame.rgba8[offset + 1U] < 20U && frame.rgba8[offset + 2U] < 20U) {
                ++count;
                row_sum += offset / 4U / frame.width;
            }
        }
        if (count == 0U) {
            return std::nullopt;
        }
        return static_cast<double>(row_sum) / static_cast<double>(count);
    };
    auto const lower = red_center(m_renderer->render(players, std::chrono::steady_clock::now() + RENDER_TIMEOUT));
    players[0].z = 14.0F;
    auto const higher = red_center(m_renderer->render(players, std::chrono::steady_clock::now() + RENDER_TIMEOUT));
    ASSERT_TRUE(lower.has_value()) << "remote player is not visible above the stone terrain";
    ASSERT_TRUE(higher.has_value()) << "elevated remote player is not visible";
    EXPECT_LT(*higher, *lower - 10.0) << "increasing world altitude must move the visible player upward";
    EXPECT_EQ(m_renderer->validationErrorCount(), 0U);
}

} // namespace
