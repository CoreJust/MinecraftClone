#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#include <core/platform/glfw/GlfwWindow.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <gtest/gtest.h>

#include <testsupport/ImageComparison.hpp>
#include <testsupport/TerrainFixtures.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

TEST(RendererSmokeTest, DisabledTerrainOcclusionRetainsEveryRange)
{
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(false, true, VK_SUCCESS, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(false, false, VK_NOT_READY, 0U, 0U));
}

TEST(RendererSmokeTest, TerrainOcclusionRetainsEveryNonzeroResult)
{
    EXPECT_FALSE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, 1U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, UINT64_MAX, 1U));
}

TEST(RendererSmokeTest, TerrainOcclusionFailsClosedOnStaleUnavailableOrFailedQueries)
{
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, false, VK_SUCCESS, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, 0U, 0U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_NOT_READY, 0U, 1U));
    EXPECT_TRUE(client::detail::diagnosticTerrainRangeVisible(true, true, VK_ERROR_DEVICE_LOST, 0U, 1U));
}

class InertPresentationBackend final : public core::graphics::vulkan::PresentationBackend {
public:
    core::graphics::vulkan::FenceWaitResult recreation_wait_result = core::graphics::vulkan::FenceWaitResult::Completed;
    bool ready = false;
    uint32_t acquire_count = 0U;
    uint32_t submit_count = 0U;
    uint32_t image_count = 1U;
    bool invoke_record_callback = false;
    std::vector<uint32_t> waited_slots;
    std::vector<std::chrono::nanoseconds> recreation_wait_budgets;

    [[nodiscard]] core::graphics::vulkan::FenceWaitResult waitForSlot(
        uint32_t const slot,
        std::chrono::nanoseconds
    ) override
    {
        waited_slots.push_back(slot);
        return core::graphics::vulkan::FenceWaitResult::Completed;
    }

    [[nodiscard]] core::graphics::vulkan::FenceWaitResult waitForRecreation(
        std::chrono::nanoseconds const deadline
    ) override
    {
        recreation_wait_budgets.push_back(deadline);
        return recreation_wait_result;
    }

    [[nodiscard]] core::graphics::vulkan::PresentationAcquireResult acquire(
        uint32_t,
        std::chrono::nanoseconds,
        uint32_t& image
    ) override
    {
        ++acquire_count;
        image = (acquire_count - 1U) % image_count;
        return ready ? core::graphics::vulkan::PresentationAcquireResult::Ready
            : core::graphics::vulkan::PresentationAcquireResult::NotReady;
    }

    void record(
        VkCommandBuffer const command, void (*callback)(VkCommandBuffer, void*), void* const user_data
    ) override
    {
        if (invoke_record_callback) {
            callback(command, user_data);
        }
    }
    void submit(uint32_t, VkCommandBuffer) override { ++submit_count; }

    [[nodiscard]] core::graphics::vulkan::PresentationAcquireResult present(
        uint32_t,
        uint32_t
    ) override
    {
        return ready ? core::graphics::vulkan::PresentationAcquireResult::Ready
            : core::graphics::vulkan::PresentationAcquireResult::NotReady;
    }

    void abandon(uint32_t, uint32_t) noexcept override {}
    void recreate(VkExtent2D) override {}

    [[nodiscard]] std::span<uint8_t const> completedReadback(uint32_t) const noexcept override
    {
        return {};
    }
};

struct PendingGrowthPresentationFixture final {
    InertPresentationBackend* backend = nullptr;
    std::shared_ptr<core::graphics::vulkan::PresentationContext> context;

    PendingGrowthPresentationFixture()
    {
        auto owned_backend = std::make_unique<InertPresentationBackend>();
        backend = owned_backend.get();
        backend->ready = true;
        context = core::graphics::vulkan::PresentationContext::createForTesting(
            { .extent = { .width = 64U, .height = 64U } }, std::move(owned_backend)
        );
    }

    [[nodiscard]] bool present()
    {
        std::optional<core::graphics::vulkan::PresentationContext::Frame> frame;
        if (context->acquire(std::chrono::milliseconds{ 8 }, frame)
                != core::graphics::vulkan::PresentationAcquireResult::Ready || !frame) {
            return false;
        }
        frame->record([](VkCommandBuffer, void*) {}, nullptr);
        context->complete(*frame);
        return true;
    }
};

class SynchronizationRecorder final {
public:
    SynchronizationRecorder()
        : m_previous(s_current)
    {
        s_current = this;
    }

    ~SynchronizationRecorder() { s_current = m_previous; }
    SynchronizationRecorder(SynchronizationRecorder const&) = delete;
    SynchronizationRecorder& operator=(SynchronizationRecorder const&) = delete;

