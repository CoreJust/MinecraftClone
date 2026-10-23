#include <client/TerrainLodPresentation.hpp>
#include <client/render/HeightTileDrawIndex.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/ShaderAssets.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#include <core/graphics/vulkan/Vulkan.hpp>
#include <core/kernel/Spirv.hpp>
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
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef MC_STONE_RENDERER_FAIL_UNSUPPORTED
#define MC_STONE_RENDERER_FAIL_UNSUPPORTED 0
#endif

namespace {

class TerrainFragmentAssets final : public client::ShaderAssets {
public:
    explicit TerrainFragmentAssets(bool const solid) : m_solid(solid) {}

    [[nodiscard]] core::kernel::SpirvModule load(std::string_view const name) const override
    {
        return m_installed.load(m_solid && name == "stone.frag.spv" ? "stone_solid.frag.spv" : name);
    }

private:
    client::InstalledShaderAssets m_installed;
    bool m_solid;
};

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

TEST_F(StoneRendererAcceptanceTest, CanonicalClimateMatchesPhysicalMaterialDespiteWrongInterpolatedTemperature)
{
    auto synthetic = std::make_shared<generation::GlobalAtlas>();
    synthetic->configuration.resolution = 256U;
    synthetic->vertical_scale = 1.0;
    synthetic->elevation.assign(256U * 256U, 16.0F);
    synthetic->temperature.assign(256U * 256U, 12.0F);
    synthetic->precipitation.assign(256U * 256U, 0.3F);
    synthetic->water_level.assign(256U * 256U, 0.0F);
    synthetic->ocean_mask.assign(256U * 256U, 0U);
    synthetic->lake_mask.assign(256U * 256U, 0U);
    synthetic->river_mask.assign(256U * 256U, 0U);
    synthetic->temperature[0U] = -4.0F;
    synthetic->temperature[1U] = 2.0F;
    synthetic->temperature[256U] = 0.0F;
    synthetic->temperature[257U] = -1.0F;

    m_renderer.reset();
    for (bool const solid_fragment : {false, true}) {
        SCOPED_TRACE(solid_fragment ? "solid fragment" : "textured fragment");
        TerrainFragmentAssets const assets{solid_fragment};
        client::VulkanOffscreenRenderer renderer{assets, true};
        auto const verify = [&](std::shared_ptr<generation::GlobalAtlas const> const& atlas,
            uint32_t const x, uint32_t const y, shared::Block const expected, int32_t const wrong_temperature) {
            SCOPED_TRACE(testing::Message{} << "XY=" << x << ',' << y << " material=" << static_cast<int>(expected));
            shared::TerrainWorld const world{atlas};
            auto const surface = world.sampleSurface({
                .x = static_cast<int32_t>(x / 16U), .y = static_cast<int32_t>(y / 16U), .z = 0,
            });
            auto const& column = surface.columns[y % 16U * 16U + x % 16U];
            ASSERT_EQ(column.materialAt(column.highest_solid_z, atlas->block_sea_level), expected);
            int32_t const height = (static_cast<int32_t>(column.highest_solid_z) + 1) * 32;
            int32_t const px = static_cast<int32_t>(x);
            int32_t const py = static_cast<int32_t>(y);
            std::array<std::array<int32_t, 3>, 4> const corners{{
                {px - 1, py - 1, height}, {px + 1, py - 1, height},
                {px + 1, py + 1, height}, {px - 1, py + 1, height},
            }};
            auto make_scene = [&](bool const climate_surface, shared::Block const material) {
                auto scene = std::make_shared<client::TerrainLodScene>();
                scene->shading_atlas = atlas;
                scene->block_sea_level = atlas->block_sea_level;
                for (auto const indices : {std::array{0U, 1U, 2U}, std::array{0U, 2U, 3U}}) {
                    std::array<std::array<int32_t, 3>, 3> const vertices{
                        corners[indices[0]], corners[indices[1]], corners[indices[2]],
                    };
                    scene->faces.push_back(climate_surface
                        ? client::StoneFaceInstance::fromTerrainSurfaceTriangle(vertices,
                            {wrong_temperature, wrong_temperature, wrong_temperature})
                        : client::StoneFaceInstance::fromTerrainTriangle(vertices, material));
                }
                return scene;
            };
            renderer.setCamera({
                .position = {static_cast<double>(x), static_cast<double>(y) - 3.0,
                    static_cast<double>(height) / 32.0 + 4.0},
                .angles = {.pitch_degrees = -55.0},
            });
            renderer.setTerrainLodScene(make_scene(false, expected));
            auto const physical = render(renderer);
            renderer.setTerrainLodScene(make_scene(false,
                expected == shared::Block::Grass ? shared::Block::Snow : shared::Block::Grass));
            auto const wrong_material = render(renderer);
            ASSERT_NE(wrong_material.rgba8, physical.rgba8) << "the material comparison must cover visible terrain";
            renderer.setTerrainLodScene(make_scene(true, expected));
            auto const climate = render(renderer);
            EXPECT_EQ(climate.rgba8, physical.rgba8)
                << "canonical field shading must match the physical material, independent of legacy vertex temperature";
            EXPECT_EQ(renderer.validationErrorCount(), 0U);
        };

        // The old affine L0 fan gives -2 C here; canonical bilinear climate is -1.671875 C.
        verify(synthetic, 80U, 64U, shared::Block::Grass, -64);
        verify(synthetic, 65'535U, 64U, shared::Block::Snow, 0);
        auto const owner_atlas = generation::prepareAtlas({.seed = 42U, .resolution = 256U});
        verify(owner_atlas, 32'631U, 48'477U, shared::Block::Snow, 0);
        for (auto const elevation : {0.0F, -1.0F, -5.0F}) {
            auto shoreline = std::make_shared<generation::GlobalAtlas>(*synthetic);
            shoreline->temperature.assign(256U * 256U, -2.0F);
            shoreline->elevation.assign(256U * 256U, elevation);
            shoreline->ocean_mask.assign(256U * 256U, elevation < 0.0F ? 1U : 0U);
            shared::Block const expected = elevation >= 0.0F ? shared::Block::Snow
                : (elevation == -1.0F ? shared::Block::Ice : shared::Block::Stone);
            verify(shoreline, 128U, 128U, expected, 0);
        }
    }
}

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

TEST_F(StoneRendererAcceptanceTest, AllThreeQuadModesHaveByteIdenticalOffscreenOutput)
{
    ScopedStoneSetting const control_shader{ "MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER", "0" };
    std::array const cameras{
        ABOVE_CAMERA,
        BELOW_CAMERA,
        client::CameraPose{ .position = { -12.0, 8.0, 8.0 }, .angles = { .yaw_degrees = 90.0 } },
        client::CameraPose{ .position = { 28.0, 8.0, 8.0 }, .angles = { .yaw_degrees = -90.0 } },
        client::CameraPose{ .position = { 8.0, 28.0, 8.0 }, .angles = { .yaw_degrees = 180.0 } },
    };
    shared::ChunkMesh mesh = deterministicChunk();
    std::vector<shared::MeshFace> visible_faces = std::move(mesh.faces);
    ASSERT_FALSE(visible_faces.empty());
    // Keep the original mesh beyond a complete index block using valid chunk-local geometry.
    mesh.faces.assign(client::StoneIndirectDraws::QUADS_PER_INDEX_BLOCK, visible_faces.front());
    mesh.faces.insert(mesh.faces.end(), visible_faces.begin(), visible_faces.end());
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
    auto const indexed_instanced = capture_mode("1");
    auto const indexed_block = capture_mode("2");
    auto const default_mode = capture_mode(nullptr);
    auto const specialized_block = [&] {
        ScopedStoneSetting const selected{ "MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER", "1" };
        return capture_mode("2");
    }();
    ASSERT_EQ(six_vertex.size(), indexed_instanced.size());
    ASSERT_EQ(six_vertex.size(), indexed_block.size());
    ASSERT_EQ(six_vertex.size(), default_mode.size());
    ASSERT_EQ(six_vertex.size(), specialized_block.size());
    for (uint32_t view = 0U; view < static_cast<uint32_t>(six_vertex.size()); ++view) {
        EXPECT_GT(stonePixelCount(indexed_instanced[view]), 0U) << view;
        EXPECT_GT(stonePixelCount(indexed_block[view]), 0U) << view;
        EXPECT_EQ(indexed_instanced[view].rgba8, six_vertex[view].rgba8) << view;
        EXPECT_EQ(indexed_block[view].rgba8, six_vertex[view].rgba8) << view;
        EXPECT_EQ(default_mode[view].rgba8, six_vertex[view].rgba8) << view;
        EXPECT_EQ(specialized_block[view].rgba8, indexed_block[view].rgba8) << view;
    }
}

TEST_F(StoneRendererAcceptanceTest, SpecializedShaderRejectsOtherQuadModes)
{
    ScopedStoneSetting const specialized_shader{ "MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER", "1" };
    for (char const* const mode : { "0", "1" }) {
        ScopedStoneSetting const selected{ "MC_DIAGNOSTIC_INDEXED_STONE_QUADS", mode };
        EXPECT_THROW((client::VulkanOffscreenRenderer{ m_shader_assets, true }), std::invalid_argument);
    }
}

TEST_F(StoneRendererAcceptanceTest, DirectionSelectionPreservesOffscreenPixelsAndTerrainPlayerDepthOrdering)
{
    static constexpr std::array<client::CameraPose, 8> CAMERAS{
        ABOVE_CAMERA, BELOW_CAMERA,
        client::CameraPose{ .position = { -12.0, 8.0, 8.0 }, .angles = { .yaw_degrees = 90.0 } },
        client::CameraPose{ .position = { 28.0, 8.0, 8.0 }, .angles = { .yaw_degrees = -90.0 } },
        client::CameraPose{ .position = { 8.0, 28.0, 8.0 }, .angles = { .yaw_degrees = 180.0 } },
        client::CameraPose{ .position = { 65'530.0, 8.0, 8.0 }, .angles = { .yaw_degrees = 90.0 } },
        client::CameraPose{ .position = { 8.0, -20.0, 5.0 }, .angles = {} },
        client::CameraPose{ .position = { 8.0, -20.0, 4.0 }, .angles = {} },
    };
    static constexpr std::array PLAYERS{
        client::PlayerRenderData{ .x = 8.0F, .y = 8.0F, .color = { 1.0F, 0.0F, 0.0F, 1.0F }, .z = 4.0F },
    };
    shared::ChunkMesh mesh = singleStoneMesh();
    std::ranges::stable_sort(mesh.faces, {}, &shared::MeshFace::direction);
    std::vector<shared::HeightTileSurfaceQuad> quads;
    for (shared::MeshFace const face : mesh.faces) {
        quads.push_back({ .x = face.local_origin.x, .y = face.local_origin.y, .z = face.local_origin.z,
            .direction = static_cast<shared::HeightTileSurfaceDirection>(face.direction) });
    }
    auto const spans = client::HeightTileDrawIndex::directionalSpans(quads);
    for (client::CameraPose const camera : CAMERAS) {
        m_renderer->setCamera(camera);
        m_renderer->setChunkMesh(mesh);
        auto const baseline = m_renderer->render(PLAYERS, std::chrono::steady_clock::now() + RENDER_TIMEOUT);
        std::vector<client::StoneIndirectDraws::Range> ranges;
        client::HeightTileDrawIndex::appendVisibleRanges(
            { 0U, static_cast<uint32_t>(quads.size()) }, spans, camera.position, ranges
        );
        shared::ChunkMesh selected = mesh;
        selected.faces.clear();
        for (auto const range : ranges) {
            for (uint32_t instance = 0U; instance < range.instance_count; ++instance) {
                selected.faces.push_back(mesh.faces[range.first_instance + instance]);
            }
        }
        m_renderer->setChunkMesh(selected);
        auto const culled = m_renderer->render(PLAYERS, std::chrono::steady_clock::now() + RENDER_TIMEOUT);
        EXPECT_EQ(culled.rgba8, baseline.rgba8);
    }
    EXPECT_EQ(m_renderer->validationErrorCount(), 0U);
}

TEST_F(StoneRendererAcceptanceTest, InstalledSeamBucketsMatchWholeTilePixelsAcrossSubmissionModesAndWrap)
{
    static constexpr uint32_t TILE_EDGE = 16U;
    static constexpr std::array<client::CameraPose, 4> CAMERAS{
        client::CameraPose{ .position = { 24.0, -40.0, 25.0 }, .angles = { .pitch_degrees = -25.0 } },
        client::CameraPose{ .position = { -24.0, 8.0, 15.0 },
            .angles = { .yaw_degrees = 90.0, .pitch_degrees = -10.0 } },
        client::CameraPose{ .position = { 24.0, 40.0, 8.0 }, .angles = { .yaw_degrees = 180.0 } },
        client::CameraPose{ .position = { 65'512.0, 8.0, 15.0 },
            .angles = { .yaw_degrees = 90.0, .pitch_degrees = -10.0 } },
    };
    shared::HeightTile first{ .coordinate = { .x = 0, .y = 0 } };
    shared::HeightTile second{ .coordinate = { .x = 1, .y = 0 } };
    first.heights.fill(8U);
    second.heights.fill(3U);
    for (uint32_t y = 4U; y < 8U; ++y) {
        for (uint32_t x = 4U; x < 8U; ++x) {
            second.heights[y * TILE_EDGE + x] = 0U;
        }
    }
    shared::HeightTileSurfaceMesher const mesher;
    std::array meshes{
        mesher.build(first, { .positive_x = first.heights }, shared::HeightTileSurfaceDetail::Coarse),
        mesher.build(second),
    };
    auto const bridge = shared::heightTileSurfaceSeamBridge(
        meshes[0], meshes[1], shared::HeightTileSurfaceDirection::PositiveX
    );
    ASSERT_FALSE(bridge.first.empty());
    meshes[0].quads.insert(meshes[0].quads.end(), bridge.first.begin(), bridge.first.end());
    uint64_t const resident_faces = meshes[0].quads.size() + meshes[1].quads.size();
    core::platform::glfw::GlfwWindow window{
        { .width = WIDTH, .height = HEIGHT, .title = "MinecraftClone directional terrain parity" },
    };
    glfwHideWindow(window.nativeHandle());
    auto const context = client::VulkanRenderer::createPresentationContext(
        window, { .require_validation = true, .enable_frame_capture = true }
    );
    auto const capture_mode = [&](char const* const culling, char const* const quads, char const* const direct) {
        ScopedStoneSetting const selected_culling{ "MC_DIAGNOSTIC_DIRECTIONAL_STONE_CULLING", culling };
        ScopedStoneSetting const selected_quads{ "MC_DIAGNOSTIC_INDEXED_STONE_QUADS", quads };
        ScopedStoneSetting const selected_direct{ "MC_DIAGNOSTIC_DIRECT_STONE_DRAWS", direct };
        client::VulkanRenderer renderer{
            context, m_shader_assets, { .require_validation = true, .enable_frame_capture = true }
        };
        renderer.setDebugHudEnabled(false);
        client::RendererHeightTileUpsertDiagnostics upsert_diagnostics;
        auto const upsert_started_at = std::chrono::steady_clock::now();
        bool const upserted = renderer.upsertHeightTileMeshes(
            meshes,
            upsert_started_at + RENDER_TIMEOUT,
            &upsert_diagnostics
        );
        EXPECT_TRUE(upserted);
        if (!upserted) {
            return std::vector<client::RendererFrameCapture>{};
        }
        auto const upsert_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - upsert_started_at
        );
        EXPECT_GT(upsert_diagnostics.free_range_prepare_duration, std::chrono::nanoseconds::zero());
        EXPECT_GT(upsert_diagnostics.mesh_prepare_duration, std::chrono::nanoseconds::zero());
        EXPECT_GT(upsert_diagnostics.capacity_prepare_duration, std::chrono::nanoseconds::zero());
        EXPECT_GT(upsert_diagnostics.commit_upload_duration, std::chrono::nanoseconds::zero());
        EXPECT_LE(
            upsert_diagnostics.free_range_prepare_duration
                + upsert_diagnostics.mesh_prepare_duration
                + upsert_diagnostics.capacity_prepare_duration
                + upsert_diagnostics.commit_upload_duration,
            upsert_duration
        );
        std::vector<client::RendererFrameCapture> captures;
        for (client::CameraPose const camera : CAMERAS) {
            renderer.setCamera(camera);
            renderer.requestFrameCapture();
            auto const deadline = std::chrono::steady_clock::now() + RENDER_TIMEOUT;
            for (uint32_t frame = 0U; frame < 32U && std::chrono::steady_clock::now() < deadline; ++frame) {
                if (!window.nextFrame()) {
                    break;
                }
                static_cast<void>(renderer.render({}, deadline));
                if (renderer.captureState() == client::FrameCaptureState::Completed) {
                    break;
                }
            }
            auto capture = renderer.takeFrameCapture();
            EXPECT_TRUE(capture.has_value());
            if (!capture.has_value()) {
                break;
            }
            auto const info = renderer.runtimeInfo();
            EXPECT_EQ(info.height_tile_mesh_count, 2U);
            EXPECT_EQ(info.chunk_face_count, resident_faces);
            if (culling[0] == '0') {
                EXPECT_EQ(info.chunk_draw_face_count, resident_faces);
            } else {
                EXPECT_LT(info.chunk_draw_face_count, resident_faces);
            }
            captures.push_back(std::move(*capture));
        }
        EXPECT_EQ(renderer.validationErrorCount(), 0U);
        return captures;
    };
    auto const baseline = capture_mode("0", "1", "1");
    ASSERT_EQ(baseline.size(), CAMERAS.size());
    for (char const* const quads : { "0", "1", "2" }) {
        for (char const* const direct : { "0", "1" }) {
            auto const culled = capture_mode("1", quads, direct);
            ASSERT_EQ(culled.size(), baseline.size());
            for (uint32_t view = 0U; view < baseline.size(); ++view) {
                EXPECT_EQ(culled[view].rgba8, baseline[view].rgba8) << view;
            }
        }
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
