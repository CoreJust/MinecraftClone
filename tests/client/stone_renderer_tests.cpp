#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/ChunkMesher.hpp>

#include <core/graphics/vulkan/Vulkan.hpp>

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
#include <string>
#include <variant>

#ifndef MC_STONE_RENDERER_FAIL_UNSUPPORTED
#define MC_STONE_RENDERER_FAIL_UNSUPPORTED 0
#endif

namespace {

constexpr uint32_t WIDTH = 640U;
constexpr uint32_t HEIGHT = 480U;
constexpr auto RENDER_TIMEOUT = std::chrono::seconds{ 10 };

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