    static VKAPI_ATTR void VKAPI_CALL pipelineBarrier(
        VkCommandBuffer, VkPipelineStageFlags const source_stages, VkPipelineStageFlags const destination_stages,
        VkDependencyFlags, uint32_t, VkMemoryBarrier const*, uint32_t const buffer_count,
        VkBufferMemoryBarrier const* const buffers, uint32_t const image_count,
        VkImageMemoryBarrier const* const images
    )
    {
        auto& recorder = *s_current;
        for (uint32_t i = 0U; i < buffer_count; ++i) {
            auto const& barrier = buffers[i];
            if ((source_stages & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) != 0U
                && (destination_stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0U) {
                recorder.completeTextReads();
            }
            if ((source_stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0U
                && (destination_stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0U
                && (barrier.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) != 0U
                && (barrier.dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) != 0U) {
                ++recorder.ordered_text_overwrites;
            }
            if ((source_stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0U
                && (destination_stages & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) != 0U
                && (barrier.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) != 0U
                && (barrier.dstAccessMask & VK_ACCESS_SHADER_READ_BIT) != 0U) {
                recorder.m_text_visible = true;
            }
        }
        for (uint32_t i = 0U; i < image_count; ++i) {
            auto const& barrier = images[i];
            recorder.depth_layouts.push_back({barrier.oldLayout, barrier.newLayout});
            bool const orders_depth = (source_stages & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) != 0U
                && (destination_stages & VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT) != 0U
                && (barrier.srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0U
                && (barrier.dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT) != 0U
                && (barrier.dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0U;
            if (orders_depth) {
                std::erase(recorder.m_pending_depth_writes, barrier.image);
            }
        }
    }

    static VKAPI_ATTR void VKAPI_CALL updateBuffer(
        VkCommandBuffer, VkBuffer, VkDeviceSize const offset, VkDeviceSize const size, void const* const data
    )
    {
        auto& recorder = *s_current;
        EXPECT_LE(size, 65'536U);
        EXPECT_EQ(size % 4U, 0U);
        EXPECT_EQ(offset % 4U, 0U);
        recorder.m_text_storage.resize(std::max<uint64_t>(recorder.m_text_storage.size(), offset + size));
        std::memcpy(recorder.m_text_storage.data() + offset, data, size);
        recorder.m_text_visible = false;
        ++recorder.text_update_count;
    }

    void readText(uint64_t const byte_count)
    {
        EXPECT_TRUE(m_text_visible);
        m_pending_text_reads.push_back(byte_count);
    }

    void completeTextReads()
    {
        for (uint64_t const byte_count : m_pending_text_reads) {
            std::vector<std::byte> bytes(byte_count);
            std::memcpy(bytes.data(), m_text_storage.data(), byte_count);
            completed_text_reads.push_back(std::move(bytes));
        }
        m_pending_text_reads.clear();
    }

    void clearAndUseDepth(VkImage const image)
    {
        unordered_depth_reuses += std::ranges::find(m_pending_depth_writes, image) != m_pending_depth_writes.end()
            ? 1U : 0U;
        m_pending_depth_writes.push_back(image);
    }

    uint32_t ordered_text_overwrites = 0U;
    uint32_t text_update_count = 0U;
    uint32_t unordered_depth_reuses = 0U;
    std::vector<std::vector<std::byte>> completed_text_reads;
    std::vector<std::array<VkImageLayout, 2>> depth_layouts;

private:
    inline static thread_local SynchronizationRecorder* s_current = nullptr;
    SynchronizationRecorder* m_previous = nullptr;
    std::vector<std::byte> m_text_storage;
    std::vector<uint64_t> m_pending_text_reads;
    std::vector<VkImage> m_pending_depth_writes;
    bool m_text_visible = false;
};

TEST(RendererSmokeTest, TextUploadsKeepEverySubmittedFramesGlyphsAcrossThreeSlots)
{
    static constexpr uint32_t FRAME_COUNT = 3U;
    static constexpr uint32_t GLYPH_COUNT = 2'500U;
    VkBuffer const buffer = std::bit_cast<VkBuffer>(uintptr_t{1U});
    SynchronizationRecorder recorder;
    PendingGrowthPresentationFixture fixture;
    fixture.backend->invoke_record_callback = true;
    std::vector<std::vector<std::byte>> expected;
    for (uint32_t i = 0U; i < FRAME_COUNT; ++i) {
        std::vector<client::TextGlyph> glyphs(GLYPH_COUNT);
        for (uint32_t glyph = 0U; glyph < GLYPH_COUNT; ++glyph) {
            glyphs[glyph].character = 'A' + i;
            glyphs[glyph].packed_color = i + glyph;
            glyphs[glyph].x = static_cast<float>(glyph);
        }
        auto const bytes = std::as_bytes(std::span{glyphs});
        expected.emplace_back(bytes.begin(), bytes.end());
        std::optional<core::graphics::vulkan::PresentationContext::Frame> frame;
        ASSERT_EQ(fixture.context->acquire(std::chrono::milliseconds{8}, frame),
            core::graphics::vulkan::PresentationAcquireResult::Ready);
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame->slotIndex(), i);
        struct Upload final {
            SynchronizationRecorder& recorder;
            VkBuffer buffer;
            std::span<client::TextGlyph const> glyphs;
        } upload{recorder, buffer, glyphs};
        frame->record([](VkCommandBuffer const command, void* const data) {
            auto const& upload = *static_cast<Upload*>(data);
            client::detail::recordTextGlyphUpload(command, upload.buffer, upload.glyphs,
                &SynchronizationRecorder::pipelineBarrier, &SynchronizationRecorder::updateBuffer);
            upload.recorder.readText(upload.glyphs.size_bytes());
        }, &upload);
        fixture.context->complete(*frame);
    }
    EXPECT_TRUE(fixture.backend->waited_slots.empty());
    EXPECT_EQ(fixture.backend->submit_count, FRAME_COUNT);
    recorder.completeTextReads();
    EXPECT_EQ(recorder.completed_text_reads, expected);
    EXPECT_EQ(recorder.ordered_text_overwrites, FRAME_COUNT);
    EXPECT_EQ(recorder.text_update_count, 9U);
}

TEST(RendererSmokeTest, DepthReuseFollowsSwapchainImageWhileAllThreeSlotsAreSubmitted)
{
    static constexpr std::array<VkImageLayout, 2> INITIAL{
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    static constexpr std::array<VkImageLayout, 2> REUSE{
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    std::array<bool, 2> initialized{};
    SynchronizationRecorder recorder;
    PendingGrowthPresentationFixture fixture;
    fixture.backend->image_count = 2U;
    fixture.backend->invoke_record_callback = true;
    for (uint32_t i = 0U; i < 3U; ++i) {
        std::optional<core::graphics::vulkan::PresentationContext::Frame> frame;
        core::graphics::vulkan::PresentationAcquireDiagnostics acquired;
        ASSERT_EQ(fixture.context->acquire(std::chrono::milliseconds{8}, frame, acquired),
            core::graphics::vulkan::PresentationAcquireResult::Ready);
        ASSERT_TRUE(frame);
        ASSERT_TRUE(acquired.image_index);
        uint32_t const image_index = *acquired.image_index;
        EXPECT_EQ(frame->slotIndex(), i);
        struct DepthUse final {
            SynchronizationRecorder& recorder;
            VkImage image;
            bool initialized;
        } depth{recorder, std::bit_cast<VkImage>(uintptr_t{image_index + 1U}), initialized[image_index]};
        frame->record([](VkCommandBuffer const command, void* const data) {
            auto const& depth = *static_cast<DepthUse*>(data);
            client::detail::recordDepthAttachmentBarrier(command, depth.image, depth.initialized,
                &SynchronizationRecorder::pipelineBarrier);
            depth.recorder.clearAndUseDepth(depth.image);
        }, &depth);
        initialized[image_index] = true;
        fixture.context->complete(*frame);
    }
    EXPECT_TRUE(fixture.backend->waited_slots.empty());
    EXPECT_EQ(fixture.backend->submit_count, 3U);
    EXPECT_EQ(recorder.unordered_depth_reuses, 0U);
    EXPECT_EQ(recorder.depth_layouts, (std::vector<std::array<VkImageLayout, 2>>{INITIAL, INITIAL, REUSE}));
}

struct CaptureColorClasses final {
    bool has_platform = false;
    bool has_platform_side = false;
    bool has_sky = false;
    bool has_red_player = false;
    bool has_green_player = false;
    bool has_debug_hud = false;

    [[nodiscard]] bool complete() const
    {
        return has_platform && has_platform_side && has_sky
            && has_red_player && has_green_player && has_debug_hud;
    }

    [[nodiscard]] std::string missingClasses() const
    {
        std::string missing;
        auto appendMissing = [&missing](bool const present, char const* const name) {
            if (!present) {
                if (!missing.empty()) {
                    missing += ", ";
                }
                missing += name;
            }
        };
        appendMissing(has_platform, "platform");
        appendMissing(has_platform_side, "platform-side");
        appendMissing(has_sky, "sky");
        appendMissing(has_red_player, "red-player");
        appendMissing(has_green_player, "green-player");
        appendMissing(has_debug_hud, "debug-hud");
        return missing;
    }
};

[[nodiscard]] shared::HeightTileSurfaceMesh denseHeightTileMesh(int32_t const tile_x)
{
    shared::HeightTileSurfaceMesh mesh{
        .coordinate = { .x = tile_x, .y = 0 },
        .quads = {},
        .identity = {},
    };
    mesh.quads.reserve(shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
    for (uint32_t index = 0U; index < shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT; ++index) {
        mesh.quads.push_back({
            .x = tile_x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH)
                + static_cast<int32_t>(index % shared::HeightTile::SIDE_LENGTH),
            .y = static_cast<int32_t>(index / shared::HeightTile::SIDE_LENGTH),
            .z = 6,
            .direction = shared::HeightTileSurfaceDirection::PositiveZ,
        });
    }
    return mesh;
}

[[nodiscard]] shared::HeightTileSurfaceMesh materialTerrainMesh(shared::HeightTileSurfaceNeighbors const& neighbors = {})
{
    shared::HeightTile::Heights heights{};
    for (uint32_t y = 0U; y < shared::HeightTile::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::HeightTile::SIDE_LENGTH; ++x) {
            uint16_t const highest_solid_z = static_cast<uint16_t>(400U + (x / 4U) * 4U);
            heights[y * shared::HeightTile::SIDE_LENGTH + x] = static_cast<uint16_t>(highest_solid_z + 1U);
        }
    }
    shared::SurfaceTile surface = testsupport::makeSurfaceTile(
        { .x = 0, .y = 0 },
        testsupport::makeTerrainIdentity(0x0499U, 1U),
        heights
    );
    for (uint32_t y = 0U; y < shared::HeightTile::SIDE_LENGTH; ++y) {
        for (uint32_t x = 0U; x < shared::HeightTile::SIDE_LENGTH; ++x) {
            uint32_t const column_index = y * shared::HeightTile::SIDE_LENGTH + x;
            double const temperature = x >= 8U && x < 12U ? -5.0 : 0.0;
            double const precipitation = x >= 4U && x < 8U ? 0.1 : 0.5;
            surface.columns[column_index] = testsupport::makeTerrainColumn(
                static_cast<uint16_t>(heights[column_index] - 1U),
                surface.block_sea_level,
                testsupport::TerrainClimate{
                    .temperature_celsius = temperature,
                    .precipitation = precipitation,
                }
            );
        }
    }
    shared::HeightTile typed_tile{
        .coordinate = { .x = 0, .y = 0 },
        .heights = heights,
        .surface = std::move(surface),
    };
    return shared::HeightTileSurfaceMesher{}.build(typed_tile, neighbors);
}

[[nodiscard]] shared::HeightTileSurfaceMesh materialPaletteDisplayMesh()
{
    static constexpr std::array<shared::Block, 6U> MATERIALS{
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Stone,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
    };
    shared::HeightTileSurfaceMesh mesh = materialTerrainMesh();
    for (uint32_t index = 0U; index < MATERIALS.size(); ++index) {
        mesh.quads.push_back({
            .x = 20 + static_cast<int32_t>((index % 3U) * 4U),
            .y = -1,
            .z = 400 + static_cast<int32_t>((index / 3U) * 8U),
            .direction = shared::HeightTileSurfaceDirection::NegativeY,
            .u_extent = 4U,
            .v_extent = 8U,
            .material = MATERIALS[index],
        });
    }
    return mesh;
}

TEST(RendererSmokeTest, MaterialPaletteFixtureCarriesCanonicalMaterialsIntoFaceInstances)
{
    static constexpr std::array<shared::Block, 6U> EXPECTED_MATERIALS{
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Stone,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
    };
    std::array<bool, 7U> observed_materials{};
    // An explicit empty neighbor exposes the full side profile down to bedrock.
    shared::HeightTileSurfaceNeighbors neighbors{};
    neighbors.negative_y = shared::HeightTile::Heights{};
    shared::HeightTileSurfaceMesh const mesh = materialTerrainMesh(neighbors);
    for (shared::HeightTileSurfaceQuad const& quad : mesh.quads) {
        client::StoneFaceInstance const instance = client::StoneFaceInstance::fromHeightTileQuad(quad);
        EXPECT_EQ(instance.material_id, static_cast<uint32_t>(quad.material));
        observed_materials[instance.material_id] = true;
    }
    for (shared::Block const material : EXPECTED_MATERIALS) {
        EXPECT_TRUE(observed_materials[static_cast<uint8_t>(material)])
            << "mesher fixture lacks material id " << static_cast<uint32_t>(material);
    }
}

struct TerrainMaterialColorClasses final {
    bool has_grass = false;
    bool has_dirt = false;
    bool has_stone = false;
    bool has_bedrock = false;
    bool has_snow = false;
    bool has_ice = false;

    [[nodiscard]] bool complete() const noexcept
    {
        return has_grass && has_dirt && has_stone && has_bedrock && has_snow && has_ice;
    }

    [[nodiscard]] std::string missingClasses() const
    {
        std::string missing;
        auto append_missing = [&missing](bool const present, char const* const name) {
            if (!present) {
                if (!missing.empty()) {
                    missing += ", ";
                }
                missing += name;
            }
        };
        append_missing(has_grass, "grass");
        append_missing(has_dirt, "dirt");
        append_missing(has_stone, "stone");
        append_missing(has_bedrock, "bedrock");
        append_missing(has_snow, "snow");
        append_missing(has_ice, "ice");
        return missing;
    }
};

struct PixelRectangle final {
    uint32_t left = 0U;
    uint32_t top = 0U;
    uint32_t right = 0U;
    uint32_t bottom = 0U;

    [[nodiscard]] bool contains(uint32_t const x, uint32_t const y) const noexcept
    {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

[[nodiscard]] std::optional<std::array<uint32_t, 2U>> projectedPixel(
    client::Camera const& camera,
    glm::vec3 const world_position,
    uint32_t const width,
    uint32_t const height
)
{
    std::optional<glm::mat4> const projection = camera.projectionMatrix(width, height);
    if (!projection.has_value()) {
        return std::nullopt;
    }
    glm::vec4 const clip = *projection * camera.viewMatrix() * glm::vec4{ world_position, 1.0F };
    if (clip.w <= 0.0F) {
        return std::nullopt;
    }
    float const pixel_x = (clip.x / clip.w * 0.5F + 0.5F) * static_cast<float>(width);
    float const pixel_y = (clip.y / clip.w * 0.5F + 0.5F) * static_cast<float>(height);
    if (pixel_x < 0.0F || pixel_x >= static_cast<float>(width)
        || pixel_y < 0.0F || pixel_y >= static_cast<float>(height)) {
        return std::nullopt;
    }
    return std::array<uint32_t, 2U>{
        static_cast<uint32_t>(std::floor(pixel_x)),
        static_cast<uint32_t>(std::floor(pixel_y)),
    };
}

[[nodiscard]] std::optional<std::array<uint8_t, 4U>> capturePixelAtWorld(
    client::RendererFrameCapture const& capture,
    client::Camera const& camera,
    glm::vec3 const world_position
)
{
    std::optional<std::array<uint32_t, 2U>> const pixel = projectedPixel(
        camera,
        world_position,
        capture.width,
        capture.height
    );
    if (!pixel.has_value()) {
        return std::nullopt;
    }
    uint64_t const offset = (static_cast<uint64_t>((*pixel)[1U]) * capture.width + (*pixel)[0U]) * 4U;
    if (offset + 4U > capture.rgba8.size()) {
        return std::nullopt;
    }
    return std::array<uint8_t, 4U>{
        capture.rgba8[offset],
        capture.rgba8[offset + 1U],
        capture.rgba8[offset + 2U],
        capture.rgba8[offset + 3U],
    };
}

[[nodiscard]] std::optional<PixelRectangle> projectedIcePanelBounds(
    client::Camera const& camera,
    uint32_t const width,
    uint32_t const height
)
{
    static constexpr std::array<glm::vec3, 4U> ICE_PANEL_CORNERS{
        glm::vec3{ 28.0F, -1.0F, 408.0F },
        glm::vec3{ 32.0F, -1.0F, 408.0F },
        glm::vec3{ 32.0F, -1.0F, 416.0F },
        glm::vec3{ 28.0F, -1.0F, 416.0F },
    };
    std::optional<glm::mat4> const projection = camera.projectionMatrix(width, height);
    if (!projection.has_value()) {
        return std::nullopt;
    }
    glm::mat4 const projection_view = *projection * camera.viewMatrix();
    float left = static_cast<float>(width);
    float top = static_cast<float>(height);
    float right = 0.0F;
    float bottom = 0.0F;
    for (glm::vec3 const corner : ICE_PANEL_CORNERS) {
        glm::vec4 const clip = projection_view * glm::vec4{ corner, 1.0F };
        if (clip.w <= 0.0F) {
            return std::nullopt;
        }
        float const pixel_x = (clip.x / clip.w * 0.5F + 0.5F) * static_cast<float>(width);
        float const pixel_y = (clip.y / clip.w * 0.5F + 0.5F) * static_cast<float>(height);
        left = std::min(left, pixel_x);
        top = std::min(top, pixel_y);
        right = std::max(right, pixel_x);
        bottom = std::max(bottom, pixel_y);
    }
    PixelRectangle const bounds{
        .left = static_cast<uint32_t>(std::clamp(std::floor(left), 0.0F, static_cast<float>(width))),
        .top = static_cast<uint32_t>(std::clamp(std::floor(top), 0.0F, static_cast<float>(height))),
        .right = static_cast<uint32_t>(std::clamp(std::ceil(right), 0.0F, static_cast<float>(width))),
        .bottom = static_cast<uint32_t>(std::clamp(std::ceil(bottom), 0.0F, static_cast<float>(height))),
    };
    return bounds.right > bounds.left && bounds.bottom > bounds.top
        ? std::optional<PixelRectangle>{ bounds }
        : std::nullopt;
}

[[nodiscard]] TerrainMaterialColorClasses classifyTerrainMaterialColors(
    client::RendererFrameCapture const& capture,
    PixelRectangle const ice_panel_bounds
)
{
    TerrainMaterialColorClasses classes;
    for (uint64_t offset = 0U; offset < capture.rgba8.size(); offset += 4U) {
        uint64_t const pixel_index = offset / 4U;
        uint32_t const pixel_x = static_cast<uint32_t>(pixel_index % capture.width);
        uint32_t const pixel_y = static_cast<uint32_t>(pixel_index / capture.width);
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        bool const gray = std::abs(static_cast<int>(red) - static_cast<int>(green)) < 14
            && std::abs(static_cast<int>(green) - static_cast<int>(blue)) < 14;
        uint8_t const brightness = static_cast<uint8_t>(
            (static_cast<uint32_t>(red) + green + blue) / 3U
        );
        bool const stone_brightness = capture.srgb_encoded
            ? brightness > 135U && brightness < 225U
            : brightness > 65U && brightness < 190U;
        bool const bedrock_brightness = capture.srgb_encoded
            ? brightness >= 55U && brightness <= 145U
            : brightness >= 25U && brightness <= 90U;
        classes.has_grass = classes.has_grass || (green > red + 18U && green > blue + 18U);
        classes.has_dirt = classes.has_dirt || (red > green + 18U && green > blue + 10U);
        classes.has_stone = classes.has_stone || (gray && stone_brightness);
        classes.has_bedrock = classes.has_bedrock || (gray && bedrock_brightness);
        classes.has_snow = classes.has_snow || (capture.srgb_encoded
            ? (red > 220U && green > 220U && blue > 220U
                && std::max({ red, green, blue }) - std::min({ red, green, blue }) < 40U)
            : (red > 205U && green > 215U && blue > 225U
                && std::max({ red, green, blue }) - std::min({ red, green, blue }) < 60U));
        if (ice_panel_bounds.contains(pixel_x, pixel_y)) {
            classes.has_ice = classes.has_ice || (capture.srgb_encoded
                ? red >= 105U && red <= 190U && green >= 140U && green <= 210U
                    && blue >= 210U && blue <= 240U && blue > green + 15U
                : red >= 45U && red <= 125U && green >= 75U && green <= 155U
                    && blue >= 165U && blue <= 235U && blue > green + 20U);
        }
    }
    return classes;
}

[[nodiscard]] bool isObservableHudPixel(
    uint8_t const red,
    uint8_t const green,
    uint8_t const blue
)
{
    uint8_t const maximum = std::max(red, std::max(green, blue));
    uint8_t const minimum = std::min(red, std::min(green, blue));
    bool const bright_color = maximum >= 220U && maximum - minimum >= 35U;
    bool const bright_neutral = minimum >= 180U;
    // The presentation background is a blue-dominant sky. Keep its high blue
    // channel from being mistaken for generic colored text, while allowing the
    // brighter cyan HUD row through.
    bool const sky_like = blue > green + 20U && green > red + 20U && green < 200U;
    return bright_neutral || (bright_color && !sky_like);
}

[[nodiscard]] CaptureColorClasses classifyCaptureColors(client::RendererFrameCapture const& capture)
{
    CaptureColorClasses classes;
    uint32_t debug_hud_pixel_count = 0U;
    for (uint64_t offset = 0U; offset < capture.rgba8.size(); offset += 4U) {
        uint32_t const pixel_index = static_cast<uint32_t>(offset / 4U);
        uint32_t const x = pixel_index % capture.width;
        uint32_t const y = pixel_index / capture.width;
        uint8_t const red = capture.rgba8[offset];
        uint8_t const green = capture.rgba8[offset + 1U];
        uint8_t const blue = capture.rgba8[offset + 2U];
        classes.has_sky = classes.has_sky || (
            blue > green + 20U && green > red + 20U
        );
        classes.has_platform = classes.has_platform || (
            red < 130U && blue < 170U && blue > green + 2U && green > red + 2U
        );
        classes.has_platform_side = classes.has_platform_side || (
            y > 0U
            && red < 130U && blue < 170U && blue > green + 2U && green > red + 2U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U] < 130U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 2U] < 170U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 2U]
                > capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 1U] + 2U
            && capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U + 1U]
                > capture.rgba8[offset - static_cast<uint64_t>(capture.width) * 4U] + 2U
        );
        classes.has_red_player = classes.has_red_player || (
            red > 200U && green < 80U && blue < 80U
        );
        classes.has_green_player = classes.has_green_player || (
            red < 80U && green > 150U && blue < 80U
        );
        if (x < 220U && y < 80U && isObservableHudPixel(red, green, blue)) {
            ++debug_hud_pixel_count;
        }
    }
    classes.has_debug_hud = debug_hud_pixel_count >= 20U;
    return classes;
}

TEST(RendererSmokeTest, CompletedCaptureSurvivesRecreateAndReadsBack)
{
    static constexpr uint32_t INITIAL_LOGICAL_WIDTH = 320U;
    static constexpr uint32_t INITIAL_LOGICAL_HEIGHT = 240U;
    static constexpr int32_t RESIZED_WIDTH = 400;
    static constexpr int32_t RESIZED_HEIGHT = 300;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr uint32_t MAXIMUM_INITIAL_STONE_FACE_CAPACITY = 65'536U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = INITIAL_LOGICAL_WIDTH,
            .height = INITIAL_LOGICAL_HEIGHT,
            .title = "MinecraftClone presentation smoke",
        },
    };
    uint32_t initial_framebuffer_width = 0U;
    uint32_t initial_framebuffer_height = 0U;
    window.framebufferSize(initial_framebuffer_width, initial_framebuffer_height);
    ASSERT_GT(initial_framebuffer_width, 0U);
    ASSERT_GT(initial_framebuffer_height, 0U);
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(false);
    EXPECT_LE(renderer.runtimeInfo().stone_face_capacity, MAXIMUM_INITIAL_STONE_FACE_CAPACITY);
    std::array<client::PlayerRenderData, 2> const players{
        client::PlayerRenderData{ .x = 2U, .y = 3U, .color = { 1.0F, 0.0F, 0.0F, 1.0F } },
        client::PlayerRenderData{ .x = 29U, .y = 28U, .color = { 0.0F, 1.0F, 0.0F, 1.0F } },
    };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    uint32_t rendered_frames = 0U;
    auto completeCapture = [&] {
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            if (!window.nextFrame()) {
                return false;
            }
            if (renderer.render(
                    players,
                    client::DebugHudInput{ .player_x = 2.0F, .player_y = 3.0F },
                    1.0F,
                    deadline
                )) {
                ++rendered_frames;
            }
            if (renderer.captureState() == client::FrameCaptureState::Completed) {
                return true;
            }
        }
        return false;
    };

    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const original_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(original_capture.has_value());
    EXPECT_EQ(original_capture->width, initial_framebuffer_width);
    EXPECT_EQ(original_capture->height, initial_framebuffer_height);
    EXPECT_EQ(
        original_capture->rgba8.size(),
        static_cast<uint64_t>(initial_framebuffer_width) * initial_framebuffer_height * 4U
    );
    EXPECT_EQ(renderer.runtimeInfo().debug_hud_draw_count, 0U);

    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);

    glfwSetWindowSize(window.nativeHandle(), RESIZED_WIDTH, RESIZED_HEIGHT);
    ASSERT_TRUE(window.nextFrame());
    uint32_t width = 0U;
    uint32_t height = 0U;
    window.framebufferSize(width, height);
    ASSERT_TRUE(width != initial_framebuffer_width || height != initial_framebuffer_height);
    renderer.recreate(width, height);
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const retained_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(retained_capture.has_value());
    EXPECT_EQ(retained_capture->width, initial_framebuffer_width);
    EXPECT_EQ(retained_capture->height, initial_framebuffer_height);
    EXPECT_EQ(retained_capture->srgb_encoded, original_capture->srgb_encoded);
    EXPECT_EQ(
        retained_capture->rgba8.size(),
        static_cast<uint64_t>(initial_framebuffer_width) * initial_framebuffer_height * 4U
    );
    EXPECT_EQ(retained_capture->rgba8, original_capture->rgba8);
    CaptureColorClasses const retained_colors = classifyCaptureColors(*retained_capture);
    EXPECT_TRUE(retained_colors.has_platform);
    EXPECT_TRUE(retained_colors.has_platform_side);
    EXPECT_TRUE(retained_colors.has_sky);
    EXPECT_TRUE(retained_colors.has_red_player);
    EXPECT_TRUE(retained_colors.has_green_player);

    renderer.setDebugHudEnabled(true);
    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    ASSERT_EQ(renderer.captureState(), client::FrameCaptureState::Completed);
    std::optional<client::RendererFrameCapture> const subsequent_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(subsequent_capture.has_value());
    EXPECT_EQ(subsequent_capture->width, width);
    EXPECT_EQ(subsequent_capture->height, height);
    EXPECT_EQ(
        subsequent_capture->rgba8.size(),
        static_cast<uint64_t>(width) * height * 4U
    );
    CaptureColorClasses const subsequent_colors = classifyCaptureColors(*subsequent_capture);
    EXPECT_TRUE(subsequent_colors.complete())
        << "subsequent capture lacks: " << subsequent_colors.missingClasses()
        << "; the resized platform/grid/sky/HUD scene may cover every physical framebuffer sample";
    EXPECT_EQ(renderer.runtimeInfo().debug_hud_draw_count, 1U);

    renderer.hotReload();
    renderer.requestFrameCapture();
    ASSERT_TRUE(completeCapture());
    std::optional<client::RendererFrameCapture> const reloaded_capture = renderer.takeFrameCapture();
    ASSERT_TRUE(reloaded_capture.has_value());
    CaptureColorClasses const reloaded_colors = classifyCaptureColors(*reloaded_capture);
    EXPECT_TRUE(reloaded_colors.complete())
        << "reloaded capture lacks: " << reloaded_colors.missingClasses()
        << "; the platform/grid/sky scene may cover every physical framebuffer sample";
    EXPECT_GE(rendered_frames, 3U);
    EXPECT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
}

TEST(RendererSmokeTest, ProjectedGridLineContrastsWithAdjacentCellSurface)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr std::array<float, 3U> GRID_LINE_POSITION{ 16.0F, 0.0F, 0.0F };
    static constexpr std::array<float, 3U> GRID_CELL_POSITION{ 16.5F, 0.5F, 0.0F };
    static constexpr uint8_t MINIMUM_CHANNEL_CONTRAST = 20U;
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone projected grid-line capture",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(false);
    client::Camera const camera{
        {
            .position = { 16.0, -2.0, 2.0 },
            .angles = { .pitch_degrees = -45.0 },
        },
    };
    renderer.setCamera(camera.pose());
    renderer.requestFrameCapture();
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    std::optional<client::RendererFrameCapture> capture;
    for (uint32_t frame = 0U;
         frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
         ++frame) {
        ASSERT_TRUE(window.nextFrame());
        static_cast<void>(renderer.render({}, deadline));
        capture = renderer.takeFrameCapture();
        if (capture.has_value()) {
            break;
        }
    }
    ASSERT_TRUE(capture.has_value());
    std::optional<std::array<uint8_t, 4U>> const line = capturePixelAtWorld(
        *capture,
        camera,
        glm::vec3{ GRID_LINE_POSITION[0U], GRID_LINE_POSITION[1U], GRID_LINE_POSITION[2U] }
    );
    std::optional<std::array<uint8_t, 4U>> const cell = capturePixelAtWorld(
        *capture,
        camera,
        glm::vec3{ GRID_CELL_POSITION[0U], GRID_CELL_POSITION[1U], GRID_CELL_POSITION[2U] }
    );
    ASSERT_TRUE(line.has_value());
    ASSERT_TRUE(cell.has_value());
    if (char const* const capture_path = std::getenv("MC_PROJECTED_GRID_CAPTURE_PATH");
        capture_path != nullptr && capture_path[0] != '\0') {
        auto const written = testsupport::writePpm(
            {
                .width = capture->width,
                .height = capture->height,
                .srgb_encoded = capture->srgb_encoded,
                .pixels = capture->rgba8,
            },
            std::filesystem::path{ capture_path },
            "projected grid line and adjacent cell presentation capture"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
    }
    for (uint32_t channel = 0U; channel < 3U; ++channel) {
        EXPECT_GE(
            static_cast<uint32_t>((*cell)[channel]),
            static_cast<uint32_t>((*line)[channel]) + MINIMUM_CHANNEL_CONTRAST
        ) << "grid line must remain darker than the adjacent cell in channel " << channel;
    }
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, GrowsStoneFaceArenaWithoutBreakingPresentation)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t TILE_COUNT = 65U;
    static constexpr uint32_t INITIAL_CAPACITY = 65'536U;
    static constexpr double CAMERA_Y = -20.0;
    static constexpr double CAMERA_Z = 18.0;
    static constexpr double CAMERA_X_OFFSET = 8.0;
    static constexpr double CAMERA_PITCH_DEGREES = -25.0;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone stone-face arena smoke",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true }
        ),
        shader_assets,
        { .require_validation = true },
    };
    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    std::chrono::steady_clock::time_point const arena_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (uint32_t tile_index = 0U; tile_index < TILE_COUNT; ++tile_index) {
        ASSERT_TRUE(renderer.upsertHeightTileMesh(
            denseHeightTileMesh(static_cast<int32_t>(tile_index)),
            arena_deadline
        ));
    }
    EXPECT_GT(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);

    for (uint32_t tile_index = 1U; tile_index < TILE_COUNT - 1U; ++tile_index) {
        EXPECT_TRUE(renderer.removeHeightTileMesh({ .x = static_cast<int32_t>(tile_index), .y = 0 }));
    }

    auto const renderVisibleTile = [&renderer, &window](int32_t const tile_x) {
        renderer.setCamera({
            .position = {
                static_cast<double>(tile_x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH))
                    + CAMERA_X_OFFSET,
                CAMERA_Y,
                CAMERA_Z,
            },
            .angles = { .pitch_degrees = CAMERA_PITCH_DEGREES },
        });
        ASSERT_TRUE(window.nextFrame());
        EXPECT_TRUE(renderer.render(
            {},
            client::DebugHudInput{},
            1.0F,
            std::chrono::steady_clock::now() + TIMEOUT
        ));
        EXPECT_GT(renderer.runtimeInfo().chunk_draw_count, 0U);
        EXPECT_EQ(renderer.runtimeInfo().chunk_draw_face_count, shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT);
    };
    renderVisibleTile(0);
    renderVisibleTile(static_cast<int32_t>(TILE_COUNT - 1U));
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, BatchUpsertCoalescesNewlyRetiredRangesWithReusableRanges)
{
    static constexpr uint32_t INITIAL_CAPACITY = 65'536U;
    static constexpr uint32_t MAXIMUM_TILE_FACE_COUNT = shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT;
    static constexpr uint32_t SMALL_TILE_FACE_COUNT = MAXIMUM_TILE_FACE_COUNT / 2U;
    static constexpr uint32_t FULL_TILE_COUNT =
        (INITIAL_CAPACITY - 2U * SMALL_TILE_FACE_COUNT) / MAXIMUM_TILE_FACE_COUNT;
    static constexpr uint32_t FINAL_TILE_FACE_COUNT = INITIAL_CAPACITY - 2U * SMALL_TILE_FACE_COUNT
        - FULL_TILE_COUNT * MAXIMUM_TILE_FACE_COUNT;
    static constexpr uint32_t HALF_RETIREMENT_DELAY_FRAMES = 4U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };

    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone retired stone-range smoke" },
    };
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    auto const deadline = [] { return std::chrono::steady_clock::now() + TIMEOUT; };
    auto mesh_with_faces = [](int32_t const tile_x, uint32_t const face_count) {
        shared::HeightTileSurfaceMesh mesh = denseHeightTileMesh(tile_x);
        mesh.quads.resize(face_count);
        return mesh;
    };

    std::vector<shared::HeightTileSurfaceMesh> resident_meshes;
    resident_meshes.reserve(FULL_TILE_COUNT + 3U);
    resident_meshes.push_back(mesh_with_faces(0, SMALL_TILE_FACE_COUNT));
    resident_meshes.push_back(mesh_with_faces(1, SMALL_TILE_FACE_COUNT));
    for (uint32_t index = 0U; index < FULL_TILE_COUNT; ++index) {
        resident_meshes.push_back(mesh_with_faces(static_cast<int32_t>(index + 2U), MAXIMUM_TILE_FACE_COUNT));
    }
    resident_meshes.push_back(mesh_with_faces(
        static_cast<int32_t>(FULL_TILE_COUNT + 2U), FINAL_TILE_FACE_COUNT
    ));
    for (shared::HeightTileSurfaceMesh const& mesh : resident_meshes) {
        ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline()));
    }
    ASSERT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    ASSERT_EQ(renderer.runtimeInfo().chunk_face_count, INITIAL_CAPACITY);

    EXPECT_TRUE(renderer.removeHeightTileMesh({ .x = 0, .y = 0 }));
    for (uint32_t frame = 0U; frame < HALF_RETIREMENT_DELAY_FRAMES; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline()));
    }
    EXPECT_TRUE(renderer.removeHeightTileMesh({ .x = 1, .y = 0 }));
    for (uint32_t frame = 0U; frame < HALF_RETIREMENT_DELAY_FRAMES; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline()));
    }
    std::array const flush_batch{ resident_meshes[2] };
    ASSERT_TRUE(renderer.upsertHeightTileMeshes(flush_batch, deadline()));

    for (uint32_t frame = 0U; frame < HALF_RETIREMENT_DELAY_FRAMES; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline()));
    }
    std::array const replacement_batch{
        mesh_with_faces(static_cast<int32_t>(resident_meshes.size()), MAXIMUM_TILE_FACE_COUNT),
    };
    ASSERT_TRUE(renderer.upsertHeightTileMeshes(replacement_batch, deadline()));

    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.growth_attempt_count, 0U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, TerrainOcclusionOrdinaryCaptureMatchesBaselineAndInvalidatesOnSceneChange)
{
    static constexpr uint32_t MAXIMUM_FRAMES = 24U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    char const* const requested = std::getenv("MC_DIAGNOSTIC_TERRAIN_OCCLUSION");
    if (requested == nullptr || std::string_view{ requested } != "1") {
        GTEST_SKIP() << "set MC_DIAGNOSTIC_TERRAIN_OCCLUSION=1 to exercise the diagnostic oracle";
    }
    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone terrain occlusion diagnostic" },
    };
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window, { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    if (!renderer.runtimeInfo().diagnostic_terrain_occlusion_enabled) {
        GTEST_SKIP() << "set MC_DIAGNOSTIC_TERRAIN_OCCLUSION=1 to exercise the diagnostic oracle";
    }
    renderer.setDebugHudEnabled(false);
    renderer.setCamera({ .position = { 8.0, -20.0, 25.0 }, .angles = { .pitch_degrees = -30.0 } });
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    shared::HeightTileSurfaceMesh mesh = denseHeightTileMesh(0);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline));
    ASSERT_TRUE(window.nextFrame());
    renderer.requestFrameCapture();
    ASSERT_TRUE(renderer.render({}, deadline));
    ASSERT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_oracle_frame);
    ASSERT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_filtered_frame);
    auto const baseline = renderer.takeFrameCapture();
    ASSERT_TRUE(baseline.has_value());

    bool saw_oracle = false;
    bool saw_filtered = false;
    renderer.requestFrameCapture();
    for (uint32_t frame = 0U; frame < MAXIMUM_FRAMES && std::chrono::steady_clock::now() < deadline; ++frame) {
        ASSERT_TRUE(window.nextFrame());
        ASSERT_TRUE(renderer.render({}, deadline));
        auto const info = renderer.runtimeInfo();
        if (info.diagnostic_terrain_occlusion_oracle_frame) {
            saw_oracle = true;
            EXPECT_FALSE(renderer.takeFrameCapture().has_value());
            EXPECT_FALSE(info.diagnostic_gpu_terrain_duration.has_value());
        }
        if (info.diagnostic_terrain_occlusion_filtered_frame) {
            saw_filtered = true;
            auto const filtered = renderer.takeFrameCapture();
            ASSERT_TRUE(filtered.has_value());
            EXPECT_EQ(filtered->rgba8, baseline->rgba8);
            EXPECT_EQ(filtered->width, baseline->width);
            EXPECT_EQ(filtered->height, baseline->height);
            break;
        }
    }
    ASSERT_TRUE(saw_oracle);
    ASSERT_TRUE(saw_filtered);

    renderer.setCamera({ .position = { 9.0, -20.0, 25.0 }, .angles = { .pitch_degrees = -30.0 } });
    ASSERT_TRUE(window.nextFrame());
    ASSERT_TRUE(renderer.render({}, deadline));
    EXPECT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_filtered_frame);
    EXPECT_EQ(renderer.runtimeInfo().diagnostic_terrain_occlusion_removed_quad_count, 0U);
    mesh.quads.pop_back();
    ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline));
    ASSERT_TRUE(window.nextFrame());
    ASSERT_TRUE(renderer.render({}, deadline));
    EXPECT_FALSE(renderer.runtimeInfo().diagnostic_terrain_occlusion_filtered_frame);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, HeightTileBatchKeepsBothOldMeshesWhenDeadlinePreventsArenaGrowth)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t INITIAL_CAPACITY = 65'536U;
    static constexpr uint32_t BATCH_TILE_COUNT = 2U;
    static constexpr uint32_t FILLER_TILE_COUNT = (INITIAL_CAPACITY - BATCH_TILE_COUNT)
        / shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };

    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone height tile batch smoke",
        },
    };
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window, { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    std::chrono::steady_clock::time_point const setup_deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (uint32_t index = 0U; index < FILLER_TILE_COUNT; ++index) {
        ASSERT_TRUE(renderer.upsertHeightTileMesh(denseHeightTileMesh(static_cast<int32_t>(index)), setup_deadline));
    }

    std::array<shared::HeightTileSurfaceMesh, BATCH_TILE_COUNT> replacements;
    for (uint32_t index = 0U; index < BATCH_TILE_COUNT; ++index) {
        int32_t const tile_x = static_cast<int32_t>(FILLER_TILE_COUNT + index);
        shared::HeightTileSurfaceMesh original{
            .coordinate = { .x = tile_x, .y = 0 },
            .quads = {},
            .identity = {},
        };
        original.quads.push_back({
            .x = tile_x * static_cast<int32_t>(shared::HeightTile::SIDE_LENGTH),
            .y = 0,
            .z = 6,
            .direction = shared::HeightTileSurfaceDirection::PositiveZ,
        });
        ASSERT_TRUE(renderer.upsertHeightTileMesh(original, setup_deadline));
        replacements[index] = denseHeightTileMesh(tile_x);
    }

    uint32_t const original_face_count = FILLER_TILE_COUNT * shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT
        + BATCH_TILE_COUNT;
    ASSERT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    ASSERT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    ASSERT_EQ(renderer.runtimeInfo().chunk_face_count, original_face_count);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.growth_attempt_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.before_recreate_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.actual_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.actual_capacity_high_water, INITIAL_CAPACITY);
    renderer.setDebugHudEnabled(false);
    renderer.setCamera({ .position = { 1'024.0, -20.0, 18.0 }, .angles = { .pitch_degrees = -25.0 } });
    auto const capture_installed = [&] {
        renderer.requestFrameCapture();
        auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
        for (uint32_t frame = 0U; frame < 32U && std::chrono::steady_clock::now() < deadline; ++frame) {
            if (!window.nextFrame()) {
                break;
            }
            static_cast<void>(renderer.render({}, deadline));
            if (renderer.captureState() == client::FrameCaptureState::Completed) {
                break;
            }
        }
        return renderer.takeFrameCapture();
    };
    auto const original_capture = capture_installed();
    ASSERT_TRUE(original_capture.has_value());
    uint64_t const original_draw_faces = renderer.runtimeInfo().chunk_draw_face_count;

    EXPECT_FALSE(renderer.upsertHeightTileMeshes(replacements, std::chrono::steady_clock::now()));
    EXPECT_EQ(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, original_face_count);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.pre_growth_deadline_rejection_count, 1U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.growth_attempt_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().stone_face_growth.publication_retry_count, 1U);
    EXPECT_GT(renderer.runtimeInfo().stone_face_growth.last_required_capacity, INITIAL_CAPACITY);
    auto const retained_capture = capture_installed();
    ASSERT_TRUE(retained_capture.has_value());
    EXPECT_EQ(retained_capture->rgba8, original_capture->rgba8);
    EXPECT_EQ(renderer.runtimeInfo().chunk_draw_face_count, original_draw_faces);

    ASSERT_TRUE(renderer.upsertHeightTileMeshes(replacements, std::chrono::steady_clock::now() + TIMEOUT));
    EXPECT_GT(renderer.runtimeInfo().stone_face_capacity, INITIAL_CAPACITY);
    auto const growth = renderer.runtimeInfo().stone_face_growth;
    EXPECT_EQ(growth.pre_growth_deadline_rejection_count, 1U);
    EXPECT_EQ(growth.growth_attempt_count, 1U);
    EXPECT_EQ(growth.before_recreate_count, 1U);
    EXPECT_EQ(growth.growth_success_count, 1U);
    EXPECT_EQ(growth.deferred_recreation_count, 0U);
    EXPECT_EQ(growth.recreation_exception_count, 0U);
    EXPECT_EQ(growth.rollback_count, 0U);
    EXPECT_EQ(growth.publication_retry_count, 1U);
    EXPECT_EQ(growth.last_target_capacity, renderer.runtimeInfo().stone_face_capacity);
    EXPECT_EQ(growth.actual_capacity, renderer.runtimeInfo().stone_face_capacity);
    EXPECT_EQ(growth.actual_capacity_high_water, renderer.runtimeInfo().stone_face_capacity);
    EXPECT_GE(growth.growth_work_duration, std::chrono::nanoseconds::zero());
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, FILLER_TILE_COUNT + BATCH_TILE_COUNT);
    EXPECT_EQ(
        renderer.runtimeInfo().chunk_face_count,
        (FILLER_TILE_COUNT + BATCH_TILE_COUNT) * shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT
    );
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, ReportsExactResidentFaceCountsAcrossPublicationRemovalAndLegacyReset)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr uint32_t FIRST_FACE_COUNT = 3U;
    static constexpr uint32_t SECOND_FACE_COUNT = 5U;
    static constexpr uint32_t REPLACEMENT_FACE_COUNT = 2U;
    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone resident face-count smoke" },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    auto const deadline = [] { return std::chrono::steady_clock::now() + TIMEOUT; };
    shared::HeightTileSurfaceMesh first = denseHeightTileMesh(0);
    shared::HeightTileSurfaceMesh second = denseHeightTileMesh(1);
    shared::HeightTileSurfaceMesh third = denseHeightTileMesh(2);
    first.quads.resize(FIRST_FACE_COUNT);
    second.quads.resize(SECOND_FACE_COUNT);
    third.quads.resize(REPLACEMENT_FACE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(first, deadline()));
    ASSERT_TRUE(renderer.upsertHeightTileMesh(first, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, FIRST_FACE_COUNT);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(second, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, FIRST_FACE_COUNT + SECOND_FACE_COUNT);
    first.quads.resize(REPLACEMENT_FACE_COUNT);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(first, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT + SECOND_FACE_COUNT);

    first.quads.clear();
    second.quads.resize(FIRST_FACE_COUNT);
    std::array const batch{ first, second, third };
    ASSERT_TRUE(renderer.upsertHeightTileMeshes(batch, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, FIRST_FACE_COUNT + REPLACEMENT_FACE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 2U);
    second.quads.clear();
    ASSERT_TRUE(renderer.upsertHeightTileMesh(second, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);
    EXPECT_FALSE(renderer.removeHeightTileMesh(second.coordinate));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);
    ASSERT_TRUE(renderer.removeHeightTileMesh(third.coordinate));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(third, deadline()));
    renderer.hotReload();
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);

    shared::ChunkMesher mesher;
    shared::ChunkMesh const legacy = mesher.update(shared::Chunk::makeStoneFixture({}));
    ASSERT_FALSE(legacy.faces.empty());
    renderer.setChunkMesh(legacy);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, legacy.faces.size());
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(third, deadline()));
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, REPLACEMENT_FACE_COUNT);
    std::array const legacy_batch{ legacy, legacy };
    renderer.setChunkMeshes(legacy_batch);
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, legacy.faces.size() * legacy_batch.size());
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 0U);
    renderer.setChunkMeshes({});
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(third, deadline()));
    renderer.setChunkMesh({});
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, 0U);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 0U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, RejectedDuplicateBatchDoesNotChangeResidentFaceCounts)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr uint32_t ORIGINAL_FACE_COUNT = 3U;
    core::platform::glfw::GlfwWindow window{
        { .width = 320U, .height = 240U, .title = "MinecraftClone rejected face-count smoke" },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    shared::HeightTileSurfaceMesh original = denseHeightTileMesh(0);
    original.quads.resize(ORIGINAL_FACE_COUNT);
    ASSERT_TRUE(renderer.upsertHeightTileMesh(original, std::chrono::steady_clock::now() + TIMEOUT));
    std::array const duplicate_batch{ denseHeightTileMesh(0), denseHeightTileMesh(0) };
    EXPECT_THROW(
        static_cast<void>(renderer.upsertHeightTileMeshes(duplicate_batch, std::chrono::steady_clock::now() + TIMEOUT)),
        std::invalid_argument
    );
    EXPECT_EQ(renderer.runtimeInfo().chunk_face_count, ORIGINAL_FACE_COUNT);
    EXPECT_EQ(renderer.runtimeInfo().height_tile_mesh_count, 1U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, RuntimeSceneAndHudReceiptAdvanceOnlyAfterSubmission)
{
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = 320U,
            .height = 240U,
            .title = "MinecraftClone submitted scene receipt",
        },
    };
    glfwHideWindow(window.nativeHandle());
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(window, { .require_validation = true }),
        shader_assets,
        { .require_validation = true },
    };
    renderer.setDebugHudEnabled(false);
    auto first = std::make_shared<client::TerrainLodScene>();
    first->faces = { client::StoneFaceInstance::fromHeightTileQuad({
        .z = 6,
        .direction = shared::HeightTileSurfaceDirection::PositiveZ,
    }) };
    first->groups = { { .range = { 0U, 1U }, .extent = 16U } };
    auto second = std::make_shared<client::TerrainLodScene>(*first);
    second->faces[0].z += 1;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto publish = [&](std::shared_ptr<client::TerrainLodScene const> const& scene) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (renderer.upsertTerrainLodScene(scene, deadline)) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    };
    auto submit = [&] {
        while (std::chrono::steady_clock::now() < deadline) {
            if (!window.nextFrame()) {
                return false;
            }
            if (renderer.render({}, deadline)) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(publish(first));
    EXPECT_EQ(renderer.runtimeInfo().submitted_frame_count, 0U);
    EXPECT_FALSE(renderer.runtimeInfo().terrain_lod_scene);
    ASSERT_TRUE(submit());
    auto const first_receipt = renderer.runtimeInfo();
    EXPECT_EQ(first_receipt.terrain_lod_scene, first);
    EXPECT_FALSE(first_receipt.debug_hud_enabled);

    ASSERT_TRUE(publish(second));
    renderer.setDebugHudEnabled(true);
    auto const undrawn_receipt = renderer.runtimeInfo();
    EXPECT_EQ(undrawn_receipt.submitted_frame_count, first_receipt.submitted_frame_count);
    EXPECT_EQ(undrawn_receipt.terrain_lod_scene, first);
    EXPECT_FALSE(undrawn_receipt.debug_hud_enabled);
    ASSERT_TRUE(submit());
    auto const second_receipt = renderer.runtimeInfo();
    EXPECT_GT(second_receipt.submitted_frame_count, first_receipt.submitted_frame_count);
    EXPECT_EQ(second_receipt.terrain_lod_scene, second);
    EXPECT_TRUE(second_receipt.debug_hud_enabled);
    EXPECT_EQ(first_receipt.terrain_lod_scene, first);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, FailedStoneFaceArenaGrowthRetriesWithOriginalDeadlineAndRestoresCapacity)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };
    static constexpr std::array<uint32_t, 2> EXPECTED_CAPACITIES{ 16U, INITIAL_CAPACITY };

    uint32_t capacity = INITIAL_CAPACITY;
    std::vector<uint32_t> recreated_capacities;
    std::vector<std::chrono::steady_clock::time_point> recreation_deadlines;
    recreated_capacities.reserve(EXPECTED_CAPACITIES.size());
    recreation_deadlines.reserve(EXPECTED_CAPACITIES.size());
    auto recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        recreated_capacities.push_back(capacity);
        recreation_deadlines.push_back(deadline);
        if (recreated_capacities.size() == 1U) {
            throw std::runtime_error("simulated capacity growth failure");
        }
    };

    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity,
        REQUIRED_CAPACITY,
        MAXIMUM_CAPACITY,
        FRAME_DEADLINE,
        recreate
    ));

    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(recreated_capacities, (std::vector<uint32_t>{ 16U, INITIAL_CAPACITY }));
    EXPECT_EQ(
        recreation_deadlines,
        (std::vector<std::chrono::steady_clock::time_point>{ FRAME_DEADLINE, FRAME_DEADLINE })
    );
}

TEST(RendererSmokeTest, DeferredArenaRecreationHasSeparateAttributionAndKeepsRollbackDeadline)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    std::vector<uint32_t> recreated_capacities;
    auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        recreated_capacities.push_back(capacity);
        if (capacity != INITIAL_CAPACITY) {
            throw client::detail::StoneFaceRecreationDeferred{};
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
    ));

    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(recreated_capacities, (std::vector<uint32_t>{ MAXIMUM_CAPACITY, INITIAL_CAPACITY }));
    EXPECT_EQ(diagnostics.growth_attempt_count, 1U);
    EXPECT_EQ(diagnostics.before_recreate_count, 0U);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_count, 0U);
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 0U);
    EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() }, "");
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, "");
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 0U);
    EXPECT_EQ(diagnostics.growth_success_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);
    EXPECT_EQ(diagnostics.last_required_capacity, REQUIRED_CAPACITY);
    EXPECT_EQ(diagnostics.last_target_capacity, MAXIMUM_CAPACITY);
    EXPECT_GE(diagnostics.growth_work_duration, std::chrono::nanoseconds::zero());
}

TEST(RendererSmokeTest, ArenaRecreationExceptionBeforeHookDoesNotMasqueradeAsDeferralAndCanRetry)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    bool fail_growth = true;
    auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        if (fail_growth && capacity != INITIAL_CAPACITY) {
            throw std::runtime_error("simulated native drain failure");
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 1U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);

    fail_growth = false;
    EXPECT_TRUE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, MAXIMUM_CAPACITY);
    EXPECT_EQ(diagnostics.growth_attempt_count, 2U);
    EXPECT_EQ(diagnostics.growth_success_count, 1U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);
    EXPECT_EQ(diagnostics.required_capacity_high_water, REQUIRED_CAPACITY);
}

TEST(RendererSmokeTest, ArenaRecreationExceptionAfterHookKeepsItsStageAndRollbackFailurePropagates)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        if (capacity != INITIAL_CAPACITY) {
            ++diagnostics.before_recreate_count;
            throw std::runtime_error("simulated native creation failure");
        }
        throw std::invalid_argument("simulated rollback failure");
    };
    try {
        static_cast<void>(client::detail::tryGrowStoneFaceCapacity(
            capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, FRAME_DEADLINE, recreate, &diagnostics
        ));
        FAIL() << "rollback failure must propagate";
    } catch (std::invalid_argument const& error) {
        EXPECT_EQ(std::string{ error.what() }, "simulated rollback failure");
    }
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.growth_attempt_count, 1U);
    EXPECT_EQ(diagnostics.before_recreate_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 0U);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
    EXPECT_EQ(diagnostics.growth_success_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, 1U);
    EXPECT_EQ(diagnostics.rollback_exception_count, 1U);
    EXPECT_GE(diagnostics.growth_work_duration, std::chrono::nanoseconds::zero());
}

TEST(RendererSmokeTest, FreshArenaDiagnosticsDoNotCountCapacitySatisfiedWithoutRecreationAsGrowth)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 4U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    auto const recreate = [](std::chrono::steady_clock::time_point) {
        FAIL() << "an already sufficient arena must not be recreated";
    };
    EXPECT_TRUE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.growth_attempt_count, 0U);
    EXPECT_EQ(diagnostics.growth_success_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, 0U);
    EXPECT_EQ(diagnostics.last_required_capacity, REQUIRED_CAPACITY);
    EXPECT_EQ(diagnostics.required_capacity_high_water, REQUIRED_CAPACITY);
    EXPECT_EQ(diagnostics.last_target_capacity, 0U);
    EXPECT_EQ(diagnostics.growth_work_duration, std::chrono::nanoseconds::zero());
}

TEST(RendererSmokeTest, ArenaNativeExceptionReasonsRetainFirstAndFollowOnlyChangedLastReason)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr std::array<std::string_view, 3U> REASONS{ "submitted fence", "submitted fence", "acquire fence" };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    for (std::string_view const reason : REASONS) {
        auto const recreate = [&](std::chrono::steady_clock::time_point) {
            if (capacity != INITIAL_CAPACITY) {
                throw std::runtime_error(std::string{ reason });
            }
        };
        EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
            capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
        ));
        EXPECT_EQ(capacity, INITIAL_CAPACITY);
        EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() }, REASONS.front());
        EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, reason);
    }
    EXPECT_EQ(diagnostics.recreation_exception_count, REASONS.size());
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 0U);
    EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
    EXPECT_EQ(diagnostics.rollback_count, REASONS.size());
}

TEST(RendererSmokeTest, ArenaNativeExceptionReasonsTruncateAndTerminateAfterShorterReplacements)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr uint32_t MAXIMUM_REASON_BYTES = 256U;

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    std::string reason(MAXIMUM_REASON_BYTES + 64U, 'x');
    auto const recreate = [&](std::chrono::steady_clock::time_point) {
        if (capacity != INITIAL_CAPACITY) {
            throw std::runtime_error(reason);
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(
        std::string_view{ diagnostics.first_recreation_exception_reason.data() },
        reason.substr(0U, MAXIMUM_REASON_BYTES)
    );
    EXPECT_EQ(
        std::string_view{ diagnostics.last_recreation_exception_reason.data() },
        reason.substr(0U, MAXIMUM_REASON_BYTES)
    );
    EXPECT_EQ(diagnostics.first_recreation_exception_reason.back(), '\0');
    EXPECT_EQ(diagnostics.last_recreation_exception_reason.back(), '\0');
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);

    reason = "x";
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, reason);
    EXPECT_EQ(diagnostics.last_recreation_exception_reason[1U], '\0');

    reason.clear();
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
    ));
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, reason);
    EXPECT_EQ(diagnostics.last_recreation_exception_reason.front(), '\0');
    EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() },
        std::string(MAXIMUM_REASON_BYTES, 'x'));
    EXPECT_EQ(diagnostics.recreation_exception_count, 3U);
}

TEST(RendererSmokeTest, UnknownArenaExceptionsStayCountedAndDoNotReplaceKnownEmptyFirstReason)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t REQUIRED_CAPACITY = 9U;
    static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
    static constexpr std::array<std::string_view, 2U> KNOWN_REASONS{ "", "later fence" };

    uint32_t capacity = INITIAL_CAPACITY;
    client::StoneFaceGrowthDiagnostics diagnostics;
    auto const unknown_recreate = [&](std::chrono::steady_clock::time_point) {
        if (capacity != INITIAL_CAPACITY) {
            // This deliberately exercises unknown non-std exception accounting through catch(...).
            // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
            throw uint32_t{ 1U };
        }
    };
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, unknown_recreate, &diagnostics
    ));
    EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 1U);
    for (std::string_view const reason : KNOWN_REASONS) {
        auto const recreate = [&](std::chrono::steady_clock::time_point) {
            if (capacity != INITIAL_CAPACITY) {
                throw std::runtime_error(std::string{ reason });
            }
        };
        EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
            capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, recreate, &diagnostics
        ));
    }
    EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
        capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, {}, unknown_recreate, &diagnostics
    ));
    EXPECT_EQ(capacity, INITIAL_CAPACITY);
    EXPECT_EQ(diagnostics.recreation_exception_count, 4U);
    EXPECT_EQ(diagnostics.recreation_unknown_exception_count, 2U);
    EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 4U);
    EXPECT_EQ(std::string_view{ diagnostics.first_recreation_exception_reason.data() }, "");
    EXPECT_EQ(std::string_view{ diagnostics.last_recreation_exception_reason.data() }, KNOWN_REASONS.back());
    EXPECT_EQ(diagnostics.rollback_count, 4U);
}

TEST(RendererSmokeTest, PendingArenaDrainGuardsAcquireAndSubmitUntilNativeRecreationCompletesWithoutNewUploads)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr uint32_t LARGER_TARGET_CAPACITY = 32U;
    static constexpr auto FRAME_BUDGET = std::chrono::milliseconds{ 8 };
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};
    static constexpr auto FRAME_DEADLINE = STARTED_AT + FRAME_BUDGET;

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    uint32_t actual_capacity = INITIAL_CAPACITY;
    fixture.backend->recreation_wait_result = core::graphics::vulkan::FenceWaitResult::TimedOut;
    auto const grow = [&](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
        EXPECT_EQ(deadline, FRAME_DEADLINE);
        try {
            fixture.context->recreate(fixture.context->info().extent, {}, FRAME_BUDGET);
            actual_capacity = target;
        } catch (...) {
            EXPECT_EQ(pending.recordFailure(std::current_exception(), target, false, STARTED_AT),
                client::detail::StoneFaceRecreationFailure::DrainPending);
        }
        return actual_capacity;
    };
    EXPECT_EQ(grow(TARGET_CAPACITY, FRAME_DEADLINE), INITIAL_CAPACITY);
    pending.coalesce(LARGER_TARGET_CAPACITY);
    pending.coalesce(TARGET_CAPACITY);
    EXPECT_EQ(pending.targetCapacity(), LARGER_TARGET_CAPACITY);

    EXPECT_FALSE(pending.render(actual_capacity, FRAME_DEADLINE, grow, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
    EXPECT_EQ(actual_capacity, INITIAL_CAPACITY);

    fixture.backend->recreation_wait_result = core::graphics::vulkan::FenceWaitResult::Completed;
    EXPECT_TRUE(pending.render(actual_capacity, FRAME_DEADLINE, grow, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(actual_capacity, LARGER_TARGET_CAPACITY);
    EXPECT_EQ(pending.targetCapacity(), 0U);
    EXPECT_EQ(fixture.backend->acquire_count, 1U);
    EXPECT_EQ(fixture.backend->submit_count, 1U);
    EXPECT_EQ(fixture.backend->recreation_wait_budgets, (std::vector<std::chrono::nanoseconds>(3U, FRAME_BUDGET)));
}

TEST(RendererSmokeTest, PendingArenaDrainRequiresExactCoreFenceErrorAndOldResourcesBeforeTheHook)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};
    static constexpr std::string_view RECREATION_TIMEOUT_REASON = "Vulkan presentation recreation fence wait timed out";
    static constexpr std::string_view PRESENT_TIMEOUT_REASON = "Vulkan presentation present fence wait timed out";

    std::array<std::exception_ptr, 2U> const timeouts{
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{ std::string{ RECREATION_TIMEOUT_REASON } }),
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{ std::string{ PRESENT_TIMEOUT_REASON } }),
    };
    std::array<std::exception_ptr, 4U> const nonmatching_failures{
        std::make_exception_ptr(std::runtime_error{ std::string{ PRESENT_TIMEOUT_REASON } }),
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{
            "Vulkan presentation acquire fence wait timed out"
        }),
        std::make_exception_ptr(core::graphics::vulkan::VulkanError{
            "Vulkan presentation present fence wait timed out with extra detail"
        }),
        std::make_exception_ptr(uint32_t{ 1U }),
    };
    for (std::exception_ptr const& timeout : timeouts) {
        client::detail::PendingStoneFaceGrowth pending;
        EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, false, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::DrainPending);
        EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, true, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::Failed);
        EXPECT_EQ(pending.targetCapacity(), 0U);
    }
    for (std::exception_ptr const& failure : nonmatching_failures) {
        PendingGrowthPresentationFixture fixture;
        client::detail::PendingStoneFaceGrowth pending;
        EXPECT_EQ(pending.recordFailure(timeouts.front(), TARGET_CAPACITY, false, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::DrainPending);
        EXPECT_EQ(pending.recordFailure(failure, TARGET_CAPACITY, false, STARTED_AT),
            client::detail::StoneFaceRecreationFailure::Failed);
        EXPECT_TRUE(pending.render(INITIAL_CAPACITY, STARTED_AT,
            [](uint32_t, std::chrono::steady_clock::time_point) {
                ADD_FAILURE() << "failure must clear latch";
                return 0U;
            },
            STARTED_AT, [&] { return fixture.present(); }));
        EXPECT_EQ(fixture.backend->acquire_count, 1U);
        EXPECT_EQ(fixture.backend->submit_count, 1U);
    }
}

TEST(RendererSmokeTest, FrameBoundedPresentationRecreationDefersOnlyDrainTimeouts)
{
    static constexpr auto FRAME_DEADLINE = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{ 123'456 }
    };
    static constexpr std::array<std::string_view, 2U> RETRYABLE_REASONS{
        "Vulkan presentation recreation fence wait timed out",
        "Vulkan presentation present fence wait timed out",
    };

    for (std::string_view const reason : RETRYABLE_REASONS) {
        std::chrono::steady_clock::time_point observed_deadline{};
        auto const recreate = [&](std::chrono::steady_clock::time_point const deadline) {
            observed_deadline = deadline;
            throw core::graphics::vulkan::VulkanError{ std::string{ reason } };
        };
        EXPECT_FALSE(client::detail::tryRecreatePresentationWithinDeadline(FRAME_DEADLINE, recreate));
        EXPECT_EQ(observed_deadline, FRAME_DEADLINE);
    }

    auto const failed_recreation = [](std::chrono::steady_clock::time_point const) {
        throw core::graphics::vulkan::VulkanError{ "Vulkan presentation recreation lost the device" };
    };
    EXPECT_THROW(
        static_cast<void>(client::detail::tryRecreatePresentationWithinDeadline(FRAME_DEADLINE, failed_recreation)),
        core::graphics::vulkan::VulkanError
    );
}

TEST(RendererSmokeTest, NormalArenaDeferralRetainsTargetUntilTheNextFrame)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation present fence wait timed out" }), TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    EXPECT_FALSE(pending.render(INITIAL_CAPACITY, STARTED_AT,
        [&](uint32_t, std::chrono::steady_clock::time_point) {
            EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(client::detail::StoneFaceRecreationDeferred{}),
                TARGET_CAPACITY, false, STARTED_AT), client::detail::StoneFaceRecreationFailure::Deferred);
            return INITIAL_CAPACITY;
        }, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), TARGET_CAPACITY);
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
    EXPECT_TRUE(pending.render(INITIAL_CAPACITY, STARTED_AT + std::chrono::milliseconds{ 8 },
        [](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
            EXPECT_EQ(target, TARGET_CAPACITY);
            EXPECT_EQ(deadline, STARTED_AT + std::chrono::milliseconds{ 8 });
            return target;
        }, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), 0U);
}

TEST(RendererSmokeTest, ExhaustedUploadDeadlineRetainsGrowthForAFreshFrameBudget)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto FRAME_BUDGET = std::chrono::milliseconds{ 8 };
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    pending.coalesce(TARGET_CAPACITY, STARTED_AT);
    EXPECT_EQ(pending.targetCapacity(), TARGET_CAPACITY);
    auto const next_frame = STARTED_AT + FRAME_BUDGET;
    bool grew = false;
    EXPECT_TRUE(pending.render(INITIAL_CAPACITY, next_frame + FRAME_BUDGET,
        [&](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
            EXPECT_EQ(target, TARGET_CAPACITY);
            EXPECT_EQ(deadline, next_frame + FRAME_BUDGET);
            grew = true;
            return target;
        }, next_frame, [&] { return fixture.present(); }));
    EXPECT_TRUE(grew);
    EXPECT_EQ(pending.targetCapacity(), 0U);
}

TEST(RendererSmokeTest, PendingArenaRollbackFailurePropagatesBeforeAnyNativeAcquire)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" }), TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    try {
        static_cast<void>(pending.render(INITIAL_CAPACITY, STARTED_AT,
            [](uint32_t, std::chrono::steady_clock::time_point) -> uint32_t {
                throw std::invalid_argument("rollback failed");
            }, STARTED_AT, [&] { return fixture.present(); }));
        FAIL() << "rollback failure must propagate";
    } catch (std::invalid_argument const& error) {
        EXPECT_EQ(std::string_view{ error.what() }, "rollback failed");
    }
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
}

TEST(RendererSmokeTest, PendingArenaAgeIsFiniteAndNotRenewedByCoalescingOrTimeoutRetries)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr uint32_t LARGER_TARGET_CAPACITY = 32U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};
    static constexpr auto FRAME_DEADLINE = STARTED_AT + std::chrono::milliseconds{ 8 };
    static constexpr auto EXPIRES_AT = STARTED_AT + std::chrono::seconds{ 5 };

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    std::exception_ptr const recreation_timeout = std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" });
    std::exception_ptr const present_timeout = std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation present fence wait timed out" });
    EXPECT_EQ(pending.recordFailure(recreation_timeout, TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    pending.coalesce(LARGER_TARGET_CAPACITY);
    EXPECT_FALSE(pending.render(INITIAL_CAPACITY, FRAME_DEADLINE,
        [&](uint32_t const target, std::chrono::steady_clock::time_point const deadline) {
            EXPECT_EQ(deadline, FRAME_DEADLINE);
            EXPECT_EQ(pending.recordFailure(present_timeout, target, false, EXPIRES_AT - std::chrono::nanoseconds{ 1 }),
                client::detail::StoneFaceRecreationFailure::DrainPending);
            return INITIAL_CAPACITY;
        }, EXPIRES_AT - std::chrono::nanoseconds{ 1 }, [&] { return fixture.present(); }));
    try {
        static_cast<void>(pending.render(INITIAL_CAPACITY, FRAME_DEADLINE,
            [](uint32_t, std::chrono::steady_clock::time_point) {
                ADD_FAILURE() << "expired drain must not retry";
                return 0U;
            },
            EXPIRES_AT, [&] { return fixture.present(); }));
        FAIL() << "pending drain must surface its finite horizon";
    } catch (std::runtime_error const& error) {
        EXPECT_EQ(std::string_view{ error.what() },
            "stone face arena growth exceeded its pending drain horizon: actual=8, target=32, "
            "reason=Vulkan presentation present fence wait timed out");
    }
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
}

TEST(RendererSmokeTest, ResetOrAlreadyInstalledActualCapacityCancelsPendingArenaGrowth)
{
    static constexpr uint32_t INITIAL_CAPACITY = 8U;
    static constexpr uint32_t TARGET_CAPACITY = 16U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    std::exception_ptr const timeout = std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" });
    auto const unexpected_growth = [](uint32_t, std::chrono::steady_clock::time_point) {
        ADD_FAILURE() << "cancelled or installed growth must not retry";
        return 0U;
    };
    EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    pending.clear();
    EXPECT_TRUE(pending.render(INITIAL_CAPACITY, STARTED_AT, unexpected_growth, STARTED_AT,
        [&] { return fixture.present(); }));
    EXPECT_EQ(pending.recordFailure(timeout, TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    EXPECT_TRUE(pending.render(TARGET_CAPACITY, STARTED_AT, unexpected_growth, STARTED_AT,
        [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), 0U);
    EXPECT_EQ(fixture.backend->acquire_count, 2U);
    EXPECT_EQ(fixture.backend->submit_count, 2U);
}

TEST(RendererSmokeTest, SmallerExternalArenaCapacityDoesNotClearTheLargerPendingTarget)
{
    static constexpr uint32_t SMALLER_CAPACITY = 16U;
    static constexpr uint32_t TARGET_CAPACITY = 32U;
    static constexpr auto STARTED_AT = std::chrono::steady_clock::time_point{};

    PendingGrowthPresentationFixture fixture;
    client::detail::PendingStoneFaceGrowth pending;
    EXPECT_EQ(pending.recordFailure(std::make_exception_ptr(core::graphics::vulkan::VulkanError{
        "Vulkan presentation recreation fence wait timed out" }), TARGET_CAPACITY, false, STARTED_AT),
        client::detail::StoneFaceRecreationFailure::DrainPending);
    EXPECT_FALSE(pending.render(SMALLER_CAPACITY, STARTED_AT,
        [](uint32_t const target, std::chrono::steady_clock::time_point) {
            EXPECT_EQ(target, TARGET_CAPACITY);
            return SMALLER_CAPACITY;
        }, STARTED_AT, [&] { return fixture.present(); }));
    EXPECT_EQ(pending.targetCapacity(), TARGET_CAPACITY);
    EXPECT_EQ(fixture.backend->acquire_count, 0U);
    EXPECT_EQ(fixture.backend->submit_count, 0U);
}

TEST(RendererSmokeTest, RejectsASelectedTransformThatRequiresClientCompensation)
{
    auto context = core::graphics::vulkan::PresentationContext::createForTesting(
        {
            .extent = { .width = 1280U, .height = 720U },
            .surface_transform = {
                .pre_transform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
                .requires_client_orientation_compensation = true,
            },
        },
        std::make_unique<InertPresentationBackend>()
    );
    client::InstalledShaderAssets const shader_assets;

    EXPECT_THROW(
        static_cast<void>(client::VulkanRenderer{ std::move(context), shader_assets }),
        std::runtime_error
    );
}

TEST(RendererSmokeTest, GlfwInputAdapterPreservesPressAndRelease)
{
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = 64U,
            .height = 64U,
            .title = "MinecraftClone input adapter",
        },
    };
    EXPECT_FALSE(window.keyPressed(core::platform::glfw::WindowKey::W));
    window.injectKeyForTesting(core::platform::glfw::WindowKey::W, true);
    EXPECT_TRUE(window.keyPressed(core::platform::glfw::WindowKey::W));
    window.injectKeyForTesting(core::platform::glfw::WindowKey::W, false);
    EXPECT_FALSE(window.keyPressed(core::platform::glfw::WindowKey::W));
}

TEST(RendererSmokeTest, GlfwCursorCaptureSupportsContinuousCameraLook)
{
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = 64U,
            .height = 64U,
            .title = "MinecraftClone cursor capture",
        },
    };
    glfwSetInputMode(window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    EXPECT_EQ(glfwGetInputMode(window.nativeHandle(), GLFW_CURSOR), GLFW_CURSOR_DISABLED);
    glfwSetInputMode(window.nativeHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    EXPECT_EQ(glfwGetInputMode(window.nativeHandle(), GLFW_CURSOR), GLFW_CURSOR_NORMAL);
}

TEST(RendererSmokeTest, NormalRendererDisplaysCanonicalTerrainTextures)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone canonical terrain textures",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(false);
    client::Camera const capture_camera{
        { .position = { 16.0, -32.0, 420.0 }, .angles = { .pitch_degrees = -20.0 } },
    };
    renderer.setCamera(capture_camera.pose());
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    shared::HeightTileSurfaceMesh mesh = materialPaletteDisplayMesh();
    ASSERT_FALSE(mesh.quads.empty());
    ASSERT_TRUE(renderer.upsertHeightTileMesh(mesh, deadline));
    ASSERT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
    renderer.requestFrameCapture();

    client::RendererFrameCapture capture;
    bool captured = false;
    for (uint32_t frame = 0U;
         frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
         ++frame) {
        ASSERT_TRUE(window.nextFrame());
        static_cast<void>(renderer.render({}, deadline));
        if (std::optional<client::RendererFrameCapture> completed = renderer.takeFrameCapture(); completed.has_value()) {
            capture = std::move(*completed);
            captured = true;
            break;
        }
    }

    ASSERT_TRUE(captured);
    ASSERT_GT(capture.width, 0U);
    ASSERT_GT(capture.height, 0U);
    if (char const* const capture_path = std::getenv("MC_TERRAIN_MATERIAL_CAPTURE_PATH");
        capture_path != nullptr && capture_path[0] != '\0') {
        auto const written = testsupport::writePpm(
            {
                .width = capture.width,
                .height = capture.height,
                .srgb_encoded = capture.srgb_encoded,
                .pixels = capture.rgba8,
            },
            std::filesystem::path{ capture_path },
            "MC-AI-0499 normal renderer canonical terrain textures"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
    }
    std::optional<PixelRectangle> const ice_panel_bounds = projectedIcePanelBounds(
        capture_camera,
        capture.width,
        capture.height
    );
    ASSERT_TRUE(ice_panel_bounds.has_value());
    TerrainMaterialColorClasses const classes = classifyTerrainMaterialColors(capture, *ice_panel_bounds);
    EXPECT_TRUE(classes.complete()) << "capture lacks: " << classes.missingClasses();
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, GrassBlockUsesVanillaTopSideAndBottomTextures)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone vanilla grass face orientation",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(false);
    shared::ChunkMesh const mesh{
        .coordinate = { .x = 0, .y = 0, .z = 0 },
        .content_identity = { .revision = 1U, .content_hash = 1U },
        .faces = {
            {
                .local_origin = { .x = 0U, .y = 0U, .z = 0U },
                .direction = shared::FaceDirection::NegativeY,
                .material = shared::Block::Grass,
            },
            {
                .local_origin = { .x = 2U, .y = 0U, .z = 0U },
                .direction = shared::FaceDirection::PositiveZ,
                .material = shared::Block::Grass,
            },
            {
                .local_origin = { .x = 4U, .y = 0U, .z = 0U },
                .direction = shared::FaceDirection::NegativeZ,
                .material = shared::Block::Grass,
            },
        },
    };
    renderer.setChunkMesh(mesh);
    client::Camera const upper_camera{
        { .position = { 2.5, -5.0, 2.5 }, .angles = { .pitch_degrees = -20.0 } },
    };
    client::Camera const lower_camera{
        { .position = { 2.5, -5.0, -2.5 }, .angles = { .pitch_degrees = 20.0 } },
    };
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto captureWithCamera = [&](client::Camera const& camera) -> std::optional<client::RendererFrameCapture> {
        renderer.setCamera(camera.pose());
        renderer.requestFrameCapture();
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            if (!window.nextFrame()) {
                return std::nullopt;
            }
            static_cast<void>(renderer.render({}, deadline));
            if (std::optional<client::RendererFrameCapture> capture = renderer.takeFrameCapture();
                capture.has_value()) {
                return capture;
            }
        }
        return std::nullopt;
    };

    std::optional<client::RendererFrameCapture> const upper_capture = captureWithCamera(upper_camera);
    ASSERT_TRUE(upper_capture.has_value());
    char const* const capture_prefix = std::getenv("MC_GRASS_FACE_CAPTURE_PREFIX");
    auto write_face_capture = [capture_prefix](
        client::RendererFrameCapture const& capture,
        char const* const suffix
    ) {
        if (capture_prefix == nullptr || capture_prefix[0] == '\0') {
            return;
        }
        std::string output_path{ capture_prefix };
        output_path += suffix;
        output_path += ".ppm";
        auto const written = testsupport::writePpm(
            {
                .width = capture.width,
                .height = capture.height,
                .srgb_encoded = true,
                .pixels = capture.rgba8,
            },
            std::filesystem::path{ output_path },
            "vanilla grass top and side face orientation"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
    };
    write_face_capture(*upper_capture, "-upper");
    std::optional<std::array<uint8_t, 4U>> const grass_side_top = capturePixelAtWorld(
        *upper_capture,
        upper_camera,
        { 0.53F, 0.0F, 0.9F }
    );
    std::optional<std::array<uint8_t, 4U>> const grass_side_bottom = capturePixelAtWorld(
        *upper_capture,
        upper_camera,
        { 0.53F, 0.0F, 0.1F }
    );
    std::optional<std::array<uint8_t, 4U>> const grass_top = capturePixelAtWorld(
        *upper_capture,
        upper_camera,
        { 2.53F, 0.53F, 1.0F }
    );
    ASSERT_TRUE(grass_side_top.has_value());
    ASSERT_TRUE(grass_side_bottom.has_value());
    ASSERT_TRUE(grass_top.has_value());
    static constexpr std::array<uint8_t, 3U> GRASS_TOP_SRGB{ 100U, 132U, 60U };
    static constexpr std::array<uint8_t, 3U> GRASS_SIDE_TOP_SRGB{ 87U, 115U, 52U };
    auto expectSrgbColor = [](
        std::array<uint8_t, 4U> const& actual,
        std::array<uint8_t, 3U> const& expected,
        char const* const label
    ) {
        for (size_t channel = 0U; channel < expected.size(); ++channel) {
            EXPECT_NEAR(actual[channel], expected[channel], 2U) << label << " channel " << channel;
        }
    };
    expectSrgbColor(*grass_top, GRASS_TOP_SRGB, "pinned vanilla grass top texel after Plains tint");
    expectSrgbColor(*grass_side_top, GRASS_SIDE_TOP_SRGB, "pinned vanilla grass side texel after overlay tint and shade");
    EXPECT_GT(static_cast<int>((*grass_side_top)[1U]), static_cast<int>((*grass_side_top)[0U]) + 10);
    EXPECT_GT(static_cast<int>((*grass_side_top)[1U]), static_cast<int>((*grass_side_top)[2U]) + 10);
    EXPECT_GT(static_cast<int>((*grass_side_bottom)[0U]), static_cast<int>((*grass_side_bottom)[1U]) + 10);
    EXPECT_GT(static_cast<int>((*grass_side_bottom)[1U]), static_cast<int>((*grass_side_bottom)[2U]) + 10);
    EXPECT_GT(static_cast<int>((*grass_top)[1U]), static_cast<int>((*grass_top)[0U]) + 10);
    EXPECT_GT(static_cast<int>((*grass_top)[1U]), static_cast<int>((*grass_top)[2U]) + 10);

    std::optional<client::RendererFrameCapture> const lower_capture = captureWithCamera(lower_camera);
    ASSERT_TRUE(lower_capture.has_value());
    write_face_capture(*lower_capture, "-lower");
    std::optional<std::array<uint8_t, 4U>> const grass_bottom = capturePixelAtWorld(
        *lower_capture,
        lower_camera,
        { 4.53F, 0.53F, 0.0F }
    );
    ASSERT_TRUE(grass_bottom.has_value());
    EXPECT_GT(static_cast<uint32_t>((*grass_bottom)[0U]) * 10U,
              static_cast<uint32_t>((*grass_bottom)[1U]) * 13U);
    EXPECT_GT(static_cast<uint32_t>((*grass_bottom)[1U]) * 10U,
              static_cast<uint32_t>((*grass_bottom)[2U]) * 13U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, NormalRendererDisplaysAspectFittedMapPixelsAndGuiOverlay)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAP_WIDTH = 4U;
    static constexpr uint32_t MAP_HEIGHT = 2U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr std::array<
        std::array<uint8_t, 4>,
        static_cast<size_t>(MAP_WIDTH) * MAP_HEIGHT
    > MAP_PIXELS{
        std::array<uint8_t, 4>{ 255U, 0U, 0U, 255U },
        std::array<uint8_t, 4>{ 0U, 255U, 0U, 255U },
        std::array<uint8_t, 4>{ 0U, 0U, 255U, 255U },
        std::array<uint8_t, 4>{ 255U, 255U, 0U, 255U },
        std::array<uint8_t, 4>{ 0U, 255U, 255U, 255U },
        std::array<uint8_t, 4>{ 255U, 0U, 255U, 255U },
        std::array<uint8_t, 4>{ 255U, 255U, 255U, 255U },
        std::array<uint8_t, 4>{ 255U, 128U, 0U, 255U },
    };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone loading map capture",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(false);
    renderer.setFrameCompletionTracking(true);
    renderer.addGuiText(
        "MAP READY",
        client::TextPlacement{ .position = { 8.0F, 100.0F, 0.0F } }
    );
    ASSERT_TRUE(renderer.setMapImage(MAP_WIDTH, MAP_HEIGHT, MAP_PIXELS));
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    auto captureFrame = [&]() -> std::optional<client::RendererFrameCapture> {
        renderer.requestFrameCapture();
        for (uint32_t frame = 0U;
             frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
             ++frame) {
            if (!window.nextFrame()) {
                return std::nullopt;
            }
            static_cast<void>(renderer.render({}, deadline));
            if (std::optional<client::RendererFrameCapture> capture = renderer.takeFrameCapture();
                capture.has_value()) {
                return capture;
            }
        }
        return std::nullopt;
    };
    auto const capture = captureFrame();
    ASSERT_TRUE(capture.has_value());
    ASSERT_GT(capture->width, 0U);
    ASSERT_GT(capture->height, 0U);
    client::RendererFrameProgress const frame_progress = renderer.frameProgress();
    ASSERT_TRUE(frame_progress.submitted.has_value());
    ASSERT_TRUE(frame_progress.completed.has_value());
    EXPECT_EQ(frame_progress.completed->sequence, frame_progress.submitted->sequence);
    EXPECT_EQ(
        frame_progress.completed->swapchain_generation,
        frame_progress.submitted->swapchain_generation
    );
    ASSERT_EQ(
        capture->rgba8.size(),
        static_cast<uint64_t>(capture->width) * capture->height * 4U
    );

    auto pixelAt = [&capture](uint32_t const x, uint32_t const y) {
        size_t const offset = (static_cast<size_t>(y) * capture->width + x) * 4U;
        return std::array<uint8_t, 4>{
            capture->rgba8[offset],
            capture->rgba8[offset + 1U],
            capture->rgba8[offset + 2U],
            capture->rgba8[offset + 3U],
        };
    };
    double const scale = std::min(
        static_cast<double>(capture->width) / MAP_WIDTH,
        static_cast<double>(capture->height) / MAP_HEIGHT
    );
    double const fitted_width = MAP_WIDTH * scale;
    double const fitted_height = MAP_HEIGHT * scale;
    double const origin_x = (capture->width - fitted_width) * 0.5;
    double const origin_y = (capture->height - fitted_height) * 0.5;
    auto expectedCaptureChannel = [srgb_encoded = capture->srgb_encoded](uint8_t const source) {
        if (srgb_encoded || source == 255U) {
            return source;
        }
        float const encoded = static_cast<float>(source) / 255.0F;
        float const linear = encoded <= 0.04045F
            ? encoded / 12.92F
            : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
        return static_cast<uint8_t>(std::lround(linear * 255.0F));
    };
    auto expectMapPixel = [&](uint32_t const source_x, uint32_t const source_y,
                              uint32_t const sample_x, uint32_t const sample_y) {
        std::array<uint8_t, 4> const actual = pixelAt(sample_x, sample_y);
        std::array<uint8_t, 4> const source = MAP_PIXELS[source_y * MAP_WIDTH + source_x];
        for (size_t channel = 0U; channel < source.size(); ++channel) {
            EXPECT_NEAR(actual[channel], expectedCaptureChannel(source[channel]), 12U)
                << "map pixel (" << source_x << ", " << source_y << "), channel " << channel
                << " at viewport (" << sample_x << ", " << sample_y << ")";
        }
    };
    for (uint32_t y = 0U; y < MAP_HEIGHT; ++y) {
        for (uint32_t x = 0U; x < MAP_WIDTH; ++x) {
            uint32_t const sample_x = static_cast<uint32_t>(
                std::floor(origin_x + (static_cast<double>(x) + 0.5) * scale - 0.5)
            );
            uint32_t const sample_y = static_cast<uint32_t>(
                std::floor(origin_y + (static_cast<double>(y) + 0.5) * scale - 0.5)
            );
            expectMapPixel(x, y, sample_x, sample_y);

            uint32_t const near_edge_x = static_cast<uint32_t>(std::floor(origin_x + x * scale + 1.0));
            uint32_t const near_edge_y = static_cast<uint32_t>(std::floor(origin_y + y * scale + 1.0));
            expectMapPixel(x, y, near_edge_x, near_edge_y);
        }
    }

    double const letterbox_x = origin_x > 0.0 ? origin_x * 0.5 : capture->width * 0.5;
    double const letterbox_y = origin_y > 0.0 ? origin_y * 0.5 : capture->height * 0.5;
    std::array<uint8_t, 4> const letterbox = pixelAt(
        static_cast<uint32_t>(letterbox_x),
        static_cast<uint32_t>(letterbox_y)
    );
    auto encodeChannel = [srgb_encoded = capture->srgb_encoded](float const linear) {
        float encoded = linear;
        if (srgb_encoded) {
            encoded = linear <= 0.003'130'8F
                ? linear * 12.92F
                : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
        }
        return static_cast<uint8_t>(std::lround(encoded * 255.0F));
    };
    static constexpr std::array<float, 3> LETTERBOX_LINEAR{ 0.035F, 0.043F, 0.055F };
    for (size_t channel = 0U; channel < LETTERBOX_LINEAR.size(); ++channel) {
        EXPECT_NEAR(letterbox[channel], encodeChannel(LETTERBOX_LINEAR[channel]), 8U);
    }

    uint32_t const text_pixel_count = [&] {
        uint32_t count = 0U;
        for (uint32_t y = 0U; y < std::min(capture->height, 140U); ++y) {
            for (uint32_t x = 0U; x < std::min(capture->width, 120U); ++x) {
                std::array<uint8_t, 4> const pixel = pixelAt(x, y);
                if (pixel[0] > 220U && pixel[1] > 220U && pixel[2] > 220U) {
                    ++count;
                }
            }
        }
        return count;
    }();
    EXPECT_GE(text_pixel_count, 20U);
    if (char const* const capture_path = std::getenv("MC_MAP_IMAGE_CAPTURE_PATH");
        capture_path != nullptr && capture_path[0] != '\0') {
        auto const written = testsupport::writePpm(
            {
                .width = capture->width,
                .height = capture->height,
                .srgb_encoded = capture->srgb_encoded,
                .pixels = capture->rgba8,
            },
            std::filesystem::path{ capture_path },
            "loading map RGBA8 pixels, aspect fit and GUI text overlay"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
    }

    renderer.clearMapImage();
    auto const cleared_capture = captureFrame();
    ASSERT_TRUE(cleared_capture.has_value());
    ASSERT_EQ(cleared_capture->width, capture->width);
    ASSERT_EQ(cleared_capture->height, capture->height);
    size_t const first_pixel_center_x = static_cast<size_t>(std::floor(origin_x + 0.5 * scale - 0.5));
    size_t const first_pixel_center_y = static_cast<size_t>(std::floor(origin_y + 0.5 * scale - 0.5));
    size_t const first_pixel_offset = (first_pixel_center_y * cleared_capture->width + first_pixel_center_x) * 4U;
    EXPECT_FALSE(
        cleared_capture->rgba8[first_pixel_offset] > 220U
        && cleared_capture->rgba8[first_pixel_offset + 1U] < 30U
        && cleared_capture->rgba8[first_pixel_offset + 2U] < 30U
    );
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RendererSmokeTest, ActiveThirdPersonRendererShowsPlatformPlayersAndTopLeftHud)
{
    static constexpr uint32_t LOGICAL_WIDTH = 320U;
    static constexpr uint32_t LOGICAL_HEIGHT = 240U;
    static constexpr uint32_t MAX_CAPTURE_FRAMES = 12U;
    static constexpr auto TIMEOUT = std::chrono::seconds{ 10 };
    static constexpr shared::Player LOCAL{
        .id = 1U,
        .x = 8U,
        .y = 8U,
        .ch = '@',
    };
    static constexpr shared::Player REMOTE{
        .id = 2U,
        .x = 12U,
        .y = 15U,
        .ch = '#',
    };
    core::platform::glfw::GlfwWindow window{
        core::platform::glfw::WindowDescriptor{
            .width = LOGICAL_WIDTH,
            .height = LOGICAL_HEIGHT,
            .title = "MinecraftClone active third-person renderer",
        },
    };
    client::InstalledShaderAssets const shader_assets;
    client::VulkanRenderer renderer{
        client::VulkanRenderer::createPresentationContext(
            window,
            { .require_validation = true, .enable_frame_capture = true }
        ),
        shader_assets,
        { .require_validation = true, .enable_frame_capture = true },
    };
    renderer.setDebugHudEnabled(true);
    std::array<client::PlayerRenderData, 2U> const players{
        client::PlayerRenderData{
            .x = LOCAL.x,
            .y = LOCAL.y,
            .color = { 1.0F, 0.0F, 0.0F, 1.0F },
        },
        client::PlayerRenderData{
            .x = REMOTE.x,
            .y = REMOTE.y,
            .color = { 0.0F, 1.0F, 0.0F, 1.0F },
        },
    };
    EXPECT_FALSE(client::shouldRenderRemotePlayer(LOCAL, LOCAL.ch));
    EXPECT_TRUE(client::shouldRenderRemotePlayer(REMOTE, LOCAL.ch));
    client::Camera const camera{
        {
            .position = client::localPlayerThirdPersonPose(
                LOCAL,
                { .pitch_degrees = -10.0 }
            ).position,
            .angles = { .pitch_degrees = -10.0 },
        },
    };
    renderer.setCamera(camera.pose());
    renderer.requestFrameCapture();

    client::RendererFrameCapture capture;
    bool captured = false;
    auto const deadline = std::chrono::steady_clock::now() + TIMEOUT;
    for (uint32_t frame = 0U;
         frame < MAX_CAPTURE_FRAMES && std::chrono::steady_clock::now() < deadline;
         ++frame) {
        ASSERT_TRUE(window.nextFrame());
        static_cast<void>(renderer.render(
            players,
            client::DebugHudInput{ .player_x = static_cast<float>(LOCAL.x), .player_y = static_cast<float>(LOCAL.y) },
            1.0F,
            deadline
        ));
        if (std::optional<client::RendererFrameCapture> completed = renderer.takeFrameCapture(); completed.has_value()) {
            capture = std::move(*completed);
            captured = true;
            break;
        }
    }

    ASSERT_TRUE(captured);
    if (char const* const capture_path = std::getenv("MC_RENDERER_SMOKE_CAPTURE_PATH");
        capture_path != nullptr && capture_path[0] != '\0') {
        auto const written = testsupport::writePpm(
            {
                .width = capture.width,
                .height = capture.height,
                .srgb_encoded = capture.srgb_encoded,
                .pixels = capture.rgba8,
            },
            std::filesystem::path{ capture_path },
            "MC-AI-0118 normal close third-person HUD smoke capture"
        );
        ASSERT_TRUE(written.has_value()) << written.error();
    }
    CaptureColorClasses const classes = classifyCaptureColors(capture);
    EXPECT_TRUE(classes.has_platform);
    EXPECT_TRUE(classes.has_platform_side);
    EXPECT_TRUE(classes.has_sky);
    EXPECT_TRUE(classes.has_debug_hud);
    EXPECT_TRUE(classes.has_red_player);
    EXPECT_TRUE(classes.has_green_player);
    EXPECT_EQ(renderer.runtimeInfo().pipeline_path, client::RendererPipelinePath::Vertex);
}

} // namespace
