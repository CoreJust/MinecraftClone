#include <client/Camera.hpp>
#include <client/PlayerPresentation.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/render/DebugHud.hpp>
#include <client/render/DepthFormat.hpp>
#include <client/render/GuiRenderer.hpp>
#include <client/render/HeightTileDrawIndex.hpp>
#include <client/render/HeightTileInstanceReaper.hpp>
#include <client/render/MapImageData.hpp>
#include <client/render/PresentationFrameProgress.hpp>
#include <client/render/ShaderAssets.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/TerrainLodUploadPlan.hpp>
#include <client/render/TerrainTextures.hpp>
#include <client/render/TextRenderer.hpp>
#include <client/render/VulkanFrustum.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <generation/Generation.hpp>
#include <core/common/Assert.hpp>
#include <core/common/Version.hpp>
#include <core/graphics/Graphics.hpp>
#include <core/graphics/vulkan/Vulkan.hpp>
#if defined(__ANDROID__)
#include <core/graphics/vulkan/android/AndroidSurface.hpp>
#else
#include <core/graphics/vulkan/glfw/GlfwSurface.hpp>
#endif
#include <core/IO/Log.hpp>
#include <core/kernel/Program.hpp>
#include <core/kernel/Spirv.hpp>

#if !defined(__ANDROID__)
#include <core/platform/glfw/GlfwWindow.hpp>
#endif

#include <glm/common.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace client {
namespace {

using StoneDrawRange = StoneIndirectDraws::Range;

void appendVisibleStoneRange(std::vector<StoneDrawRange>& ranges, StoneDrawRange const range)
{
    if (!ranges.empty()
        && ranges.back().first_instance
            + ranges.back().instance_count == range.first_instance) {
        ranges.back().instance_count += range.instance_count;
    } else if (range.instance_count != 0U) {
        ranges.push_back(range);
    }
}

using MapImageData = detail::MapImageData;

class DiagnosticDurationScope final {
public:
    DiagnosticDurationScope() = default;

    void start(std::chrono::nanoseconds* const duration)
    {
        finish();
        m_duration = duration;
        if (m_duration != nullptr) {
            m_started_at = std::chrono::steady_clock::now();
        }
    }

    void finish()
    {
        if (m_duration != nullptr) {
            *m_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_started_at
            );
            m_duration = nullptr;
        }
    }

    ~DiagnosticDurationScope() { finish(); }

    DiagnosticDurationScope(DiagnosticDurationScope const&) = delete;
    DiagnosticDurationScope& operator=(DiagnosticDurationScope const&) = delete;

private:
    std::chrono::nanoseconds* m_duration = nullptr;
    std::chrono::steady_clock::time_point m_started_at;
};

template <typename Value>
[[nodiscard]] Value& requiredValue(std::optional<Value>& value) noexcept
{
    if (!value.has_value()) {
        std::terminate();
    }
    return *value;
}

template <typename Value>
[[nodiscard]] Value const& requiredValue(std::optional<Value> const& value) noexcept
{
    if (!value.has_value()) {
        std::terminate();
    }
    return *value;
}

#if (!defined(__ANDROID__) && defined(_CORE_DEBUG)) || defined(_MC_VK_VALIDATION_LAYERS)
constexpr bool REQUIRE_VALIDATION = true;
#else
constexpr bool REQUIRE_VALIDATION = false;
#endif

constexpr uint32_t GRID_WORKGROUPS_X = 32U;
constexpr uint32_t GRID_WORKGROUPS_Y = 32U;
constexpr uint32_t KERNEL_CACHE_CAPACITY = 32U;
constexpr uint32_t QUAD_VERTEX_COUNT = 6U;
constexpr uint32_t GRID_VERTEX_COUNT = 30U;
constexpr uint32_t BOX_VERTEX_COUNT = 36U;
constexpr uint32_t STONE_FACE_VERTEX_COUNT = 6U;
constexpr uint32_t MAXIMUM_TEXT_GLYPH_COUNT = 16'384U;
constexpr size_t MAXIMUM_TEXT_UPDATE_GLYPHS = 65'536U / sizeof(TextGlyph);
constexpr uint32_t MAP_IMAGE_UPLOAD_SLOT_COUNT = 3U;
constexpr VkDeviceSize MAP_IMAGE_UPLOAD_BYTE_COUNT =
    static_cast<VkDeviceSize>(MapImageData::MAXIMUM_DIMENSION)
    * MapImageData::MAXIMUM_DIMENSION
    * sizeof(MapImageData::Pixel);
constexpr uint32_t MAXIMUM_RENDERED_STONE_FACE_COUNT =
    shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT * shared::HEIGHT_TILE_INTEREST_COUNT;
constexpr uint32_t INITIAL_RENDERED_STONE_FACE_CAPACITY = 65'536U;
constexpr std::array DEPTH_FORMAT_CANDIDATES{
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D16_UNORM,
};

struct alignas(16) GridPushConstants final {
    glm::mat4 projection_view{ 1.0F };
    float world_size = 32.0F;
    float line_width = 0.025F;
    float pad0 = 0.0F;
    float pad1 = 0.0F;
    std::array<float, 4> line_color{ 0.11F, 0.12F, 0.14F, 1.0F };
    std::array<float, 4> surface_color{ 0.30F, 0.34F, 0.39F, 1.0F };
};
static_assert(sizeof(GridPushConstants) == 112U);

struct alignas(16) BoxPushConstants final {
    glm::mat4 projection_view{ 1.0F };
    std::array<float, 4> origin{ 0.0F, 0.0F, 0.0F, 0.0F };
    std::array<float, 4> extent{ 2.0F, 2.0F, 2.0F, 0.0F };
    std::array<float, 4> color{ 1.0F, 1.0F, 1.0F, 1.0F };
    glm::vec4 curvature{};
};
static_assert(sizeof(BoxPushConstants) == 128U);

struct alignas(16) TextPushConstants final {
    glm::mat4 projection_view{ 1.0F };
    std::array<float, 2> resolution{ 0.0F, 0.0F };
    float scale = 1.0F;
    uint32_t mode = 0U;
    glm::vec4 curvature_camera{};
};
static_assert(sizeof(TextPushConstants) == 96U);

struct MapImagePushConstants final {
    std::array<uint32_t, 2> viewport_size{};
    std::array<uint32_t, 2> image_size{};
    uint32_t encode_srgb = 0U;
};
static_assert(sizeof(MapImagePushConstants) == 20U);

void checkResult(VkResult const result, char const* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string{ operation } + " failed with Vulkan result " + std::to_string(result));
    }
}

void checkIndirectAllocationResult(VkResult const result, char const* const operation)
{
    if (result != VK_SUCCESS) {
        throw StoneIndirectDraws::AllocationError(result, operation);
    }
}

[[nodiscard]]
std::optional<uint32_t> findHostVisibleCoherentMemoryType(
    VkPhysicalDevice const physical_device,
    uint32_t const memory_type_bits
) noexcept
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    std::optional<uint32_t> fallback;
    VkMemoryPropertyFlags const required_flags =
        static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        | static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        if ((memory_type_bits & (1U << index)) == 0U
            || (properties.memoryTypes[index].propertyFlags & required_flags) != required_flags) {
            continue;
        }
        if ((properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            return index;
        }
        if (!fallback.has_value()) {
            fallback = index;
        }
    }
    return fallback;
}

struct alignas(16) StonePushConstants final {
    glm::mat4 projection_view{ 1.0F };
    std::array<int32_t, 4> world_origin{ 0, 0, 0, 0 };
    // camera_fog.z enables manual sRGB encoding for UNORM presentation attachments.
    glm::vec4 camera_fog{ 0.0F, 0.0F, 0.0F, 720.0F };
    glm::vec4 curvature{};
    glm::vec4 surface{};
};
static_assert(sizeof(StonePushConstants) == 128U);

[[nodiscard]]
std::vector<StoneFaceInstance> stoneFaceInstances(shared::ChunkMesh const& mesh)
{
    if (mesh.faces.size() > shared::ChunkMesh::MAXIMUM_FACE_COUNT) {
        throw std::invalid_argument("chunk mesh exceeds its face limit");
    }
    std::vector<StoneFaceInstance> instances;
    instances.reserve(mesh.faces.size());
    for (shared::MeshFace const& face : mesh.faces) {
        instances.emplace_back(StoneFaceInstance::fromChunkFace(mesh.coordinate, face));
    }
    return instances;
}

[[nodiscard]]
std::vector<StoneFaceInstance> stoneFaceInstances(shared::HeightTileSurfaceMesh const& mesh)
{
    if (mesh.quads.size() > shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT) {
        throw std::invalid_argument("height tile mesh exceeds its quad limit");
    }
    std::vector<StoneFaceInstance> instances;
    std::array<uint32_t, 6> next_instance{};
    std::vector<StoneFaceInstance> source_instances;
    source_instances.reserve(mesh.quads.size());
    for (shared::HeightTileSurfaceQuad const& quad : mesh.quads) {
        StoneFaceInstance const instance = StoneFaceInstance::fromHeightTileQuad(quad);
        ++next_instance[instance.direction];
        source_instances.push_back(instance);
    }
    uint32_t first_instance = 0U;
    for (uint32_t& next : next_instance) {
        uint32_t const count = next;
        next = first_instance;
        first_instance += count;
    }
    instances.resize(mesh.quads.size());
    for (StoneFaceInstance const& instance : source_instances) {
        instances[next_instance[instance.direction]++] = instance;
    }
    return instances;
}

class StoneFaceBuffer final {
public:
    StoneFaceBuffer(
        VkDevice const device,
        VkPhysicalDevice const physical_device,
        uint32_t const capacity
    )
        : m_device(device)
        , m_physical_device(physical_device)
        , m_capacity(capacity)
    {
        if (m_capacity == 0U || m_capacity > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
            throw std::invalid_argument("stone face buffer capacity is invalid");
        }
        try {
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = byteSize();
            buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkResult(vkCreateBuffer(m_device, &buffer_info, nullptr, &m_buffer), "vkCreateBuffer stone faces");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits);
            checkResult(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory stone faces");
            checkResult(vkBindBufferMemory(m_device, m_buffer, m_memory, 0U), "vkBindBufferMemory stone faces");
            checkResult(vkMapMemory(m_device, m_memory, 0U, VK_WHOLE_SIZE, 0U, &m_mapped), "vkMapMemory stone faces");
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~StoneFaceBuffer()
    {
        reset();
    }

    StoneFaceBuffer(StoneFaceBuffer const&) = delete;
    StoneFaceBuffer& operator=(StoneFaceBuffer const&) = delete;

    void upload(uint32_t const first_instance, std::span<StoneFaceInstance const> const instances)
    {
        if (first_instance > m_capacity || instances.size() > m_capacity - first_instance) {
            throw std::invalid_argument("stone face upload exceeds its buffer capacity");
        }
        if (instances.empty()) {
            return;
        }
        VkDeviceSize const offset = static_cast<VkDeviceSize>(first_instance) * sizeof(StoneFaceInstance);
        std::memcpy(static_cast<std::byte*>(m_mapped) + offset, instances.data(), instances.size_bytes());
    }

    [[nodiscard]] VkDescriptorBufferInfo descriptor() const noexcept
    {
        return {
            .buffer = m_buffer,
            .offset = 0U,
            .range = byteSize(),
        };
    }

    [[nodiscard]] uint32_t capacity() const noexcept { return m_capacity; }

    void destroy() noexcept
    {
        reset();
    }

private:
    [[nodiscard]] uint32_t memoryType(uint32_t const memory_type_bits) const
    {
        if (std::optional<uint32_t> const memory_type =
                findHostVisibleCoherentMemoryType(m_physical_device, memory_type_bits)) {
            return *memory_type;
        }
        throw std::runtime_error("Vulkan device has no coherent host-visible memory for stone faces");
    }

    [[nodiscard]] VkDeviceSize byteSize() const noexcept
    {
        return static_cast<VkDeviceSize>(m_capacity)
            * static_cast<VkDeviceSize>(sizeof(StoneFaceInstance));
    }

    void reset() noexcept
    {
        if (m_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            if (m_mapped != nullptr) {
                vkUnmapMemory(m_device, m_memory);
            }
            vkFreeMemory(m_device, m_memory, nullptr);
        }
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_mapped = nullptr;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    uint32_t m_capacity = 0U;
};

[[nodiscard]]
std::optional<std::string> environmentVariable(char const* const name)
{
#if defined(_MSC_VER)
    char* raw_value = nullptr;
    size_t value_size = 0U;
    auto const status = _dupenv_s(&raw_value, &value_size, name);
    std::unique_ptr<char, decltype(&std::free)> const value{ raw_value, &std::free };
    if (status != 0) {
        throw std::runtime_error("failed to read environment variable");
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{ value.get() };
#else
    char const* const value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{ value };
#endif
}

[[nodiscard]]
StoneIndirectDraws::QuadMode diagnosticStoneQuadMode()
{
    std::optional<std::string> const requested = environmentVariable("MC_DIAGNOSTIC_INDEXED_STONE_QUADS");
    if (requested == "0") {
        return StoneIndirectDraws::QuadMode::SixVertex;
    }
    if (requested == "1") {
        return StoneIndirectDraws::QuadMode::IndexedInstanced;
    }
    if (!requested.has_value() || *requested == "2") {
        return StoneIndirectDraws::QuadMode::IndexedBlock;
    }
    throw std::invalid_argument("MC_DIAGNOSTIC_INDEXED_STONE_QUADS must be 0, 1, or 2");
}

[[nodiscard]]
bool diagnosticDirectionalStoneCulling()
{
    std::optional<std::string> const requested = environmentVariable("MC_DIAGNOSTIC_DIRECTIONAL_STONE_CULLING");
    return !requested.has_value() || *requested != "0";
}

[[nodiscard]]
std::string_view stoneVertexShaderName(StoneIndirectDraws::QuadMode const quad_mode)
{
    std::optional<std::string> const requested = environmentVariable("MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER");
    if (!requested.has_value() || *requested == "0") {
        return "stone.vert.spv";
    }
    if (*requested != "1") {
        throw std::invalid_argument("MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER must be 0 or 1");
    }
    if (quad_mode != StoneIndirectDraws::QuadMode::IndexedBlock) {
        throw std::invalid_argument("specialized stone shader requires indexed-block draw mode");
    }
    return "stone_indexed_block.vert.spv";
}

[[nodiscard]]
bool diagnosticPresentationStages()
{
    std::optional<std::string> const requested = environmentVariable("MC_DIAGNOSTIC_PRESENTATION_STAGES");
    if (!requested.has_value() || *requested == "0") {
        return false;
    }
    if (*requested == "1") {
        return true;
    }
    throw std::invalid_argument("MC_DIAGNOSTIC_PRESENTATION_STAGES must be 0 or 1");
}

enum class StoneDrawBufferKind : uint8_t {
    Indirect,
    IndexedIndirect,
    QuadIndices,
};

class VulkanStoneDrawBuffer final : public StoneIndirectDraws::Buffer {
public:
    VulkanStoneDrawBuffer(
        VkDevice const device,
        VkPhysicalDevice const physical_device,
        uint32_t const command_count,
        StoneDrawBufferKind const kind = StoneDrawBufferKind::Indirect
    )
        : m_device(device)
        , m_capacity(command_count)
        , m_kind(kind)
    {
        if (command_count == 0U || command_count > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
            throw std::invalid_argument("stone indirect command count is invalid");
        }
        try {
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = m_kind == StoneDrawBufferKind::QuadIndices
                ? static_cast<VkDeviceSize>(StoneIndirectDraws::INDEX_COUNT_PER_BLOCK) * sizeof(uint16_t)
                : static_cast<VkDeviceSize>(m_capacity) * (m_kind == StoneDrawBufferKind::IndexedIndirect
                    ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand));
            buffer_info.usage = m_kind == StoneDrawBufferKind::QuadIndices
                ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkIndirectAllocationResult(
                vkCreateBuffer(m_device, &buffer_info, nullptr, &m_buffer), "vkCreateBuffer stone indirect"
            );
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
            std::optional<uint32_t> const memory_type =
                findHostVisibleCoherentMemoryType(physical_device, requirements.memoryTypeBits);
            if (!memory_type.has_value()) {
                throw StoneIndirectDraws::AllocationError(
                    VK_ERROR_MEMORY_MAP_FAILED, "coherent host-visible memory selection for stone indirect"
                );
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = *memory_type;
            checkIndirectAllocationResult(
                vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory stone indirect"
            );
            checkIndirectAllocationResult(
                vkBindBufferMemory(m_device, m_buffer, m_memory, 0U), "vkBindBufferMemory stone indirect"
            );
            checkIndirectAllocationResult(
                vkMapMemory(m_device, m_memory, 0U, VK_WHOLE_SIZE, 0U, &m_mapped), "vkMapMemory stone indirect"
            );
            if (m_kind == StoneDrawBufferKind::QuadIndices) {
                auto const& indices = StoneIndirectDraws::quadIndexBlock();
                std::memcpy(m_mapped, indices.data(), sizeof(indices));
            }
        } catch (...) {
            reset();
            throw;
        }
    }

    ~VulkanStoneDrawBuffer() override
    {
        reset();
    }

    VulkanStoneDrawBuffer(VulkanStoneDrawBuffer const&) = delete;
    VulkanStoneDrawBuffer& operator=(VulkanStoneDrawBuffer const&) = delete;

    [[nodiscard]] uint32_t capacity() const noexcept override { return m_capacity; }
    [[nodiscard]] VkBuffer handle() const noexcept { return m_buffer; }

    [[nodiscard]] std::span<VkDrawIndirectCommand> mappedCommands() override
    {
        return { static_cast<VkDrawIndirectCommand*>(m_mapped), m_capacity };
    }

    [[nodiscard]] std::span<VkDrawIndexedIndirectCommand> mappedIndexedCommands() override
    {
        return { static_cast<VkDrawIndexedIndirectCommand*>(m_mapped), m_capacity };
    }

    void record(
        VkCommandBuffer const command,
        VkDeviceSize const offset,
        uint32_t const draw_count
    ) const override
    {
        if (m_kind == StoneDrawBufferKind::IndexedIndirect) {
            vkCmdDrawIndexedIndirect(command, m_buffer, offset, draw_count, sizeof(VkDrawIndexedIndirectCommand));
        } else {
            vkCmdDrawIndirect(command, m_buffer, offset, draw_count, sizeof(VkDrawIndirectCommand));
        }
    }

private:
    void reset() noexcept
    {
        if (m_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            if (m_mapped != nullptr) {
                vkUnmapMemory(m_device, m_memory);
            }
            vkFreeMemory(m_device, m_memory, nullptr);
        }
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_mapped = nullptr;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    uint32_t m_capacity = 0U;
    StoneDrawBufferKind m_kind = StoneDrawBufferKind::Indirect;
};

class StoneTextureBuffer final {
public:
    StoneTextureBuffer(VkDevice const device, VkPhysicalDevice const physical_device)
        : m_device(device)
        , m_physical_device(physical_device)
    {
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(m_physical_device, &device_properties);
        uint64_t const texel_count = static_cast<uint64_t>(render::terrain_textures::TILE_COUNT)
            * render::terrain_textures::PIXELS_PER_TILE;
        if (texel_count > device_properties.limits.maxTexelBufferElements) {
            throw std::runtime_error("Vulkan device cannot expose the terrain texture atlas as a texel buffer");
        }
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = sizeof(render::terrain_textures::RGBA8);
        buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkResult(vkCreateBuffer(m_device, &buffer_info, nullptr, &m_buffer), "vkCreateBuffer terrain textures");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(m_physical_device, &properties);
        uint32_t memory_type = properties.memoryTypeCount;
        for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
            VkMemoryPropertyFlags constexpr required =
                static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                | static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if ((requirements.memoryTypeBits & (1U << index)) != 0U
                && (properties.memoryTypes[index].propertyFlags & required) == required) {
                memory_type = index;
                break;
            }
        }
        if (memory_type == properties.memoryTypeCount) {
            reset();
            throw std::runtime_error("Vulkan device has no coherent host-visible memory for terrain textures");
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        checkResult(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory terrain textures");
        checkResult(vkBindBufferMemory(m_device, m_buffer, m_memory, 0U), "vkBindBufferMemory terrain textures");
        void* mapped = nullptr;
        checkResult(vkMapMemory(m_device, m_memory, 0U, sizeof(render::terrain_textures::RGBA8), 0U, &mapped),
            "vkMapMemory terrain textures");
        std::memcpy(mapped, render::terrain_textures::RGBA8.data(), sizeof(render::terrain_textures::RGBA8));
        vkUnmapMemory(m_device, m_memory);
        VkBufferViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        view_info.buffer = m_buffer;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.range = sizeof(render::terrain_textures::RGBA8);
        checkResult(vkCreateBufferView(m_device, &view_info, nullptr, &m_view),
            "vkCreateBufferView terrain textures");
    }

    ~StoneTextureBuffer() { reset(); }
    StoneTextureBuffer(StoneTextureBuffer const&) = delete;
    StoneTextureBuffer& operator=(StoneTextureBuffer const&) = delete;

    [[nodiscard]] VkBufferView const& view() const noexcept { return m_view; }
    void destroy() noexcept { reset(); }

private:
    void reset() noexcept
    {
        if (m_view != VK_NULL_HANDLE) vkDestroyBufferView(m_device, m_view, nullptr);
        if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
        if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
        m_view = VK_NULL_HANDLE;
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkBufferView m_view = VK_NULL_HANDLE;
};

class CanonicalTerrainFieldBuffer final {
public:
    CanonicalTerrainFieldBuffer(
        VkDevice const device,
        VkPhysicalDevice const physical_device,
        std::shared_ptr<generation::GlobalAtlas const> atlas = nullptr
    )
        : m_device(device), m_atlas(std::move(atlas))
    {
        struct Header final {
            uint32_t resolution, entry_count;
            float sea_level;
            uint32_t enabled;
        };
        static_assert(sizeof(Header) == 16U);
        uint32_t const resolution = m_atlas ? m_atlas->configuration.resolution : 1U;
        uint64_t const count = static_cast<uint64_t>(resolution) * resolution;
        if (m_atlas && ((resolution != 256U && resolution != 512U)
            || m_atlas->temperature.size() != count || m_atlas->elevation.size() != count
            || !std::isfinite(m_atlas->sea_level))) {
            throw std::invalid_argument("Canonical terrain shading atlas is incomplete");
        }
        std::vector<std::byte> data(sizeof(Header) + count * sizeof(std::array<float, 2>));
        Header const header{resolution, static_cast<uint32_t>(count), m_atlas ? m_atlas->sea_level : 0.0F,
            m_atlas ? 1U : 0U};
        std::memcpy(data.data(), &header, sizeof(header));
        if (m_atlas) {
            for (size_t i = 0U; i < count; ++i) {
                std::array<float, 2> const value{m_atlas->temperature[i], m_atlas->elevation[i]};
                if (!std::isfinite(value[0]) || !std::isfinite(value[1])) {
                    throw std::invalid_argument("Canonical terrain shading atlas contains nonfinite values");
                }
                std::memcpy(data.data() + sizeof(Header) + i * sizeof(value), value.data(), sizeof(value));
            }
        }
        VkPhysicalDeviceProperties limits{};
        vkGetPhysicalDeviceProperties(physical_device, &limits);
        if (data.size() > limits.limits.maxStorageBufferRange) {
            throw std::runtime_error("Canonical terrain shading atlas exceeds Vulkan storage buffer range");
        }
        try {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = data.size();
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkResult(vkCreateBuffer(device, &info, nullptr, &m_buffer), "vkCreateBuffer canonical terrain");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, m_buffer, &requirements);
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
            uint32_t memory_type = properties.memoryTypeCount;
            for (uint32_t i = 0U; i < properties.memoryTypeCount; ++i) {
                VkMemoryPropertyFlags constexpr REQUIRED_MEMORY = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if ((requirements.memoryTypeBits & (1U << i)) != 0U
                    && (properties.memoryTypes[i].propertyFlags & REQUIRED_MEMORY) == REQUIRED_MEMORY) {
                    memory_type = i;
                    break;
                }
            }
            if (memory_type == properties.memoryTypeCount) {
                throw std::runtime_error("No coherent memory for canonical terrain shading");
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type;
            checkResult(vkAllocateMemory(device, &allocation, nullptr, &m_memory), "vkAllocateMemory canonical terrain");
            checkResult(vkBindBufferMemory(device, m_buffer, m_memory, 0U), "vkBindBufferMemory canonical terrain");
            void* mapped = nullptr;
            checkResult(vkMapMemory(device, m_memory, 0U, data.size(), 0U, &mapped), "vkMapMemory canonical terrain");
            std::memcpy(mapped, data.data(), data.size());
            vkUnmapMemory(device, m_memory);
            m_size = data.size();
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~CanonicalTerrainFieldBuffer() { destroy(); }
    CanonicalTerrainFieldBuffer(CanonicalTerrainFieldBuffer const&) = delete;
    CanonicalTerrainFieldBuffer& operator=(CanonicalTerrainFieldBuffer const&) = delete;
    [[nodiscard]] auto const& atlas() const noexcept { return m_atlas; }
    [[nodiscard]] VkDescriptorBufferInfo descriptor() const noexcept { return {m_buffer, 0U, m_size}; }
    void destroy() noexcept
    {
        if (m_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_memory, nullptr);
        }
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }
private:
    VkDevice m_device;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkDeviceSize m_size = 0U;
    std::shared_ptr<generation::GlobalAtlas const> m_atlas;
};

class StoneDescriptorSet final {
public:
    StoneDescriptorSet(
        VkDevice const device,
        StoneFaceBuffer const& buffer,
        StoneTextureBuffer const& texture,
        CanonicalTerrainFieldBuffer const& field
    )
        : m_device(device)
    {
        try {
            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            bindings[0].binding = 0U;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1U;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            bindings[1].binding = 1U;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            bindings[1].descriptorCount = 1U;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[2].binding = 2U;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[2].descriptorCount = 1U;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
            layout_info.pBindings = bindings.data();
            checkResult(
                vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_layout),
                "vkCreateDescriptorSetLayout stone faces"
            );
            std::array<VkDescriptorPoolSize, 2> pool_sizes{
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2U},
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1U},
            };
            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = 1U;
            pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
            pool_info.pPoolSizes = pool_sizes.data();
            checkResult(
                vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_pool),
                "vkCreateDescriptorPool stone faces"
            );
            VkDescriptorSetAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = m_pool;
            allocate_info.descriptorSetCount = 1U;
            allocate_info.pSetLayouts = &m_layout;
            checkResult(
                vkAllocateDescriptorSets(m_device, &allocate_info, &m_set),
                "vkAllocateDescriptorSets stone faces"
            );
            VkDescriptorBufferInfo const buffer_info = buffer.descriptor();
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_set;
            write.dstBinding = 0U;
            write.descriptorCount = 1U;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer_info;
            VkWriteDescriptorSet texture_write{};
            texture_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            texture_write.dstSet = m_set;
            texture_write.dstBinding = 1U;
            texture_write.descriptorCount = 1U;
            texture_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            texture_write.pTexelBufferView = &texture.view();
            VkDescriptorBufferInfo const field_info = field.descriptor();
            VkWriteDescriptorSet field_write{};
            field_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            field_write.dstSet = m_set;
            field_write.dstBinding = 2U;
            field_write.descriptorCount = 1U;
            field_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            field_write.pBufferInfo = &field_info;
            std::array<VkWriteDescriptorSet, 3> const writes{write, texture_write, field_write};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0U, nullptr);
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    void setField(CanonicalTerrainFieldBuffer const& field)
    {
        VkDescriptorBufferInfo const info = field.descriptor();
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = 2U;
        write.descriptorCount = 1U;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(m_device, 1U, &write, 0U, nullptr);
    }

    ~StoneDescriptorSet()
    {
        reset();
    }

    StoneDescriptorSet(StoneDescriptorSet const&) = delete;
    StoneDescriptorSet& operator=(StoneDescriptorSet const&) = delete;

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    [[nodiscard]] VkDescriptorSet set() const noexcept { return m_set; }

    void destroy() noexcept
    {
        reset();
    }

private:
    void reset() noexcept
    {
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        }
        if (m_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
        }
        m_pool = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
        m_set = VK_NULL_HANDLE;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

class MapImageUploadBuffer final {
public:
    MapImageUploadBuffer(VkDevice const device, VkPhysicalDevice const physical_device)
        : m_device(device)
        , m_physical_device(physical_device)
    {
        try {
            VkBufferCreateInfo const buffer_info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0U,
                .size = MAP_IMAGE_UPLOAD_BYTE_COUNT,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0U,
                .pQueueFamilyIndices = nullptr,
            };
            checkResult(vkCreateBuffer(m_device, &buffer_info, nullptr, &m_buffer), "vkCreateBuffer map upload");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
            std::optional<uint32_t> const memory_type = findHostVisibleCoherentMemoryType(
                m_physical_device,
                requirements.memoryTypeBits
            );
            if (!memory_type.has_value()) {
                throw std::runtime_error("Vulkan device has no coherent host-visible memory for map uploads");
            }
            VkMemoryAllocateInfo const allocation{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = nullptr,
                .allocationSize = requirements.size,
                .memoryTypeIndex = *memory_type,
            };
            checkResult(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory map upload");
            checkResult(vkBindBufferMemory(m_device, m_buffer, m_memory, 0U), "vkBindBufferMemory map upload");
            checkResult(vkMapMemory(m_device, m_memory, 0U, VK_WHOLE_SIZE, 0U, &m_mapped), "vkMapMemory map upload");
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~MapImageUploadBuffer()
    {
        reset();
    }

    MapImageUploadBuffer(MapImageUploadBuffer const&) = delete;
    MapImageUploadBuffer& operator=(MapImageUploadBuffer const&) = delete;

    void copy(std::span<MapImageData::Pixel const> const pixels) const
    {
        if (pixels.empty() || pixels.size_bytes() > MAP_IMAGE_UPLOAD_BYTE_COUNT) {
            throw std::invalid_argument("map upload exceeds its fixed staging buffer");
        }
        std::memcpy(m_mapped, pixels.data(), pixels.size_bytes());
    }

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer; }

private:
    void reset() noexcept
    {
        if (m_mapped != nullptr) {
            vkUnmapMemory(m_device, m_memory);
        }
        if (m_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_memory, nullptr);
        }
        m_mapped = nullptr;
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
};

class MapImageTexture final {
public:
    MapImageTexture(VkDevice const device, VkPhysicalDevice const physical_device)
        : m_device(device)
        , m_physical_device(physical_device)
    {
        try {
            createImage();
            createSampler();
            createDescriptors();
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~MapImageTexture()
    {
        reset();
    }

    MapImageTexture(MapImageTexture const&) = delete;
    MapImageTexture& operator=(MapImageTexture const&) = delete;

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    [[nodiscard]] VkDescriptorSet set() const noexcept { return m_set; }

    [[nodiscard]] VkImageLayout recordUpload(
        VkCommandBuffer const command,
        uint32_t const frame_slot,
        MapImageData const& data,
        VkImageLayout const previous_layout
    )
    {
        if (frame_slot >= MAP_IMAGE_UPLOAD_SLOT_COUNT || data.empty()) {
            throw std::invalid_argument("map upload has an invalid frame slot or empty pixels");
        }
        std::unique_ptr<MapImageUploadBuffer>& upload_buffer = m_upload_buffers[frame_slot];
        if (!upload_buffer) {
            upload_buffer = std::make_unique<MapImageUploadBuffer>(m_device, m_physical_device);
        }
        upload_buffer->copy(data.pixels());

        VkImageMemoryBarrier to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer.srcAccessMask = previous_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0U : VK_ACCESS_SHADER_READ_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = previous_layout;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = m_image;
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1U;
        to_transfer.subresourceRange.layerCount = 1U;
        vkCmdPipelineBarrier(
            command,
            previous_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0U,
            0U,
            nullptr,
            0U,
            nullptr,
            1U,
            &to_transfer
        );

        VkBufferImageCopy const copy{
            .bufferOffset = 0U,
            .bufferRowLength = 0U,
            .bufferImageHeight = 0U,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0U,
                .baseArrayLayer = 0U,
                .layerCount = 1U,
            },
            .imageOffset = { .x = 0, .y = 0, .z = 0 },
            .imageExtent = { .width = data.width(), .height = data.height(), .depth = 1U },
        };
        vkCmdCopyBufferToImage(
            command,
            upload_buffer->buffer(),
            m_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1U,
            &copy
        );

        VkImageMemoryBarrier to_shader{};
        to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader.image = m_image;
        to_shader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_shader.subresourceRange.levelCount = 1U;
        to_shader.subresourceRange.layerCount = 1U;
        vkCmdPipelineBarrier(
            command,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0U,
            0U,
            nullptr,
            0U,
            nullptr,
            1U,
            &to_shader
        );
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

private:
    void createImage()
    {
        VkImageCreateInfo const image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0U,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_SRGB,
            .extent = {
                .width = MapImageData::MAXIMUM_DIMENSION,
                .height = MapImageData::MAXIMUM_DIMENSION,
                .depth = 1U,
            },
            .mipLevels = 1U,
            .arrayLayers = 1U,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0U,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        checkResult(vkCreateImage(m_device, &image_info, nullptr, &m_image), "vkCreateImage map");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, m_image, &requirements);
        VkPhysicalDeviceMemoryProperties const properties = [&] {
            VkPhysicalDeviceMemoryProperties memory_properties{};
            vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memory_properties);
            return memory_properties;
        }();
        std::optional<uint32_t> memory_type;
        for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
            if ((requirements.memoryTypeBits & (1U << index)) != 0U
                && (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                memory_type = index;
                break;
            }
        }
        if (!memory_type.has_value()) {
            throw std::runtime_error("Vulkan device has no device-local memory for map texture");
        }
        VkMemoryAllocateInfo const allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = *memory_type,
        };
        checkResult(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory map");
        checkResult(vkBindImageMemory(m_device, m_image, m_memory, 0U), "vkBindImageMemory map");
        VkImageViewCreateInfo const view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0U,
            .image = m_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_SRGB,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0U,
                .levelCount = 1U,
                .baseArrayLayer = 0U,
                .layerCount = 1U,
            },
        };
        checkResult(vkCreateImageView(m_device, &view_info, nullptr, &m_view), "vkCreateImageView map");
    }

    void createSampler()
    {
        VkSamplerCreateInfo const sampler_info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0U,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0F,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0F,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_NEVER,
            .minLod = 0.0F,
            .maxLod = 0.0F,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        checkResult(vkCreateSampler(m_device, &sampler_info, nullptr, &m_sampler), "vkCreateSampler map");
    }

    void createDescriptors()
    {
        VkDescriptorSetLayoutBinding const binding{
            .binding = 0U,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1U,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr,
        };
        VkDescriptorSetLayoutCreateInfo const layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0U,
            .bindingCount = 1U,
            .pBindings = &binding,
        };
        checkResult(
            vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_layout),
            "vkCreateDescriptorSetLayout map"
        );
        VkDescriptorPoolSize const pool_size{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1U,
        };
        VkDescriptorPoolCreateInfo const pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0U,
            .maxSets = 1U,
            .poolSizeCount = 1U,
            .pPoolSizes = &pool_size,
        };
        checkResult(vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_pool), "vkCreateDescriptorPool map");
        VkDescriptorSetAllocateInfo const allocate_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = nullptr,
            .descriptorPool = m_pool,
            .descriptorSetCount = 1U,
            .pSetLayouts = &m_layout,
        };
        checkResult(vkAllocateDescriptorSets(m_device, &allocate_info, &m_set), "vkAllocateDescriptorSets map");
        VkDescriptorImageInfo const image_info{
            .sampler = m_sampler,
            .imageView = m_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet const write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = m_set,
            .dstBinding = 0U,
            .dstArrayElement = 0U,
            .descriptorCount = 1U,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
        vkUpdateDescriptorSets(m_device, 1U, &write, 0U, nullptr);
    }

    void reset() noexcept
    {
        for (std::unique_ptr<MapImageUploadBuffer>& upload_buffer : m_upload_buffers) {
            upload_buffer.reset();
        }
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        }
        if (m_sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_sampler, nullptr);
        }
        if (m_view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_view, nullptr);
        }
        if (m_image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_image, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_memory, nullptr);
        }
        if (m_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
        }
        m_pool = VK_NULL_HANDLE;
        m_sampler = VK_NULL_HANDLE;
        m_view = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
        m_set = VK_NULL_HANDLE;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    std::array<std::unique_ptr<MapImageUploadBuffer>, MAP_IMAGE_UPLOAD_SLOT_COUNT> m_upload_buffers{};
};

class TextGlyphBuffer final {
public:
    TextGlyphBuffer(VkDevice const device, VkPhysicalDevice const physical_device)
        : m_device(device)
        , m_physical_device(physical_device)
    {
        try {
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = byteSize();
            buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkResult(vkCreateBuffer(m_device, &buffer_info, nullptr, &m_buffer), "vkCreateBuffer text glyphs");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits);
            checkResult(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory text glyphs");
            checkResult(vkBindBufferMemory(m_device, m_buffer, m_memory, 0U), "vkBindBufferMemory text glyphs");
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~TextGlyphBuffer()
    {
        reset();
    }

    TextGlyphBuffer(TextGlyphBuffer const&) = delete;
    TextGlyphBuffer& operator=(TextGlyphBuffer const&) = delete;

    [[nodiscard]] VkDescriptorBufferInfo descriptor() const noexcept
    {
        return {
            .buffer = m_buffer,
            .offset = 0U,
            .range = byteSize(),
        };
    }

    void update(VkCommandBuffer const command, std::span<TextGlyph const> const glyphs) const
    {
        detail::recordTextGlyphUpload(command, m_buffer, glyphs, vkCmdPipelineBarrier, vkCmdUpdateBuffer);
    }

    void destroy() noexcept
    {
        reset();
    }

private:
    [[nodiscard]] uint32_t memoryType(uint32_t const memory_type_bits) const
    {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(m_physical_device, &properties);
        for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
            if ((memory_type_bits & (1U << index)) != 0U
                && (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                return index;
            }
        }
        throw std::runtime_error("Vulkan device has no local memory for text glyphs");
    }

    [[nodiscard]] static constexpr VkDeviceSize byteSize() noexcept
    {
        return static_cast<VkDeviceSize>(MAXIMUM_TEXT_GLYPH_COUNT) * sizeof(TextGlyph);
    }

    void reset() noexcept
    {
        if (m_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_memory, nullptr);
        }
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
};

class TextDescriptorSet final {
public:
    TextDescriptorSet(VkDevice const device, TextGlyphBuffer const& buffer)
        : m_device(device)
    {
        try {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0U;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 1U;
            binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = 1U;
            layout_info.pBindings = &binding;
            checkResult(
                vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_layout),
                "vkCreateDescriptorSetLayout text glyphs"
            );
            VkDescriptorPoolSize pool_size{};
            pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            pool_size.descriptorCount = 1U;
            VkDescriptorPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_info.maxSets = 1U;
            pool_info.poolSizeCount = 1U;
            pool_info.pPoolSizes = &pool_size;
            checkResult(
                vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_pool),
                "vkCreateDescriptorPool text glyphs"
            );
            VkDescriptorSetAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate_info.descriptorPool = m_pool;
            allocate_info.descriptorSetCount = 1U;
            allocate_info.pSetLayouts = &m_layout;
            checkResult(vkAllocateDescriptorSets(m_device, &allocate_info, &m_set),
                "vkAllocateDescriptorSets text glyphs");
            VkDescriptorBufferInfo const buffer_info = buffer.descriptor();
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_set;
            write.dstBinding = 0U;
            write.descriptorCount = 1U;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer_info;
            vkUpdateDescriptorSets(m_device, 1U, &write, 0U, nullptr);
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~TextDescriptorSet()
    {
        reset();
    }

    TextDescriptorSet(TextDescriptorSet const&) = delete;
    TextDescriptorSet& operator=(TextDescriptorSet const&) = delete;

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    [[nodiscard]] VkDescriptorSet set() const noexcept { return m_set; }

    void destroy() noexcept
    {
        reset();
    }

private:
    void reset() noexcept
    {
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        }
        if (m_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
        }
        m_pool = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
        m_set = VK_NULL_HANDLE;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

[[nodiscard]]
std::chrono::nanoseconds remaining(std::chrono::steady_clock::time_point const deadline)
{
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::seconds{ 5 };
    }
    auto const duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now()
    );
    return std::max(duration, std::chrono::nanoseconds::zero());
}

[[nodiscard]]
RendererPresentMode rendererPresentMode(VkPresentModeKHR const mode) noexcept
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return RendererPresentMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR: return RendererPresentMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR: return RendererPresentMode::FIFO;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return RendererPresentMode::FIFORelaxed;
    default: return RendererPresentMode::Unknown;
    }
}

[[nodiscard]]
std::optional<glm::mat4> sceneProjection(
    Camera const& camera,
    VkExtent2D const extent,
    bool const curved_world
)
{
    if (!curved_world) {
        return camera.projectionMatrix(extent.width, extent.height);
    }
    CameraProjection projection = camera.projection();
    projection.far_plane = curvedWorldFarPlane(camera.pose().position.z, projection.far_plane);
    return Camera{ camera.pose(), projection }.projectionMatrix(extent.width, extent.height);
}

void recordPlayers(
    VkCommandBuffer const command,
    VkPipeline const player_pipeline,
    VkPipeline const player_overlay_pipeline,
    VkPipelineLayout const player_layout,
    glm::mat4 const& projection_view,
    std::span<PlayerRenderData const> const players,
    std::optional<glm::dvec3> const curved_camera = std::nullopt
)
{
    auto const draw = [&](VkPipeline const pipeline, bool const overlay) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (PlayerRenderData const& player : players) {
            if (player.render_on_top != overlay) {
                continue;
            }
            glm::vec3 origin{ player.x, player.y, player.z };
            if (curved_camera) {
                origin = boundsNearestToCamera(origin, origin, *curved_camera).minimum - glm::vec3(*curved_camera);
            }
            BoxPushConstants const push{
                .projection_view = projection_view,
                .origin = {
                    origin.x,
                    origin.y,
                    origin.z,
                    0.0F,
                },
                .extent = {
                    PLAYER_BODY_DIMENSIONS.x,
                    PLAYER_BODY_DIMENSIONS.y,
                    PLAYER_BODY_DIMENSIONS.z,
                    0.0F,
                },
                .color = player.color,
                .curvature = {
                    0.0F, 0.0F, curved_camera ? 1.0F : 0.0F,
                    curved_camera ? worldVisibilityRadius(curved_camera->z) : MIN_WORLD_VISIBILITY_RADIUS,
                },
            };
            vkCmdPushConstants(
                command,
                player_layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0U,
                sizeof(push),
                &push
            );
            vkCmdDraw(command, BOX_VERTEX_COUNT, 1U, 0U, 0U);
        }
    };
    draw(player_pipeline, false);
    draw(player_overlay_pipeline, true);
}

void recordFlat3dScene(
    VkCommandBuffer const command,
    VkPipeline const grid_pipeline,
    VkPipelineLayout const grid_layout,
    VkPipeline const player_pipeline,
    VkPipeline const player_overlay_pipeline,
    VkPipelineLayout const player_layout,
    Camera const& camera,
    std::span<PlayerRenderData const> const players,
    VkExtent2D const extent
)
{
    std::optional<glm::mat4> const projection = camera.projectionMatrix(extent.width, extent.height);
    if (!projection.has_value()) {
        return;
    }
    glm::mat4 const projection_view = *projection * camera.viewMatrix();
    BoxPushConstants const platform_push{
        .projection_view = projection_view,
        .origin = { 0.0F, 0.0F, -1.0F, 0.0F },
        .extent = { 32.0F, 32.0F, 1.0F, 1.0F },
        .color = { 0.2F, 0.22F, 0.26F, 1.0F },
    };
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, player_pipeline);
    vkCmdPushConstants(
        command,
        player_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0U,
        sizeof(platform_push),
        &platform_push
    );
    vkCmdDraw(command, BOX_VERTEX_COUNT, 1U, 0U, 0U);

    GridPushConstants const grid_push{ .projection_view = projection_view };
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipeline);
    vkCmdPushConstants(
        command,
        grid_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0U,
        sizeof(grid_push),
        &grid_push
    );
    vkCmdDraw(command, GRID_VERTEX_COUNT, GRID_WORKGROUPS_X * GRID_WORKGROUPS_Y, 0U, 0U);

    recordPlayers(command, player_pipeline, player_overlay_pipeline, player_layout, projection_view, players);
}

[[nodiscard]]
uint32_t recordStoneScene(
    VkCommandBuffer const command,
    VkPipeline const pipeline,
    VkPipeline const solid_pipeline,
    VkPipelineLayout const layout,
    VkDescriptorSet const descriptor_set,
    VkPipeline const player_pipeline,
    VkPipeline const player_overlay_pipeline,
    VkPipelineLayout const player_layout,
    Camera const& camera,
    std::span<PlayerRenderData const> const players,
    std::span<StoneDrawRange const> const draw_ranges,
    std::span<StoneDrawRange const> const solid_draw_ranges,
    VkExtent2D const extent,
    bool const manual_srgb_encode,
    StoneIndirectDraws::Prepared const prepared = {},
    VkBuffer const quad_index_buffer = VK_NULL_HANDLE,
    VkQueryPool const diagnostic_query_pool = VK_NULL_HANDLE,
    StoneIndirectDraws::QuadMode const quad_mode = StoneIndirectDraws::QuadMode::SixVertex,
    VkQueryPool const occlusion_query_pool = VK_NULL_HANDLE,
    bool const curved_world = false,
    float const block_sea_level = 0.0F
)
{
    std::optional<glm::mat4> const projection = sceneProjection(camera, extent, curved_world);
    if (!projection.has_value()) {
        return 0U;
    }
    CameraPose const camera_pose = camera.pose();
    std::array<int32_t, 4> const world_origin{
        static_cast<int32_t>(std::floor(camera_pose.position.x)),
        static_cast<int32_t>(std::floor(camera_pose.position.y)),
        static_cast<int32_t>(std::floor(camera_pose.position.z)),
        static_cast<int32_t>(quad_mode),
    };
    glm::vec3 const relative_eye{
        static_cast<float>(camera_pose.position.x - world_origin[0]),
        static_cast<float>(camera_pose.position.y - world_origin[1]),
        static_cast<float>(camera_pose.position.z - world_origin[2]),
    };
    glm::mat4 const relative_view = glm::lookAtRH(
        relative_eye,
        relative_eye + glm::vec3(camera.forward()),
        glm::vec3(camera.up())
    );
    StonePushConstants const push{
        .projection_view = *projection * relative_view,
        .world_origin = world_origin,
        .camera_fog = {
            relative_eye.x,
            relative_eye.y,
            manual_srgb_encode ? 1.0F : 0.0F,
            curved_world ? worldVisibilityRadius(camera_pose.position.z) : static_cast<float>(
                camera.projection().far_plane > 1'200.0 ? camera.projection().far_plane * 0.9 : 720.0
            ),
        },
        .curvature = {
            relative_eye.x, relative_eye.y, curved_world ? 1.0F : 0.0F,
            worldVisibilityRadius(camera_pose.position.z),
        },
        .surface = { block_sea_level, 0.0F, 0.0F, 0.0F },
    };
    if (quad_index_buffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(command, quad_index_buffer, 0U, VK_INDEX_TYPE_UINT16);
    }
    if (diagnostic_query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, diagnostic_query_pool, 0U);
    }
    uint32_t occlusion_query_index = 0U;
    auto const record_direct = [&](std::span<StoneDrawRange const> const ranges) {
        uint32_t draw_count = 0U;
        for (StoneDrawRange const range : ranges) {
            if (occlusion_query_pool != VK_NULL_HANDLE) {
                vkCmdBeginQuery(command, occlusion_query_pool, occlusion_query_index, 0U);
            }
            if (quad_mode == StoneIndirectDraws::QuadMode::IndexedBlock) {
                uint32_t first = range.first_instance;
                uint32_t remaining_quads = range.instance_count;
                do {
                    uint32_t const count = std::min(remaining_quads, StoneIndirectDraws::QUADS_PER_INDEX_BLOCK);
                    auto const draw = StoneIndirectDraws::indexedBlockCommand({ first, count });
                    vkCmdDrawIndexed(command, draw.indexCount, draw.instanceCount, draw.firstIndex,
                        draw.vertexOffset, draw.firstInstance);
                    ++draw_count;
                    first += count;
                    remaining_quads -= count;
                } while (remaining_quads != 0U);
            } else if (quad_index_buffer != VK_NULL_HANDLE) {
                vkCmdDrawIndexed(command, STONE_FACE_VERTEX_COUNT, range.instance_count, 0U, 0, range.first_instance);
                ++draw_count;
            } else {
                vkCmdDraw(command, STONE_FACE_VERTEX_COUNT, range.instance_count, 0U, range.first_instance);
                ++draw_count;
            }
            if (occlusion_query_pool != VK_NULL_HANDLE) {
                vkCmdEndQuery(command, occlusion_query_pool, occlusion_query_index++);
            }
        }
        return draw_count;
    };
    uint32_t draw_count = 0U;
    if (!draw_ranges.empty()) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(
            command,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout,
            0U,
            1U,
            &descriptor_set,
            0U,
            nullptr
        );
        vkCmdPushConstants(
            command,
            layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0U,
            sizeof(push),
            &push
        );
        if (prepared.buffer != nullptr) {
            prepared.record(command, 0U, prepared.textured_command_count);
            draw_count += prepared.textured_command_count;
        } else {
            draw_count += record_direct(draw_ranges);
        }
    }
    if (!solid_draw_ranges.empty()) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline);
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0U, 1U, &descriptor_set, 0U, nullptr
        );
        vkCmdPushConstants(
            command,
            layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0U,
            sizeof(push),
            &push
        );
        if (prepared.buffer != nullptr) {
            prepared.record(
                command,
                prepared.textured_command_count,
                prepared.logical_draw_count - prepared.textured_command_count
            );
            draw_count += prepared.logical_draw_count - prepared.textured_command_count;
        } else {
            draw_count += record_direct(solid_draw_ranges);
        }
    }
    if (diagnostic_query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, diagnostic_query_pool, 1U);
    }
    recordPlayers(
        command,
        player_pipeline,
        player_overlay_pipeline,
        player_layout,
        curved_world ? *projection * glm::lookAtRH(
            glm::vec3(0.0F), glm::vec3(camera.forward()), glm::vec3(camera.up())
        ) : *projection * camera.viewMatrix(),
        players,
        curved_world ? std::optional<glm::dvec3>{ camera_pose.position } : std::nullopt
    );
    return draw_count;
}

[[nodiscard]]
TextBatch buildWorldTextBatch(std::span<TextLabel const> const labels)
{
    TextBatch result{};
    result.space = TextSpace::World;
    for (TextLabel const& label : labels) {
        if (label.placement.space != TextSpace::World) {
            continue;
        }
        TextDocument document{};
        if (!appendText(document, label.value, label.color)) {
            continue;
        }
        TextBatch batch{};
        packText(document, batch, label.placement, {}, label.color);
        result.glyphs.insert(result.glyphs.end(), batch.glyphs.begin(), batch.glyphs.end());
    }
    return result;
}

void recordText(
    VkCommandBuffer const command,
    VkPipeline const pipeline,
    VkPipelineLayout const layout,
    VkDescriptorSet const descriptor_set,
    TextBatch const& batch,
    glm::mat4 const& projection_view,
    VkExtent2D const extent,
    TextShaderMode const mode,
    uint32_t const first_instance,
    uint32_t& draw_count,
    glm::vec4 const curvature_camera = {}
)
{
    size_t const count = batch.glyphs.size();
    if (count == 0U) {
        return;
    }
    if (count > MAXIMUM_TEXT_GLYPH_COUNT) {
        throw std::invalid_argument("text batch exceeds its bounded GPU buffer");
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(
        command,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        layout,
        0U,
        1U,
        &descriptor_set,
        0U,
        nullptr
    );
    TextPushConstants const push{
        .projection_view = projection_view,
        .resolution = {
            static_cast<float>(extent.width),
            static_cast<float>(extent.height),
        },
        .scale = 1.0F,
        .mode = static_cast<uint32_t>(mode),
        .curvature_camera = curvature_camera,
    };
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(push), &push);
    vkCmdDraw(command, QUAD_VERTEX_COUNT, static_cast<uint32_t>(count), 0U, first_instance);
    ++draw_count;
}

} // namespace

namespace detail {

void recordTextGlyphUpload(
    VkCommandBuffer const command,
    VkBuffer const buffer,
    std::span<TextGlyph const> const glyphs,
    PFN_vkCmdPipelineBarrier const pipeline_barrier,
    PFN_vkCmdUpdateBuffer const update_buffer
)
{
    if (glyphs.empty()) {
        return;
    }
    if (glyphs.size() > MAXIMUM_TEXT_GLYPH_COUNT) {
        throw std::invalid_argument("text glyph upload exceeds its bounded GPU buffer");
    }
    size_t const bytes = glyphs.size() * sizeof(TextGlyph);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = static_cast<VkDeviceSize>(bytes);
    // The glyph buffer is shared by submitted frame slots; finish their reads before overwriting it.
    pipeline_barrier(
        command,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0U,
        0U,
        nullptr,
        1U,
        &barrier,
        0U,
        nullptr
    );
    for (size_t offset = 0U; offset < glyphs.size(); offset += MAXIMUM_TEXT_UPDATE_GLYPHS) {
        size_t const count = std::min(MAXIMUM_TEXT_UPDATE_GLYPHS, glyphs.size() - offset);
        update_buffer(
            command,
            buffer,
            static_cast<VkDeviceSize>(offset) * sizeof(TextGlyph),
            static_cast<VkDeviceSize>(count) * sizeof(TextGlyph),
            glyphs.data() + offset
        );
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pipeline_barrier(
        command,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0U,
        0U,
        nullptr,
        1U,
        &barrier,
        0U,
        nullptr
    );
}

void recordDepthAttachmentBarrier(
    VkCommandBuffer const command,
    VkImage const image,
    bool const initialized,
    PFN_vkCmdPipelineBarrier const pipeline_barrier
)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = initialized ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0U;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1U;
    barrier.subresourceRange.layerCount = 1U;
    // A slot fence need not belong to the previous use of this swapchain image's depth target.
    VkPipelineStageFlags const depth_stages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    pipeline_barrier(
        command,
        initialized ? depth_stages : VkPipelineStageFlags{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT},
        depth_stages,
        0U,
        0U,
        nullptr,
        0U,
        nullptr,
        1U,
        &barrier
    );
}

} // namespace detail

// Keep resources before dependent wrappers so reverse destruction releases wrappers first.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct VulkanRenderer::Impl final {
    using PresentationContext = core::graphics::vulkan::PresentationContext;
    using ResourceScope = PresentationContext::PresentationResourceScope;

    struct HeightTileCoordinateHash final {
        [[nodiscard]] size_t operator()(shared::HeightTileCoordinate const coordinate) const noexcept
        {
            return static_cast<size_t>(shared::heightTileCoordinateHash(coordinate.x, coordinate.y));
        }
    };

    struct HeightTileSlot final {
        StoneDrawRange range;
        std::vector<StoneFaceInstance> instances;
        glm::vec3 minimum;
        glm::vec3 maximum;
        HeightTileDrawIndex::DirectionalSpans directional_spans{};
        shared::TerrainIdentity identity;
        shared::HeightTileSurfaceDetail detail = shared::HeightTileSurfaceDetail::Fine;
    };

    using HeightTileSlots = std::unordered_map<
        shared::HeightTileCoordinate, HeightTileSlot, HeightTileCoordinateHash
    >;
    using HeightTileSlotNode = HeightTileSlots::node_type;

    struct HeightTileSlotRetirement final {
        explicit HeightTileSlotRetirement(
            HeightTileInstanceReaper<HeightTileSlotNode>& owner,
            uint64_t const maximum_nodes
        )
            : reaper(owner)
        {
            nodes.reserve(maximum_nodes);
        }

        ~HeightTileSlotRetirement() noexcept
        {
            if (!nodes.empty()) {
                static_cast<void>(reaper.retire(nodes, owned_storage_bytes));
            }
        }

        HeightTileSlotRetirement(HeightTileSlotRetirement const&) = delete;
        HeightTileSlotRetirement& operator=(HeightTileSlotRetirement const&) = delete;

        void extract(HeightTileSlots& slots, HeightTileSlots::const_iterator const existing)
        {
            owned_storage_bytes += sizeof(HeightTileSlots::value_type) + 4U * sizeof(void*)
                + static_cast<uint64_t>(existing->second.instances.capacity()) * sizeof(StoneFaceInstance);
            nodes.push_back(slots.extract(existing));
        }

        HeightTileInstanceReaper<HeightTileSlotNode>& reaper;
        std::vector<HeightTileSlotNode> nodes;
        uint64_t owned_storage_bytes = 0U;
    };

    struct RetiredStoneRange final {
        StoneDrawRange range;
        uint64_t reusable_after_frame = 0U;
    };

    struct DepthTarget final {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool layout_initialized = false;
    };

    struct DiagnosticQuerySlot final {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint64_t attempt_id = 0U;
        uint64_t quad_count = 0U;
        uint32_t draw_count = 0U;
        bool submitted = false;
    };

    struct OcclusionIdentity final {
        CameraPose pose;
        CameraProjection projection;
        uint64_t mesh_generation = 0U;
        uint32_t width = 0U;
        uint32_t height = 0U;

        bool operator==(OcclusionIdentity const&) const noexcept = default;
    };

    struct OcclusionQuerySlot final {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t capacity = 0U;
        OcclusionIdentity identity;
        std::vector<StoneDrawRange> textured;
        std::vector<StoneDrawRange> solid;
        bool submitted = false;
    };

    struct OcclusionResults final {
        OcclusionIdentity identity;
        std::vector<StoneDrawRange> textured;
        std::vector<StoneDrawRange> solid;
        std::vector<std::array<uint64_t, 2>> samples;
    };

    Impl(
        std::shared_ptr<PresentationContext> context,
        ShaderAssets const& shader_assets,
        VulkanRendererOptions options
    )
        : m_context(std::move(context))
        , m_shader_assets(shader_assets)
        , m_options(options)
    {
        if (!m_context) {
            throw std::invalid_argument("Vulkan renderer requires a presentation context");
        }
        if (m_context->info().surface_transform.requires_client_orientation_compensation) {
            throw std::runtime_error(
                "Vulkan renderer cannot present upright without client orientation compensation"
            );
        }
        m_text_glyph_upload.reserve(MAXIMUM_TEXT_GLYPH_COUNT);
        createResources();
        refreshCaptureState();
    }

    ~Impl()
    {
        try {
            m_context->recreate(
                m_context->info().extent,
                {
                    .before = &Impl::beforeRecreate,
                    .user_data = this,
                },
                std::chrono::seconds{ 5 }
            );
        } catch (std::exception const&) {
            destroyResources();
        }
    }

    [[nodiscard]] bool render(
        std::span<PlayerRenderData const> const players,
        DebugHudInput debug_hud_input,
        float const debug_hud_dpi_scale,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        m_successful_presentation_id.reset();
        m_successful_presentation_swapchain_generation = 0U;
        ++m_diagnostic_render_attempt_id;
        m_diagnostic_frame_recorded = false;
        m_diagnostic_stone_indirect = false;
        m_diagnostic_submitted_stone_quad_count = 0U;
        m_occlusion_oracle_frame = false;
        m_occlusion_filtered_frame = false;
        m_occlusion_removed_quads = 0U;
        m_occlusion_removed_ranges = 0U;
        m_occlusion_removed_commands = 0U;
        m_diagnostic_presentation_acquire = {};
        m_current_diagnostic_query_pool = VK_NULL_HANDLE;
        m_cpu_acquire_wait_duration = std::chrono::nanoseconds::zero();
        m_cpu_presentation_recreation_duration = std::chrono::nanoseconds::zero();
        m_presentation_recreation_attempted = false;
        m_presentation_recreation_deferred = false;
        m_cpu_command_record_duration = std::chrono::nanoseconds::zero();
        m_cpu_visible_range_collection_duration = std::chrono::nanoseconds::zero();
        m_cpu_height_tile_group_and_tile_traversal_duration = std::chrono::nanoseconds::zero();
        m_cpu_height_tile_pending_range_order_duration = std::chrono::nanoseconds::zero();
        m_cpu_height_tile_visible_range_merge_duration = std::chrono::nanoseconds::zero();
        m_cpu_indirect_prepare_duration = std::chrono::nanoseconds::zero();
        m_cpu_stone_scene_record_duration = std::chrono::nanoseconds::zero();
        m_cpu_command_record_residual_duration = std::chrono::nanoseconds::zero();
        m_cpu_complete_present_wait_duration = std::chrono::nanoseconds::zero();
        m_cpu_frame_duration = std::chrono::nanoseconds::zero();
        bool const presented = m_pending_stone_face_growth.render(
            requiredValue(m_stone_face_buffer).capacity(),
            deadline,
            [this](uint32_t const target_capacity, std::chrono::steady_clock::time_point const frame_deadline) {
                static_cast<void>(growStoneFaceCapacity(target_capacity, frame_deadline));
                return requiredValue(m_stone_face_buffer).capacity();
            },
            std::chrono::steady_clock::now(),
            [this, players, debug_hud_input, debug_hud_dpi_scale, deadline] {
                return renderWithReadyStoneFaceCapacity(players, debug_hud_input, debug_hud_dpi_scale, deadline);
            }
        );
        if (!presented) {
            m_last_presented = false;
        }
        return presented;
    }

    [[nodiscard]] bool renderWithReadyStoneFaceCapacity(
        std::span<PlayerRenderData const> const players,
        DebugHudInput debug_hud_input,
        float const debug_hud_dpi_scale,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        debug_hud_input.presented = m_last_presented;
        m_debug_hud_state.setDpiScale(debug_hud_dpi_scale);
        m_debug_hud_state.update(debug_hud_input);
        m_gui_renderer.begin();
        m_world_text_batch = {};
        if (m_debug_hud_state.formatText(m_debug_hud_text)) {
            DebugHudSnapshot const snapshot = m_debug_hud_state.snapshot();
            static_cast<void>(m_gui_renderer.submit(
                m_debug_hud_text,
                m_debug_hud_state.lineColors(),
                {},
                TextPlacement{ .position = { 8.0F, 8.0F, 0.0F }, .scale = snapshot.dpi_scale }
            ));
        }
        if (!m_gui_text.labels().empty()) {
            static_cast<void>(m_gui_renderer.submit(m_gui_text, glm::mat4{ 1.0F }, m_context->info().extent.width,
                m_context->info().extent.height));
        }
        m_world_text_batch = buildWorldTextBatch(m_world_text.labels());
        size_t remaining_text_capacity = MAXIMUM_TEXT_GLYPH_COUNT;
        if (m_world_text_batch.glyphs.size() > remaining_text_capacity) {
            m_last_presented = false;
            return false;
        }
        remaining_text_capacity -= m_world_text_batch.glyphs.size();
        for (TextBatch const& batch : m_gui_renderer.batches()) {
            if (batch.glyphs.size() > remaining_text_capacity) {
                m_last_presented = false;
                return false;
            }
            remaining_text_capacity -= batch.glyphs.size();
        }

        std::chrono::steady_clock::time_point const acquire_started_at = std::chrono::steady_clock::now();
        std::optional<PresentationContext::Frame> frame;
        std::chrono::nanoseconds const acquire_deadline = remaining(deadline);
        bool const capture_frame_completion = m_diagnostic_presentation_stages || m_frame_completion_tracking;
        core::graphics::vulkan::PresentationAcquireResult const acquired = capture_frame_completion
            ? m_context->acquire(acquire_deadline, frame, m_diagnostic_presentation_acquire)
            : m_context->acquire(acquire_deadline, frame);
        if (m_frame_completion_tracking) {
            m_frame_progress_tracker.observeAcquire(m_diagnostic_presentation_acquire);
        }
        m_cpu_acquire_wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - acquire_started_at
        );
        if (acquired == core::graphics::vulkan::PresentationAcquireResult::NeedsRecreation) {
            m_last_presented = false;
            static constexpr auto MAXIMUM_RECREATION_WAIT = std::chrono::milliseconds{ 8 };
            std::chrono::steady_clock::time_point const recreation_started_at = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point const recreation_deadline = std::min(
                deadline,
                recreation_started_at + MAXIMUM_RECREATION_WAIT
            );
            m_presentation_recreation_attempted = true;
            try {
                m_presentation_recreation_deferred = !detail::tryRecreatePresentationWithinDeadline(
                    recreation_deadline,
                    [this](std::chrono::steady_clock::time_point const bounded_deadline) {
                        recreate(m_context->info().extent, bounded_deadline);
                    }
                );
            } catch (...) {
                m_cpu_presentation_recreation_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - recreation_started_at
                );
                throw;
            }
            m_cpu_presentation_recreation_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - recreation_started_at
            );
            return false;
        }
        if (acquired != core::graphics::vulkan::PresentationAcquireResult::Ready || !frame.has_value()) {
            m_last_presented = false;
            return false;
        }

        std::chrono::steady_clock::time_point const command_record_started_at =
            std::chrono::steady_clock::now();
        m_players = players;
        m_image_view = frame->imageView();
        m_current_frame_slot = frame->slotIndex();
        if (m_frame_completion_tracking) {
            m_frame_progress_tracker.prepareSlot(frame->slotIndex());
        }
        acquireDiagnosticQuerySlot();
        m_current_depth_target = &depthTargetFor(frame->image());
        frame->record(&Impl::recordFrame, this);
        m_diagnostic_frame_recorded = true;
        m_cpu_command_record_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - command_record_started_at
        );
        m_cpu_command_record_residual_duration = std::max(
            std::chrono::nanoseconds::zero(),
            m_cpu_command_record_duration - m_cpu_visible_range_collection_duration
                - m_cpu_indirect_prepare_duration - m_cpu_stone_scene_record_duration
        );
        std::chrono::steady_clock::time_point const complete_present_started_at =
            std::chrono::steady_clock::now();
        m_context->complete(*frame);
        m_last_submitted_terrain_lod_scene = m_terrain_lod_scene;
        m_last_submitted_debug_hud_enabled = m_debug_hud_state.snapshot().enabled;
        if (m_map_image_recorded_revision != 0U) {
            m_map_image_uploaded_revision = m_map_image_recorded_revision;
            m_map_texture_layout = m_map_image_recorded_layout;
            m_map_image_recorded_revision = 0U;
        }
        if (m_frame_completion_tracking) {
            static_cast<void>(m_frame_progress_tracker.recordSubmission(
                frame->slotIndex(),
                frame->swapchainGeneration()
            ));
        }
        m_successful_presentation_id = frame->presentationId();
        m_successful_presentation_swapchain_generation = frame->swapchainGeneration();
        if (m_occlusion_oracle_frame) {
            m_occlusion_query_slots.at(m_current_frame_slot).submitted = true;
        }
        if (m_current_diagnostic_query_pool != VK_NULL_HANDLE && !m_occlusion_oracle_frame) {
            DiagnosticQuerySlot& slot = m_diagnostic_query_slots.at(m_current_frame_slot);
            slot.attempt_id = m_diagnostic_render_attempt_id;
            slot.quad_count = m_diagnostic_submitted_stone_quad_count;
            slot.draw_count = m_chunk_draw_count;
            slot.submitted = true;
        }
        m_cpu_complete_present_wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - complete_present_started_at
        );
        m_current_depth_target = nullptr;
        m_last_presented = true;
        m_cpu_frame_duration = m_cpu_command_record_duration + m_cpu_complete_present_wait_duration;
        ++m_submitted_frame_count;

        if (m_capture_requested && !m_occlusion_oracle_frame) {
            std::optional<std::span<uint8_t const>> const readback = m_context->takeCompletedReadback(
                remaining(deadline)
            );
            if (readback.has_value()) {
                if (m_frame_completion_tracking) {
                    m_frame_progress_tracker.observeCompletedSlot(m_current_frame_slot);
                }
                core::graphics::vulkan::PresentationInfo const& info = m_context->info();
                m_last_capture = RendererFrameCapture{
                    .width = info.extent.width,
                    .height = info.extent.height,
                    .srgb_encoded = true,
                    .rgba8 = std::vector<uint8_t>(readback->begin(), readback->end()),
                };
                m_capture_requested = false;
                m_capture_state = FrameCaptureState::Completed;
            }
        }
        return true;
    }

    void setDebugHudEnabled(bool const enabled) noexcept
    {
        m_debug_hud_state.setEnabled(enabled);
    }

    void toggleDebugHud() noexcept
    {
        m_debug_hud_state.toggle();
    }

    void setFallbackSceneEnabled(bool const enabled) noexcept
    {
        m_fallback_scene_enabled = enabled;
    }

    void setGuiText(std::string_view const text, TextColor const color)
    {
        m_gui_text.clear();
        static_cast<void>(m_gui_text.submit(text, color));
    }

    void addGuiText(std::string_view const text, TextPlacement placement, TextColor const color)
    {
        placement.space = TextSpace::Screen;
        static_cast<void>(m_gui_text.submit(text, color, placement));
    }

    void clearGuiText() noexcept
    {
        m_gui_text.clear();
    }

    [[nodiscard]] bool setMapImage(
        uint32_t const width,
        uint32_t const height,
        std::span<MapImageData::Pixel const> const rgba8
    )
    {
        return m_map_image_data.set(width, height, rgba8);
    }

    void clearMapImage() noexcept
    {
        m_map_image_data.clear();
    }

    void setFrameCompletionTracking(bool const enabled) noexcept
    {
        if (m_frame_completion_tracking != enabled) {
            m_frame_progress_tracker.resetProgress();
        }
        m_frame_completion_tracking = enabled;
    }

    [[nodiscard]] RendererFrameProgress frameProgress() const noexcept
    {
        return m_frame_progress_tracker.progress();
    }

    void setWorldText(
        std::string_view const text,
        TextPlacement placement,
        TextColor const color
    )
    {
        m_world_text.clear();
        placement.space = TextSpace::World;
        static_cast<void>(m_world_text.submit(text, color, placement));
    }

    void addWorldText(std::string_view const text, TextPlacement placement, TextColor const color)
    {
        placement.space = TextSpace::World;
        static_cast<void>(m_world_text.submit(text, color, placement));
    }

    void clearWorldText() noexcept
    {
        m_world_text.clear();
    }

    void setCamera(CameraPose const pose) noexcept
    {
        static_cast<void>(m_camera.setPosition(pose.position));
        static_cast<void>(m_camera.setAngles(pose.angles));
    }

    void setCamera(CameraPose const pose, CameraProjection const projection) noexcept
    {
        setCamera(pose);
        static_cast<void>(m_camera.setProjection(projection));
    }

    void setChunkMesh(shared::ChunkMesh const& mesh)
    {
        ++m_occlusion_mesh_generation;
        std::vector<StoneFaceInstance> instances = stoneFaceInstances(mesh);
        if (!m_height_tile_slots.empty() || (m_chunk_scene_enabled && instances == m_stone_faces)) {
            clearHeightTileMeshes();
        }
        if (m_chunk_scene_enabled && instances == m_stone_faces) {
            return;
        }
        m_stone_faces = std::move(instances);
        m_legacy_draw_range = { .first_instance = 0U, .instance_count = static_cast<uint32_t>(m_stone_faces.size()) };
        m_chunk_scene_enabled = true;
        if (!ensureStoneFaceCapacity(static_cast<uint32_t>(m_stone_faces.size()))) {
            requiredValue(m_stone_face_buffer).upload(0U, m_stone_faces);
            ++m_chunk_mesh_upload_count;
        }
    }

    void setChunkMeshes(std::span<shared::ChunkMesh const> const meshes)
    {
        ++m_occlusion_mesh_generation;
        std::vector<StoneFaceInstance> instances;
        for (shared::ChunkMesh const& mesh : meshes) {
            std::vector<StoneFaceInstance> chunk = stoneFaceInstances(mesh);
            if (instances.size() + chunk.size() > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
                throw std::invalid_argument("chunk mesh set exceeds its face limit");
            }
            instances.insert(instances.end(), chunk.begin(), chunk.end());
        }
        if (!m_height_tile_slots.empty()) {
            clearHeightTileMeshes();
        }
        if (m_chunk_scene_enabled && instances == m_stone_faces) {
            return;
        }
        m_stone_faces = std::move(instances);
        m_legacy_draw_range = { .first_instance = 0U, .instance_count = static_cast<uint32_t>(m_stone_faces.size()) };
        m_chunk_scene_enabled = true;
        if (!ensureStoneFaceCapacity(static_cast<uint32_t>(m_stone_faces.size()))) {
            requiredValue(m_stone_face_buffer).upload(0U, m_stone_faces);
            ++m_chunk_mesh_upload_count;
        }
    }

    [[nodiscard]] bool upsertTerrainLodScene(
        std::shared_ptr<TerrainLodScene const> scene,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        if (!scene || scene->faces.empty()) {
            return false;
        }
        if (scene->faces.size() > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
            throw std::invalid_argument("terrain LOD scene exceeds the renderer face capacity");
        }
        if (scene == m_terrain_lod_scene) {
            discardPendingTerrainLodScene();
            return true;
        }
        if (scene != m_pending_terrain_lod_scene && !beginPendingTerrainLodScene(std::move(scene))) {
            return false;
        }
        static constexpr uint32_t UPLOAD_FACE_BUDGET = 32'768U;
        uint32_t remaining_budget = UPLOAD_FACE_BUDGET;
        for (uint32_t index = 0U; index < m_pending_terrain_lod_ranges.size(); ++index) {
            auto const& group = m_pending_terrain_lod_scene->groups[index];
            if (m_pending_terrain_lod_uploaded[index] == group.range.instance_count) {
                continue;
            }
            if (remaining_budget == 0U || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            auto& range = m_pending_terrain_lod_ranges[index];
            if (range.instance_count == 0U) {
                range = allocateRange(group.range.instance_count);
            }
            uint32_t const required_capacity = range.first_instance + range.instance_count;
            recordStoneFaceCapacityRequest(required_capacity);
            if (!tryEnsureStoneFaceCapacity(required_capacity, deadline)
                || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            uint32_t const count = std::min(
                remaining_budget, group.range.instance_count - m_pending_terrain_lod_uploaded[index]
            );
            requiredValue(m_stone_face_buffer).upload(
                range.first_instance + m_pending_terrain_lod_uploaded[index],
                detail::terrainLodGroupFaces(*m_pending_terrain_lod_scene, group).subspan(
                    m_pending_terrain_lod_uploaded[index], count
                )
            );
            m_pending_terrain_lod_uploaded[index] += count;
            m_terrain_lod_uploaded_face_count += count;
            remaining_budget -= count;
            ++m_chunk_mesh_upload_count;
        }
        if (!detail::terrainLodUploadsComplete(*m_pending_terrain_lod_scene, m_pending_terrain_lod_uploaded)) {
            return false;
        }
        publishPendingTerrainLodScene();
        return true;
    }

    [[nodiscard]] bool beginPendingTerrainLodScene(std::shared_ptr<TerrainLodScene const> scene)
    {
        auto plan = m_terrain_lod_upload_planner.poll(m_terrain_lod_scene, m_terrain_lod_ranges, std::move(scene));
        if (!plan) {
            return false;
        }
        auto field = fieldForScene(plan->scene);
        discardPendingTerrainLodScene();
        m_pending_terrain_field = std::move(field);
        m_pending_terrain_lod_scene = std::move(plan->scene);
        m_pending_terrain_lod_reusable = std::move(plan->reusable);
        m_pending_terrain_lod_ranges = std::move(plan->ranges);
        m_pending_terrain_lod_uploaded = std::move(plan->uploaded);
        return true;
    }

    [[nodiscard]] std::shared_ptr<CanonicalTerrainFieldBuffer> fieldForScene(
        std::shared_ptr<TerrainLodScene const> const& scene)
    {
        if (!scene || !scene->shading_atlas) {
            return m_disabled_terrain_field;
        }
        if (scene->identity.seed != scene->shading_atlas->configuration.seed
            || scene->identity.configuration_identity != generation::configurationIdentity(scene->shading_atlas->configuration)) {
            throw std::invalid_argument("Canonical shading field does not match terrain scene identity");
        }
        for (auto const& field : {m_active_terrain_field, m_pending_terrain_field}) {
            if (field && field->atlas() == scene->shading_atlas) {
                return field;
            }
        }
        return std::make_shared<CanonicalTerrainFieldBuffer>(requiredValue(m_resources).device(),
            requiredValue(m_resources).physicalDevice(), scene->shading_atlas);
    }

    [[nodiscard]] VkDescriptorSet stoneDescriptorForFrame()
    {
        auto& slot = m_stone_frame_descriptors[m_current_frame_slot];
        auto const field = m_terrain_lod_scene ? m_active_terrain_field : m_disabled_terrain_field;
        if (slot.field != field) {
            // The acquired frame slot's fence has completed; no submitted command can use its old set.
            slot.descriptors = std::make_unique<StoneDescriptorSet>(requiredValue(m_resources).device(),
                requiredValue(m_stone_face_buffer), requiredValue(m_stone_texture_buffer), *field);
            slot.field = field;
        }
        return slot.descriptors->set();
    }

    void publishPendingTerrainLodScene()
    {
        m_terrain_lod_upload_planner.invalidate();
        std::vector<bool> retained(m_terrain_lod_ranges.size());
        m_terrain_lod_reused_group_count = 0U;
        for (auto const previous : m_pending_terrain_lod_reusable) {
            if (previous) {
                retained[*previous] = true;
                ++m_terrain_lod_reused_group_count;
            }
        }
        for (uint32_t index = 0U; index < m_terrain_lod_ranges.size(); ++index) {
            if (!retained[index]) {
                releaseRange(m_terrain_lod_ranges[index]);
            }
        }
        m_active_terrain_field = std::move(m_pending_terrain_field);
        m_terrain_lod_scene = std::move(m_pending_terrain_lod_scene);
        m_terrain_lod_ranges = std::move(m_pending_terrain_lod_ranges);
        m_pending_terrain_lod_ranges.clear();
        m_pending_terrain_lod_reusable.clear();
        m_pending_terrain_lod_uploaded.clear();
        m_chunk_scene_enabled = true;
        ++m_occlusion_mesh_generation;
    }

    void discardPendingTerrainLodScene()
    {
        m_terrain_lod_upload_planner.invalidate();
        for (uint32_t index = 0U; index < m_pending_terrain_lod_ranges.size(); ++index) {
            if (!m_pending_terrain_lod_reusable[index]
                && m_pending_terrain_lod_ranges[index].instance_count != 0U) {
                makeRangeReusable(m_pending_terrain_lod_ranges[index]);
            }
        }
        m_pending_terrain_field.reset();
        m_pending_terrain_lod_scene.reset();
        m_pending_terrain_lod_ranges.clear();
        m_pending_terrain_lod_reusable.clear();
        m_pending_terrain_lod_uploaded.clear();
    }

    void clearTerrainLodScene()
    {
        discardPendingTerrainLodScene();
        for (auto const range : m_terrain_lod_ranges) {
            releaseRange(range);
        }
        m_terrain_lod_ranges.clear();
        m_terrain_lod_scene.reset();
        m_active_terrain_field.reset();
        m_chunk_scene_enabled = !m_stone_faces.empty() || !m_height_tile_slots.empty();
        ++m_occlusion_mesh_generation;
    }

    [[nodiscard]] bool upsertHeightTileMesh(
        shared::HeightTileSurfaceMesh const& mesh,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        ++m_occlusion_mesh_generation;
        std::vector<StoneFaceInstance> instances = stoneFaceInstances(mesh);
        HeightTileSlot* const existing = heightTileSlot(mesh.coordinate);
        if (existing != nullptr && existing->instances == instances
            && existing->identity == mesh.identity && existing->detail == mesh.detail) {
            return true;
        }
        HeightTileSlotRetirement retirement{ m_height_tile_slot_reaper, existing == nullptr ? 0U : 1U };
        if (instances.empty()) {
            if (existing != nullptr) {
                releaseRange(existing->range);
                m_height_tile_face_count -= existing->range.instance_count;
                retirement.extract(m_height_tile_slots, m_height_tile_slots.find(mesh.coordinate));
                m_height_tile_draw_index.remove(mesh.coordinate);
            }
            if (m_height_tile_slots.empty() && m_stone_faces.empty() && !m_terrain_lod_scene) {
                m_chunk_scene_enabled = false;
            }
            return true;
        }
        StoneDrawRange const range = allocateRange(static_cast<uint32_t>(instances.size()));
        uint32_t const required_capacity = range.first_instance + range.instance_count;
        if (!tryEnsureStoneFaceCapacity(required_capacity, deadline)) {
            makeRangeReusable(range);
            return false;
        }
        if (!m_stone_faces.empty()) {
            m_stone_faces.clear();
            m_legacy_draw_range = {};
        }
        if (existing != nullptr) {
            releaseRange(existing->range);
            m_height_tile_face_count -= existing->range.instance_count;
            retirement.extract(m_height_tile_slots, m_height_tile_slots.find(mesh.coordinate));
        }
        glm::vec3 minimum{
            static_cast<float>(instances.front().x),
            static_cast<float>(instances.front().y),
            static_cast<float>(instances.front().z),
        };
        glm::vec3 maximum = minimum;
        for (StoneFaceInstance const& instance : instances) {
            glm::vec3 const origin{
                static_cast<float>(instance.x),
                static_cast<float>(instance.y),
                static_cast<float>(instance.z),
            };
            glm::vec3 extent{ 0.0F };
            if (instance.direction <= 1U) {
                extent = { 1.0F, static_cast<float>(instance.u_extent), static_cast<float>(instance.v_extent) };
            } else if (instance.direction <= 3U) {
                extent = { static_cast<float>(instance.u_extent), 1.0F, static_cast<float>(instance.v_extent) };
            } else {
                extent = { static_cast<float>(instance.u_extent), static_cast<float>(instance.v_extent), 1.0F };
            }
            minimum = glm::min(minimum, origin);
            maximum = glm::max(maximum, origin + extent);
        }
        m_height_tile_slots.emplace(mesh.coordinate, HeightTileSlot{
            .range = range,
            .instances = std::move(instances),
            .minimum = minimum,
            .maximum = maximum,
            .directional_spans = HeightTileDrawIndex::directionalSpans(mesh.quads),
            .identity = mesh.identity,
            .detail = mesh.detail,
        });
        m_height_tile_face_count += range.instance_count;
        m_chunk_scene_enabled = true;
        HeightTileSlot const& slot = m_height_tile_slots.at(mesh.coordinate);
        requiredValue(m_stone_face_buffer).upload(slot.range.first_instance, slot.instances);
        indexHeightTileSlot(mesh.coordinate, slot);
        ++m_chunk_mesh_upload_count;
        return true;
    }

    [[nodiscard]] bool upsertHeightTileMeshes(
        std::span<shared::HeightTileSurfaceMesh const> const meshes,
        std::chrono::steady_clock::time_point const deadline,
        RendererHeightTileUpsertDiagnostics* const diagnostics = nullptr
    )
    {
        ++m_occlusion_mesh_generation;
        if (meshes.size() > 5U) {
            throw std::invalid_argument("height tile mesh batch exceeds five tiles");
        }

        struct PreparedMesh final {
            shared::HeightTileCoordinate coordinate;
            std::optional<HeightTileSlot> slot;
        };
        std::vector<PreparedMesh> prepared;
        prepared.reserve(meshes.size());
        std::vector<StoneDrawRange>& free_ranges = m_free_stone_ranges_scratch;
        free_ranges.assign(m_free_stone_ranges.begin(), m_free_stone_ranges.end());
        std::vector<StoneDrawRange> reclaimed_ranges;
        uint32_t reclaimed_retired_range_count = 0U;
        DiagnosticDurationScope diagnostic_timer;
        diagnostic_timer.start(diagnostics == nullptr ? nullptr : &diagnostics->free_range_prepare_duration);
        auto reclaimed_end = m_retired_stone_ranges.cbegin();
        while (reclaimed_end != m_retired_stone_ranges.cend()
            && reclaimed_end->reusable_after_frame <= m_submitted_frame_count) {
            ++reclaimed_end;
            ++reclaimed_retired_range_count;
        }
        reclaimed_ranges.reserve(reclaimed_retired_range_count);
        for (auto retired = m_retired_stone_ranges.cbegin(); retired != reclaimed_end; ++retired) {
            reclaimed_ranges.push_back(retired->range);
        }
        std::ranges::sort(reclaimed_ranges, {}, &StoneDrawRange::first_instance);
        mergeReusableRanges(free_ranges, reclaimed_ranges);
        diagnostic_timer.finish();
        uint32_t next_stone_face = m_next_stone_face;
        uint32_t required_capacity = m_stone_face_capacity;

        diagnostic_timer.start(diagnostics == nullptr ? nullptr : &diagnostics->mesh_prepare_duration);
        for (shared::HeightTileSurfaceMesh const& mesh : meshes) {
            if (std::ranges::any_of(prepared, [&mesh](PreparedMesh const& candidate) {
                return candidate.coordinate == mesh.coordinate;
            })) {
                throw std::invalid_argument("height tile mesh batch contains duplicate coordinates");
            }
            std::vector<StoneFaceInstance> instances = stoneFaceInstances(mesh);
            HeightTileSlot const* const existing = heightTileSlot(mesh.coordinate);
            if (existing != nullptr && existing->instances == instances
                && existing->identity == mesh.identity && existing->detail == mesh.detail) {
                prepared.push_back({ .coordinate = mesh.coordinate, .slot = std::nullopt });
                continue;
            }
            if (instances.empty()) {
                prepared.push_back({ .coordinate = mesh.coordinate, .slot = HeightTileSlot{} });
                continue;
            }
            uint32_t const instance_count = static_cast<uint32_t>(instances.size());
            StoneDrawRange range{};
            auto const reusable = std::find_if(free_ranges.begin(), free_ranges.end(), [instance_count](
                StoneDrawRange const available
            ) { return available.instance_count >= instance_count; });
            if (reusable != free_ranges.end()) {
                range = { .first_instance = reusable->first_instance, .instance_count = instance_count };
                reusable->first_instance += instance_count;
                reusable->instance_count -= instance_count;
                if (reusable->instance_count == 0U) {
                    free_ranges.erase(reusable);
                }
            } else {
                if (next_stone_face > MAXIMUM_RENDERED_STONE_FACE_COUNT - instance_count) {
                    return false;
                }
                range = { .first_instance = next_stone_face, .instance_count = instance_count };
                next_stone_face += instance_count;
            }
            required_capacity = std::max(required_capacity, range.first_instance + range.instance_count);
            glm::vec3 minimum{
                static_cast<float>(instances.front().x),
                static_cast<float>(instances.front().y),
                static_cast<float>(instances.front().z),
            };
            glm::vec3 maximum = minimum;
            for (StoneFaceInstance const& instance : instances) {
                glm::vec3 const origin{
                    static_cast<float>(instance.x),
                    static_cast<float>(instance.y),
                    static_cast<float>(instance.z),
                };
                glm::vec3 extent{ 0.0F };
                if (instance.direction <= 1U) {
                    extent = { 1.0F, static_cast<float>(instance.u_extent), static_cast<float>(instance.v_extent) };
                } else if (instance.direction <= 3U) {
                    extent = { static_cast<float>(instance.u_extent), 1.0F, static_cast<float>(instance.v_extent) };
                } else {
                    extent = { static_cast<float>(instance.u_extent), static_cast<float>(instance.v_extent), 1.0F };
                }
                minimum = glm::min(minimum, origin);
                maximum = glm::max(maximum, origin + extent);
            }
            prepared.push_back({
                .coordinate = mesh.coordinate,
                .slot = HeightTileSlot{
                    .range = range,
                    .instances = std::move(instances),
                    .minimum = minimum,
                    .maximum = maximum,
                    .directional_spans = HeightTileDrawIndex::directionalSpans(mesh.quads),
                    .identity = mesh.identity,
                    .detail = mesh.detail,
                },
            });
        }
        diagnostic_timer.finish();

        bool const growth_required = required_capacity > m_stone_face_growth.actual_capacity;
        diagnostic_timer.start(diagnostics == nullptr ? nullptr : &diagnostics->capacity_prepare_duration);
        recordStoneFaceCapacityRequest(required_capacity);
        if (std::chrono::steady_clock::now() >= deadline) {
            if (growth_required) {
                m_pending_stone_face_growth.coalesce(required_capacity);
                ++m_stone_face_growth.pre_growth_deadline_rejection_count;
                ++m_stone_face_growth.publication_retry_count;
            }
            return false;
        }
        if (!tryEnsureStoneFaceCapacity(required_capacity, deadline)) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (growth_required) {
                ++m_stone_face_growth.post_growth_deadline_rejection_count;
                ++m_stone_face_growth.publication_retry_count;
            }
            return false;
        }
        diagnostic_timer.finish();
        diagnostic_timer.start(diagnostics == nullptr ? nullptr : &diagnostics->commit_upload_duration);
        HeightTileSlotRetirement retirement{ m_height_tile_slot_reaper, prepared.size() };
        m_free_stone_ranges.swap(free_ranges);
        for (uint32_t index = 0U; index < reclaimed_retired_range_count; ++index) {
            m_retired_stone_ranges.pop_front();
        }
        m_next_stone_face = next_stone_face;
        for (PreparedMesh& candidate : prepared) {
            if (!candidate.slot.has_value()) {
                continue;
            }
            auto const existing = m_height_tile_slots.find(candidate.coordinate);
            if (existing != m_height_tile_slots.end()) {
                releaseRange(existing->second.range);
                m_height_tile_face_count -= existing->second.range.instance_count;
                retirement.extract(m_height_tile_slots, existing);
            }
            if (!candidate.slot->instances.empty()) {
                HeightTileSlot& slot = m_height_tile_slots.emplace(
                    candidate.coordinate, std::move(*candidate.slot)
                ).first->second;
                m_height_tile_face_count += slot.range.instance_count;
                requiredValue(m_stone_face_buffer).upload(slot.range.first_instance, slot.instances);
                indexHeightTileSlot(candidate.coordinate, slot);
                ++m_chunk_mesh_upload_count;
            } else {
                m_height_tile_draw_index.remove(candidate.coordinate);
            }
        }
        if (!m_height_tile_slots.empty()) {
            m_stone_faces.clear();
            m_legacy_draw_range = {};
            m_chunk_scene_enabled = true;
        } else if (m_stone_faces.empty() && !m_terrain_lod_scene) {
            m_chunk_scene_enabled = false;
        }
        diagnostic_timer.finish();
        return true;
    }

    [[nodiscard]] bool removeHeightTileMesh(shared::HeightTileCoordinate const coordinate)
    {
        ++m_occlusion_mesh_generation;
        auto const existing = m_height_tile_slots.find(coordinate);
        if (existing == m_height_tile_slots.end()) {
            return false;
        }
        HeightTileSlotRetirement retirement{ m_height_tile_slot_reaper, 1U };
        releaseRange(existing->second.range);
        m_height_tile_face_count -= existing->second.range.instance_count;
        retirement.extract(m_height_tile_slots, existing);
        m_height_tile_draw_index.remove(coordinate);
        if (m_height_tile_slots.empty() && m_stone_faces.empty() && !m_terrain_lod_scene) {
            m_chunk_scene_enabled = false;
        }
        return true;
    }

    [[nodiscard]] uint32_t removeHeightTileMeshes(
        std::span<shared::HeightTileCoordinate const> const coordinates,
        bool const capture_diagnostics
    )
    {
        m_diagnostic_height_tile_removal = {};
        if (capture_diagnostics) {
            m_diagnostic_height_tile_removal.input_coordinate_count =
                static_cast<uint64_t>(coordinates.size());
        }
        DiagnosticDurationScope prefilter_timer;
        if (capture_diagnostics) {
            prefilter_timer.start(&m_diagnostic_height_tile_removal.prefilter_duration);
        }
        if (coordinates.empty()) {
            prefilter_timer.finish();
            return 0U;
        }
        m_height_tile_removal_scratch.clear();
        m_height_tile_removal_scratch.reserve(coordinates.size());
        for (shared::HeightTileCoordinate const coordinate : coordinates) {
            if (m_height_tile_slots.contains(coordinate)) {
                m_height_tile_removal_scratch.push_back(coordinate);
            }
        }
        prefilter_timer.finish();
        if (capture_diagnostics) {
            m_diagnostic_height_tile_removal.matching_slot_count =
                static_cast<uint64_t>(m_height_tile_removal_scratch.size());
        }
        if (m_height_tile_removal_scratch.empty()) {
            return 0U;
        }
        HeightTileSlotRetirement retirement{ m_height_tile_slot_reaper, m_height_tile_removal_scratch.size() };
        ++m_occlusion_mesh_generation;
        uint32_t removed_count = 0U;
        DiagnosticDurationScope slot_retirement_timer;
        if (capture_diagnostics) {
            slot_retirement_timer.start(&m_diagnostic_height_tile_removal.slot_retirement_duration);
        }
        for (size_t index = 0U; index < m_height_tile_removal_scratch.size(); ++index) {
            shared::HeightTileCoordinate const coordinate = m_height_tile_removal_scratch[index];
            auto const existing = m_height_tile_slots.find(coordinate);
            if (existing == m_height_tile_slots.end()) {
                continue;
            }
            try {
                releaseRange(existing->second.range);
            } catch (...) {
                slot_retirement_timer.finish();
                if (capture_diagnostics) {
                    m_diagnostic_height_tile_removal.removed_slot_count = removed_count;
                }
                DiagnosticDurationScope draw_index_timer;
                if (capture_diagnostics) {
                    draw_index_timer.start(&m_diagnostic_height_tile_removal.draw_index_remove_duration);
                }
                m_height_tile_draw_index.removeBatch(std::span{
                    m_height_tile_removal_scratch.data(),
                    index,
                });
                draw_index_timer.finish();
                if (m_height_tile_slots.empty() && m_stone_faces.empty() && !m_terrain_lod_scene) {
                    m_chunk_scene_enabled = false;
                }
                throw;
            }
            m_height_tile_face_count -= existing->second.range.instance_count;
            retirement.extract(m_height_tile_slots, existing);
            ++removed_count;
        }
        slot_retirement_timer.finish();
        if (capture_diagnostics) {
            m_diagnostic_height_tile_removal.removed_slot_count = removed_count;
        }
        DiagnosticDurationScope draw_index_timer;
        if (capture_diagnostics) {
            draw_index_timer.start(&m_diagnostic_height_tile_removal.draw_index_remove_duration);
        }
        m_height_tile_draw_index.removeBatch(m_height_tile_removal_scratch);
        draw_index_timer.finish();
        if (m_height_tile_slots.empty() && m_stone_faces.empty() && !m_terrain_lod_scene) {
            m_chunk_scene_enabled = false;
        }
        return removed_count;
    }

    void hotReload()
    {
        recreate(m_context->info().extent, std::chrono::steady_clock::time_point::max());
    }

    void resize(uint32_t const width, uint32_t const height)
    {
        VkExtent2D const extent{ .width = width, .height = height };
        if (extent.width != m_context->info().extent.width || extent.height != m_context->info().extent.height) {
            recreate(extent, std::chrono::steady_clock::time_point::max());
        }
    }

    [[nodiscard]] bool waitForSubmittedFrames(std::chrono::steady_clock::time_point const deadline)
    {
        try {
            m_context->recreate(
                m_context->info().extent,
                {
                    .before = &Impl::beforeRecreate,
                    .after = &Impl::afterRecreate,
                    .user_data = this,
                },
                remaining(deadline)
            );
            return true;
        } catch (std::exception const&) {
            return false;
        }
    }

    void requestFrameCapture()
    {
        if (m_capture_state == FrameCaptureState::Completed) {
            m_capture_state = FrameCaptureState::Ready;
        }
        if (m_capture_state == FrameCaptureState::Ready) {
            m_last_capture.reset();
            m_capture_requested = true;
        }
    }

    [[nodiscard]] FrameCaptureState captureState() const noexcept { return m_capture_state; }

    [[nodiscard]] std::optional<RendererFrameCapture> takeFrameCapture()
    {
        return std::exchange(m_last_capture, std::nullopt);
    }

    [[nodiscard]] RendererRuntimeInfo runtimeInfo() const
    {
        core::graphics::vulkan::PresentationInfo const& info = m_context->info();
        return RendererRuntimeInfo{
            .width = info.extent.width,
            .height = info.extent.height,
            .gpu_name = m_gpu_name,
            .vulkan_api_version = {
                .major = VK_VERSION_MAJOR(info.api_version),
                .minor = VK_VERSION_MINOR(info.api_version),
                .patch = VK_VERSION_PATCH(info.api_version),
            },
            .validation_enabled = info.validation_enabled,
            .present_mode = rendererPresentMode(info.present_mode),
            .pipeline_path = RendererPipelinePath::Vertex,
            .presentation_history_enabled = info.presentation_history_enabled,
            .current_swapchain_generation = m_context->swapchainGeneration(),
            .successful_presentation_id = m_successful_presentation_id,
            .successful_presentation_swapchain_generation = m_successful_presentation_swapchain_generation,
            .cpu_acquire_wait_duration = m_cpu_acquire_wait_duration,
            .diagnostic_presentation_acquire = m_diagnostic_presentation_acquire,
            .retired_frame_completions = m_frame_progress_tracker.retiredCompletions(),
            .cpu_presentation_recreation_duration = m_cpu_presentation_recreation_duration,
            .presentation_recreation_attempted = m_presentation_recreation_attempted,
            .presentation_recreation_deferred = m_presentation_recreation_deferred,
            .cpu_command_record_duration = m_cpu_command_record_duration,
            .cpu_visible_range_collection_duration = m_cpu_visible_range_collection_duration,
            .cpu_height_tile_group_and_tile_traversal_duration = m_cpu_height_tile_group_and_tile_traversal_duration,
            .cpu_height_tile_pending_range_order_duration = m_cpu_height_tile_pending_range_order_duration,
            .cpu_height_tile_visible_range_merge_duration = m_cpu_height_tile_visible_range_merge_duration,
            .cpu_indirect_prepare_duration = m_cpu_indirect_prepare_duration,
            .cpu_stone_scene_record_duration = m_cpu_stone_scene_record_duration,
            .cpu_command_record_residual_duration = m_cpu_command_record_residual_duration,
            .cpu_complete_present_wait_duration = m_cpu_complete_present_wait_duration,
            .cpu_frame_duration = m_cpu_frame_duration,
            .gpu_frame_duration = std::nullopt,
            .submitted_frame_count = m_submitted_frame_count,
            .debug_hud_enabled = m_last_submitted_debug_hud_enabled,
            .debug_hud_draw_count = m_debug_hud_draw_count,
            .chunk_face_count = m_terrain_lod_scene ? static_cast<uint32_t>(m_terrain_lod_scene->faces.size())
                : m_height_tile_slots.empty() ? static_cast<uint32_t>(m_stone_faces.size()) : m_height_tile_face_count,
            .chunk_draw_count = m_chunk_draw_count,
            .chunk_draw_face_count = m_chunk_draw_face_count,
            .chunk_mesh_upload_count = m_chunk_mesh_upload_count,
            .height_tile_mesh_count = static_cast<uint32_t>(m_height_tile_slots.size()),
            .terrain_lod_scene = m_last_submitted_terrain_lod_scene,
            .terrain_lod_active = m_terrain_lod_scene != nullptr,
            .terrain_lod_leaf_count = m_terrain_lod_scene
                ? static_cast<uint32_t>(m_terrain_lod_scene->leaves.size()) : 0U,
            .terrain_lod_actual_column_count = m_terrain_lod_scene ? m_terrain_lod_scene->actual_column_count : 0U,
            .terrain_lod_uploaded_face_count = m_terrain_lod_uploaded_face_count,
            .terrain_lod_reused_group_count = m_terrain_lod_reused_group_count,
            .terrain_lod_upload_pending = m_pending_terrain_lod_scene != nullptr || m_terrain_lod_upload_planner.pending(),
            .diagnostic_height_tile_removal = m_diagnostic_height_tile_removal,
            .stone_face_capacity = requiredValue(m_stone_face_buffer).capacity(),
            .stone_face_growth = m_stone_face_growth,
            .diagnostic_render_attempt_id = m_diagnostic_render_attempt_id,
            .diagnostic_frame_recorded = m_diagnostic_frame_recorded,
            .diagnostic_indexed_stone_quads = m_stone_quad_mode != StoneIndirectDraws::QuadMode::SixVertex,
            .diagnostic_stone_indirect = m_diagnostic_stone_indirect,
            .diagnostic_frame_slot = m_diagnostic_frame_recorded
                ? std::optional<uint32_t>{ m_current_frame_slot } : std::nullopt,
            .diagnostic_submitted_stone_quad_count = m_diagnostic_submitted_stone_quad_count,
            .diagnostic_stone_draw_count = m_diagnostic_frame_recorded ? m_chunk_draw_count : 0U,
            .diagnostic_camera = {
                m_camera.pose().position.x,
                m_camera.pose().position.y,
                m_camera.pose().position.z,
                m_camera.pose().angles.yaw_degrees,
                m_camera.pose().angles.pitch_degrees,
                m_camera.pose().angles.roll_degrees,
                m_camera.projection().vertical_fov_degrees,
            },
            .diagnostic_gpu_timestamps_enabled = m_diagnostic_timestamp_valid_bits != 0U,
            .diagnostic_gpu_timestamp_reason = m_diagnostic_gpu_timestamp_reason,
            .diagnostic_gpu_sample_attempt_id = m_diagnostic_gpu_sample_attempt_id,
            .diagnostic_gpu_sample_slot = m_diagnostic_gpu_sample_slot,
            .diagnostic_gpu_sample_quad_count = m_diagnostic_gpu_sample_quad_count,
            .diagnostic_gpu_sample_draw_count = m_diagnostic_gpu_sample_draw_count,
            .diagnostic_gpu_terrain_duration = m_diagnostic_gpu_terrain_duration,
            .diagnostic_terrain_occlusion_enabled = m_occlusion_enabled,
            .diagnostic_terrain_occlusion_oracle_frame = m_occlusion_oracle_frame,
            .diagnostic_terrain_occlusion_filtered_frame = m_occlusion_filtered_frame,
            .diagnostic_terrain_occlusion_removed_quad_count = m_occlusion_removed_quads,
            .diagnostic_terrain_occlusion_removed_range_count = m_occlusion_removed_ranges,
            .diagnostic_terrain_occlusion_removed_command_count = m_occlusion_removed_commands,
        };
    }

    [[nodiscard]] core::graphics::vulkan::PresentationHistoryResult queryPresentationHistory(
        std::span<core::graphics::vulkan::PresentationTiming> const records
    )
    {
        return m_context->queryPresentationHistory(records);
    }

    void recreate(VkExtent2D const extent, std::chrono::steady_clock::time_point const deadline)
    {
        m_recreation_resources_destroyed = false;
        m_context->recreate(
            extent,
            {
                .before = &Impl::beforeRecreate,
                .after = &Impl::afterRecreate,
                .user_data = this,
            },
            remaining(deadline)
        );
    }

    [[nodiscard]] uint32_t validationErrorCount() const noexcept
    {
        return m_context->validationErrorCount();
    }

private:
    [[nodiscard]]
    std::optional<StoneDrawRange> appendUnindexedRanges(
        VulkanFrustum const& frustum,
        glm::dvec3 const camera_position
    )
    {
        if (m_height_tile_draw_index_valid && !m_height_tile_draw_index.hasFallbackRecords()) {
            return std::nullopt;
        }
        std::optional<StoneDrawRange> fallback;
        for (auto const& [coordinate, slot] : m_height_tile_slots) {
            if (m_height_tile_draw_index_valid && coordinate.x >= 0 && coordinate.y >= 0) {
                continue;
            }
            WrappedBounds const wrapped = boundsNearestToCamera(slot.minimum, slot.maximum, camera_position);
            if (!frustum.intersects(wrapped.minimum, wrapped.maximum)) {
                continue;
            }
            if (coordinate.x < 0 || coordinate.y < 0) {
                fallback = slot.range;
            } else {
                HeightTileDrawIndex::appendVisibleRanges(
                    slot.range,
                    m_directional_stone_culling ? slot.directional_spans : HeightTileDrawIndex::DirectionalSpans{},
                    camera_position,
                    m_visible_stone_draw_ranges
                );
            }
        }
        return fallback;
    }

    void indexHeightTileSlot(shared::HeightTileCoordinate const coordinate, HeightTileSlot const& slot)
    {
        if (!m_height_tile_draw_index_valid) {
            return;
        }
        try {
            m_height_tile_draw_index.upsert({
                .coordinate = coordinate,
                .range = slot.range,
                .minimum = slot.minimum,
                .maximum = slot.maximum,
                .directional_spans = m_directional_stone_culling ? slot.directional_spans
                    : HeightTileDrawIndex::DirectionalSpans{},
            });
        } catch (std::bad_alloc const&) {
            // Optional CPU indexing must not prevent already-published geometry from drawing.
            m_height_tile_draw_index.clear();
            m_height_tile_draw_index_valid = false;
        }
    }

    [[nodiscard]] HeightTileSlot* heightTileSlot(shared::HeightTileCoordinate const coordinate) noexcept
    {
        auto const existing = m_height_tile_slots.find(coordinate);
        return existing == m_height_tile_slots.end() ? nullptr : &existing->second;
    }

    [[nodiscard]] StoneDrawRange allocateRange(uint32_t const instance_count)
    {
        if (instance_count == 0U || instance_count > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
            throw std::invalid_argument("height tile mesh has an invalid quad count");
        }
        reclaimRetiredRanges();
        auto const reusable = std::find_if(
            m_free_stone_ranges.begin(),
            m_free_stone_ranges.end(),
            [this, instance_count](StoneDrawRange const range) {
                return range.instance_count >= instance_count
                    && range.first_instance >= m_stone_faces.size();
            }
        );
        if (reusable != m_free_stone_ranges.end()) {
            StoneDrawRange const allocation{ .first_instance = reusable->first_instance, .instance_count = instance_count };
            reusable->first_instance += instance_count;
            reusable->instance_count -= instance_count;
            if (reusable->instance_count == 0U) {
                m_free_stone_ranges.erase(reusable);
            }
            return allocation;
        }
        m_next_stone_face = std::max(m_next_stone_face, static_cast<uint32_t>(m_stone_faces.size()));
        if (m_next_stone_face > MAXIMUM_RENDERED_STONE_FACE_COUNT - instance_count) {
            throw std::runtime_error("height tile mesh arena is full");
        }
        StoneDrawRange const allocation{
            .first_instance = m_next_stone_face,
            .instance_count = instance_count,
        };
        m_next_stone_face += instance_count;
        return allocation;
    }

    void releaseRange(StoneDrawRange const range)
    {
        if (range.instance_count == 0U) {
            return;
        }
        m_retired_stone_ranges.push_back({
            .range = range,
            .reusable_after_frame = m_submitted_frame_count + 8U,
        });
    }

    void makeRangeReusable(StoneDrawRange const range)
    {
        m_free_stone_ranges.push_back(range);
        std::ranges::sort(m_free_stone_ranges, {}, &StoneDrawRange::first_instance);
        auto output = m_free_stone_ranges.begin();
        for (auto current = m_free_stone_ranges.begin(); current != m_free_stone_ranges.end(); ++current) {
            if (output != m_free_stone_ranges.begin()
                && std::prev(output)->first_instance + std::prev(output)->instance_count == current->first_instance) {
                std::prev(output)->instance_count += current->instance_count;
            } else {
                *output = *current;
                ++output;
            }
        }
        m_free_stone_ranges.erase(output, m_free_stone_ranges.end());
    }

    static void mergeReusableRanges(
        std::vector<StoneDrawRange>& free_ranges,
        std::vector<StoneDrawRange> const& reclaimed_ranges
    )
    {
        if (reclaimed_ranges.empty()) {
            return;
        }
        if (free_ranges.size() > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
            throw std::logic_error("reusable stone range count exceeds the arena capacity");
        }
        uint32_t const existing_range_count = static_cast<uint32_t>(free_ranges.size());
        free_ranges.reserve(free_ranges.size() + reclaimed_ranges.size());
        free_ranges.insert(free_ranges.end(), reclaimed_ranges.begin(), reclaimed_ranges.end());
        std::inplace_merge(
            free_ranges.begin(),
            free_ranges.begin() + existing_range_count,
            free_ranges.end(),
            [](StoneDrawRange const left, StoneDrawRange const right) {
                return left.first_instance < right.first_instance;
            }
        );
        auto output = free_ranges.begin();
        for (auto current = free_ranges.begin(); current != free_ranges.end(); ++current) {
            if (output != free_ranges.begin()) {
                StoneDrawRange& previous = *std::prev(output);
                uint32_t const previous_end = previous.first_instance + previous.instance_count;
                if (previous_end == current->first_instance) {
                    previous.instance_count += current->instance_count;
                    continue;
                }
                ASSERT(previous_end < current->first_instance, "reusable stone ranges overlap");
            }
            *output = *current;
            ++output;
        }
        free_ranges.erase(output, free_ranges.end());
    }

    void reclaimRetiredRanges()
    {
        uint32_t reclaimed_count = 0U;
        auto reclaimed_end = m_retired_stone_ranges.cbegin();
        while (reclaimed_end != m_retired_stone_ranges.cend()
            && reclaimed_end->reusable_after_frame <= m_submitted_frame_count) {
            ++reclaimed_end;
            ++reclaimed_count;
        }
        if (reclaimed_count == 0U) {
            return;
        }
        std::vector<StoneDrawRange> reclaimed_ranges;
        reclaimed_ranges.reserve(reclaimed_count);
        for (auto retired = m_retired_stone_ranges.cbegin(); retired != reclaimed_end; ++retired) {
            reclaimed_ranges.push_back(retired->range);
        }
        std::ranges::sort(reclaimed_ranges, {}, &StoneDrawRange::first_instance);
        mergeReusableRanges(m_free_stone_ranges, reclaimed_ranges);
        for (uint32_t index = 0U; index < reclaimed_count; ++index) {
            m_retired_stone_ranges.pop_front();
        }
    }

    void clearHeightTileMeshes()
    {
        discardPendingTerrainLodScene();
        m_terrain_lod_scene.reset();
        m_terrain_lod_ranges.clear();
        m_pending_stone_face_growth.clear();
        static constexpr uint64_t RETIREMENT_BATCH_SIZE = 128U;
        while (!m_height_tile_slots.empty()) {
            HeightTileSlotRetirement retirement{
                m_height_tile_slot_reaper,
                std::min(RETIREMENT_BATCH_SIZE, static_cast<uint64_t>(m_height_tile_slots.size())),
            };
            while (!m_height_tile_slots.empty() && retirement.nodes.size() < RETIREMENT_BATCH_SIZE) {
                retirement.extract(m_height_tile_slots, m_height_tile_slots.begin());
            }
        }
        m_height_tile_slot_reaper.drain();
        m_height_tile_draw_index.clear();
        m_height_tile_draw_index_valid = true;
        m_height_tile_face_count = 0U;
        m_free_stone_ranges.clear();
        m_retired_stone_ranges.clear();
        m_free_stone_ranges_scratch.clear();
        m_next_stone_face = 0U;
    }

    [[nodiscard]] bool ensureStoneFaceCapacity(uint32_t const required_capacity)
    {
        if (required_capacity > MAXIMUM_RENDERED_STONE_FACE_COUNT) {
            throw std::invalid_argument("stone face arena exceeds its maximum capacity");
        }
        if (required_capacity <= m_stone_face_capacity) {
            return false;
        }
        uint32_t next_capacity = m_stone_face_capacity;
        while (next_capacity < required_capacity) {
            if (next_capacity > MAXIMUM_RENDERED_STONE_FACE_COUNT / 2U) {
                next_capacity = MAXIMUM_RENDERED_STONE_FACE_COUNT;
                break;
            }
            next_capacity *= 2U;
        }
        m_stone_face_capacity = next_capacity;
        recreate(m_context->info().extent, std::chrono::steady_clock::time_point::max());
        return true;
    }

    void recordStoneFaceCapacityRequest(uint32_t const required_capacity) noexcept
    {
        if (m_pending_stone_face_growth.targetCapacity() != 0U) {
            m_pending_stone_face_growth.coalesce(required_capacity);
        }
        m_stone_face_growth.last_required_capacity = required_capacity;
        m_stone_face_growth.required_capacity_high_water = std::max(
            m_stone_face_growth.required_capacity_high_water, required_capacity
        );
    }

    [[nodiscard]] bool tryEnsureStoneFaceCapacity(
        uint32_t const required_capacity,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        if (m_pending_stone_face_growth.targetCapacity() != 0U) {
            m_pending_stone_face_growth.coalesce(required_capacity);
            ++m_stone_face_growth.publication_retry_count;
            return false;
        }
        return growStoneFaceCapacity(required_capacity, deadline);
    }

    [[nodiscard]] bool recordStoneFaceRecreationFailure(std::exception_ptr const& failure)
    {
        return m_pending_stone_face_growth.recordFailure(
            failure,
            m_stone_face_capacity,
            m_recreation_resources_destroyed,
            std::chrono::steady_clock::now()
        ) == detail::StoneFaceRecreationFailure::Failed;
    }

    [[nodiscard]] bool growStoneFaceCapacity(
        uint32_t const required_capacity,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        bool first_attempt = true;
        bool rollback_required = false;
        std::exception_ptr failure;
        bool const capacity_ready = detail::tryGrowStoneFaceCapacity(
            m_stone_face_capacity,
            required_capacity,
            MAXIMUM_RENDERED_STONE_FACE_COUNT,
            deadline,
            [this, &first_attempt, &rollback_required, &failure](
                std::chrono::steady_clock::time_point const frame_deadline
            ) {
                if (!first_attempt && !rollback_required) {
                    return;
                }
                bool const target_attempt = first_attempt;
                first_attempt = false;
                rollback_required = false;
                m_stone_face_growth_in_progress = true;
                try {
                    recreate(m_context->info().extent, frame_deadline);
                    m_stone_face_growth_in_progress = false;
                    if (!m_stone_face_buffer.has_value()
                        || requiredValue(m_stone_face_buffer).capacity() != m_stone_face_capacity) {
                        throw detail::StoneFaceRecreationDeferred{};
                    }
                } catch (...) {
                    m_stone_face_growth_in_progress = false;
                    rollback_required = m_recreation_resources_destroyed;
                    if (target_attempt) {
                        std::exception_ptr const recreation_failure = std::current_exception();
                        if (recordStoneFaceRecreationFailure(recreation_failure)) {
                            failure = recreation_failure;
                        }
                    }
                    throw;
                }
            },
            &m_stone_face_growth
        );
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!capacity_ready) {
            ++m_stone_face_growth.publication_retry_count;
        }
        return capacity_ready;
    }

    void uploadStoredMeshes()
    {
        if (m_terrain_lod_scene) {
            for (uint32_t index = 0U; index < m_terrain_lod_ranges.size(); ++index) {
                requiredValue(m_stone_face_buffer).upload(
                    m_terrain_lod_ranges[index].first_instance,
                    detail::terrainLodGroupFaces(*m_terrain_lod_scene, m_terrain_lod_scene->groups[index])
                );
                m_terrain_lod_uploaded_face_count += m_terrain_lod_ranges[index].instance_count;
            }
        }
        if (m_pending_terrain_lod_scene) {
            for (uint32_t index = 0U; index < m_pending_terrain_lod_ranges.size(); ++index) {
                if (m_pending_terrain_lod_reusable[index] || m_pending_terrain_lod_uploaded[index] == 0U) {
                    continue;
                }
                requiredValue(m_stone_face_buffer).upload(
                    m_pending_terrain_lod_ranges[index].first_instance,
                    detail::terrainLodGroupFaces(*m_pending_terrain_lod_scene, m_pending_terrain_lod_scene->groups[index])
                        .first(m_pending_terrain_lod_uploaded[index])
                );
                m_terrain_lod_uploaded_face_count += m_pending_terrain_lod_uploaded[index];
            }
        }
        if (!m_stone_faces.empty()) {
            requiredValue(m_stone_face_buffer).upload(0U, m_stone_faces);
            ++m_chunk_mesh_upload_count;
        }
        for (auto const& [coordinate, slot] : m_height_tile_slots) {
            static_cast<void>(coordinate);
            requiredValue(m_stone_face_buffer).upload(slot.range.first_instance, slot.instances);
            ++m_chunk_mesh_upload_count;
        }
    }

    static void beforeRecreate(void* const user_data)
    {
        Impl& self = *static_cast<Impl*>(user_data);
        if (self.m_stone_face_growth_in_progress) {
            ++self.m_stone_face_growth.before_recreate_count;
        }
        if (self.m_frame_completion_tracking) {
            // CoreCpp invokes this hook only after every submitted frame fence completed.
            self.m_frame_progress_tracker.observeRecreationCompleted(std::chrono::steady_clock::now());
        }
        self.m_recreation_resources_destroyed = true;
        self.destroyResources();
    }

    static void afterRecreate(
        core::graphics::vulkan::PresentationInfo const&,
        void* const user_data
    )
    {
        Impl& self = *static_cast<Impl*>(user_data);
        self.createResources();
        self.refreshCaptureState();
        self.m_recreation_resources_destroyed = false;
    }

    static void recordFrame(VkCommandBuffer const command, void* const user_data)
    {
        static_cast<Impl*>(user_data)->record(command);
    }

    void record(VkCommandBuffer const command)
    {
        prepareOcclusionQueries(command);
        if (m_current_diagnostic_query_pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(command, m_current_diagnostic_query_pool, 0U, 2U);
        }
        m_debug_hud_draw_count = 0U;
        m_chunk_draw_count = 0U;
        m_text_glyph_upload.clear();
        m_text_glyph_upload.insert(
            m_text_glyph_upload.end(),
            m_world_text_batch.glyphs.begin(),
            m_world_text_batch.glyphs.end()
        );
        for (TextBatch const& text : m_gui_renderer.batches()) {
            m_text_glyph_upload.insert(m_text_glyph_upload.end(), text.glyphs.begin(), text.glyphs.end());
        }
        if (!m_text_glyph_upload.empty()) {
            requiredValue(m_text_glyph_buffer).update(command, m_text_glyph_upload);
        }
        uint32_t const gui_text_first_instance = static_cast<uint32_t>(m_world_text_batch.glyphs.size());
        DepthTarget& depth_target = *m_current_depth_target;
        detail::recordDepthAttachmentBarrier(
            command, depth_target.image, depth_target.layout_initialized, vkCmdPipelineBarrier
        );
        depth_target.layout_initialized = true;
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = m_image_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color.float32[0] = 0.38F;
        color_attachment.clearValue.color.float32[1] = 0.62F;
        color_attachment.clearValue.color.float32[2] = 0.88F;
        color_attachment.clearValue.color.float32[3] = 1.0F;
        VkExtent2D const extent = requiredValue(m_resources).extent();
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth_target.view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = m_occlusion_enabled ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil.depth = 1.0F;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1U;
        rendering.colorAttachmentCount = 1U;
        rendering.pColorAttachments = &color_attachment;
        rendering.pDepthAttachment = &depth_attachment;
        recordMapImageUpload(command);
        m_begin_rendering(command, &rendering);
        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{};
        scissor.extent = extent;
        vkCmdSetViewport(command, 0U, 1U, &viewport);
        vkCmdSetScissor(command, 0U, 1U, &scissor);

        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sky_pipeline);
        vkCmdDraw(command, 3U, 1U, 0U, 0U);


        m_chunk_draw_face_count = 0U;
        if (m_chunk_scene_enabled) {
            std::chrono::steady_clock::time_point const range_collection_started_at =
                std::chrono::steady_clock::now();
            if (m_terrain_lod_scene) {
                m_visible_stone_draw_ranges.clear();
                m_visible_solid_stone_draw_ranges.clear();
                auto const projection = sceneProjection(m_camera, extent, true);
                if (projection) {
                    VulkanFrustum const frustum{ *projection * glm::lookAtRH(
                        glm::vec3(0.0F), glm::vec3(m_camera.forward()), glm::vec3(m_camera.up())
                    ) };
                    glm::dvec3 const camera_position = m_camera.pose().position;
                    for (uint32_t group_index = 0U; group_index < m_terrain_lod_scene->groups.size(); ++group_index) {
                        auto const& group = m_terrain_lod_scene->groups[group_index];
                        glm::vec3 const minimum{
                            static_cast<float>(group.origin_x), static_cast<float>(group.origin_y),
                            static_cast<float>(group.minimum_z),
                        };
                        glm::vec3 const maximum{
                            static_cast<float>(group.origin_x + group.extent),
                            static_cast<float>(group.origin_y + group.extent), static_cast<float>(group.maximum_z),
                        };
                        WrappedBounds const wrapped = curvedBoundsNearestToCamera(minimum, maximum, camera_position);
                        if (!insideWorldVisibility(wrapped, worldVisibilityRadius(camera_position.z))
                            || !frustum.intersects(wrapped.minimum, wrapped.maximum)) {
                            continue;
                        }
                        StoneDrawRange const range = m_terrain_lod_ranges[group_index];
                        appendVisibleStoneRange(m_visible_stone_draw_ranges, range);
                    }
                }
            } else if (m_height_tile_slots.empty()) {
                m_visible_solid_stone_draw_ranges.clear();
                m_visible_stone_draw_ranges.assign(
                    m_stone_faces.empty() ? 0U : 1U,
                    m_legacy_draw_range
                );
            } else {
                m_visible_stone_draw_ranges.clear();
                m_visible_solid_stone_draw_ranges.clear();
                m_visible_stone_draw_ranges.reserve(m_height_tile_slots.size());
                m_visible_solid_stone_draw_ranges.reserve(m_height_tile_slots.size());
                std::optional<StoneDrawRange> fallback_draw_range;
                std::optional<glm::mat4> const projection = m_camera.projectionMatrix(extent.width, extent.height);
                if (projection.has_value()) {
                    glm::dvec3 const camera_position = m_camera.pose().position;
                    VulkanFrustum const frustum{ *projection * m_camera.viewMatrix() };
                    if (m_height_tile_draw_index_valid) {
                        HeightTileDrawIndex::Work const work = m_height_tile_draw_index.collect(
                            frustum, camera_position, m_visible_stone_draw_ranges
                        );
                        m_cpu_height_tile_group_and_tile_traversal_duration = std::chrono::nanoseconds{
                            static_cast<int64_t>(work.group_and_tile_traversal_duration_ns)
                        };
                        m_cpu_height_tile_pending_range_order_duration = std::chrono::nanoseconds{
                            static_cast<int64_t>(work.pending_range_order_duration_ns)
                        };
                        m_cpu_height_tile_visible_range_merge_duration = std::chrono::nanoseconds{
                            static_cast<int64_t>(work.visible_range_merge_duration_ns)
                        };
                    }
                    fallback_draw_range = appendUnindexedRanges(frustum, camera_position);
                }
                auto const merge_ranges = [](std::vector<StoneDrawRange>& ranges) {
                    std::ranges::sort(ranges, {}, &StoneDrawRange::first_instance);
                    auto merged_end = ranges.begin();
                    for (StoneDrawRange const range : ranges) {
                        if (merged_end != ranges.begin()
                            && std::prev(merged_end)->first_instance + std::prev(merged_end)->instance_count
                            == range.first_instance) {
                            std::prev(merged_end)->instance_count += range.instance_count;
                        } else {
                            *merged_end = range;
                            ++merged_end;
                        }
                    }
                    ranges.erase(merged_end, ranges.end());
                };
                if (!m_height_tile_draw_index_valid) {
                    merge_ranges(m_visible_stone_draw_ranges);
                    merge_ranges(m_visible_solid_stone_draw_ranges);
                }
                if (fallback_draw_range.has_value()) {
                    m_visible_stone_draw_ranges.push_back(*fallback_draw_range);
                }
            }
            m_cpu_visible_range_collection_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - range_collection_started_at
            );
            filterOcclusionRanges();
            if (m_current_occlusion_pool != VK_NULL_HANDLE) {
                OcclusionQuerySlot& slot = m_occlusion_query_slots.at(m_current_frame_slot);
                uint64_t const count = m_visible_stone_draw_ranges.size() + m_visible_solid_stone_draw_ranges.size();
                if (!m_occlusion_filtered_frame && count != 0U && count <= slot.capacity) {
                    slot.identity = occlusionIdentity();
                    slot.textured = m_visible_stone_draw_ranges;
                    slot.solid = m_visible_solid_stone_draw_ranges;
                    m_occlusion_oracle_frame = true;
                    m_diagnostic_gpu_terrain_duration.reset();
                }
            }
            std::chrono::steady_clock::time_point const indirect_prepare_started_at =
                std::chrono::steady_clock::now();
            StoneIndirectDraws::Prepared const prepared = requiredValue(m_stone_indirect_draws).prepareAcquiredSlot(
                m_current_frame_slot, m_visible_stone_draw_ranges, m_visible_solid_stone_draw_ranges
            );
            m_cpu_indirect_prepare_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - indirect_prepare_started_at
            );
            m_diagnostic_stone_indirect = prepared.buffer != nullptr;
            for (std::span<StoneDrawRange const> const ranges : {
                std::span<StoneDrawRange const>{ m_visible_stone_draw_ranges },
                std::span<StoneDrawRange const>{ m_visible_solid_stone_draw_ranges },
            }) {
                for (StoneDrawRange const range : ranges) {
                    m_chunk_draw_face_count += range.instance_count;
                }
            }
            m_diagnostic_submitted_stone_quad_count = m_chunk_draw_face_count;
            std::chrono::steady_clock::time_point const stone_scene_record_started_at =
                std::chrono::steady_clock::now();
            m_chunk_draw_count = recordStoneScene(
                command,
                m_stone_pipeline,
                m_stone_solid_pipeline,
                m_stone_layout,
                stoneDescriptorForFrame(),
                m_player_pipeline,
                m_player_overlay_pipeline,
                m_player_layout,
                m_camera,
                m_occlusion_oracle_frame ? std::span<PlayerRenderData const>{} : m_players,
                m_visible_stone_draw_ranges,
                m_visible_solid_stone_draw_ranges,
                extent,
                !m_context->info().srgb,
                prepared,
                m_stone_quad_indices ? m_stone_quad_indices->handle() : VK_NULL_HANDLE,
                m_occlusion_oracle_frame ? VK_NULL_HANDLE : m_current_diagnostic_query_pool,
                m_stone_quad_mode,
                VK_NULL_HANDLE,
                m_terrain_lod_scene != nullptr,
                m_terrain_lod_scene ? static_cast<float>(m_terrain_lod_scene->block_sea_level) : 0.0F
            );
            if (m_occlusion_oracle_frame) {
                m_end_rendering(command);
                VkMemoryBarrier const barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                };
                VkPipelineStageFlags const stages = VkPipelineStageFlags{ VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT }
                    | VkPipelineStageFlags{ VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT }
                    | VkPipelineStageFlags{ VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
                vkCmdPipelineBarrier(command, stages, stages, 0U, 1U, &barrier, 0U, nullptr, 0U, nullptr);
                depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                m_begin_rendering(command, &rendering);
                static_cast<void>(recordStoneScene(
                    command, m_stone_occlusion_pipeline, m_stone_solid_occlusion_pipeline,
                    m_stone_layout, stoneDescriptorForFrame(), m_player_pipeline,
                    m_player_overlay_pipeline, m_player_layout, m_camera, m_players,
                    m_visible_stone_draw_ranges, m_visible_solid_stone_draw_ranges, extent,
                    !m_context->info().srgb,
                    {},
                    m_stone_quad_indices ? m_stone_quad_indices->handle() : VK_NULL_HANDLE,
                    VK_NULL_HANDLE, m_stone_quad_mode, m_current_occlusion_pool, m_terrain_lod_scene != nullptr,
                    m_terrain_lod_scene ? static_cast<float>(m_terrain_lod_scene->block_sea_level) : 0.0F
                ));
            }
            m_cpu_stone_scene_record_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - stone_scene_record_started_at
            );
        } else if (m_fallback_scene_enabled) {
            recordFlat3dScene(
                command,
                m_grid_pipeline,
                m_grid_layout,
                m_player_pipeline,
                m_player_overlay_pipeline,
                m_player_layout,
                m_camera,
                m_players,
                extent
            );
        }
        if (!m_world_text_batch.glyphs.empty()) {
            std::optional<glm::mat4> const projection = sceneProjection(m_camera, extent, m_terrain_lod_scene != nullptr);
            if (projection.has_value()) {
                uint32_t world_text_draw_count = 0U;
                recordText(
                    command,
                    m_world_text_pipeline,
                    m_world_text_layout,
                    requiredValue(m_text_descriptors).set(),
                    m_world_text_batch,
                    m_terrain_lod_scene ? *projection * glm::lookAtRH(
                        glm::vec3(0.0F), glm::vec3(m_camera.forward()), glm::vec3(m_camera.up())
                    ) : *projection * m_camera.viewMatrix(),
                    extent,
                    textShaderMode(m_world_text_batch.space),
                    0U,
                    world_text_draw_count,
                    glm::vec4(glm::vec3(m_camera.pose().position), m_terrain_lod_scene ? 1.0F : 0.0F)
                );
            }
        }
        recordMapImage(command, extent);
        m_end_rendering(command);
        if (!m_gui_renderer.empty()) {
            VkMemoryBarrier color_barrier{};
            color_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            color_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            color_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(
                command,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0U,
                1U,
                &color_barrier,
                0U,
                nullptr,
                0U,
                nullptr
            );
            VkRenderingAttachmentInfo gui_color_attachment{};
            gui_color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            gui_color_attachment.imageView = m_image_view;
            gui_color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            gui_color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            gui_color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo gui_rendering{};
            gui_rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            gui_rendering.renderArea.extent = extent;
            gui_rendering.layerCount = 1U;
            gui_rendering.colorAttachmentCount = 1U;
            gui_rendering.pColorAttachments = &gui_color_attachment;
            m_begin_rendering(command, &gui_rendering);
            vkCmdSetViewport(command, 0U, 1U, &viewport);
            vkCmdSetScissor(command, 0U, 1U, &scissor);
            uint32_t first_instance = gui_text_first_instance;
            for (TextBatch const& text : m_gui_renderer.batches()) {
                recordText(
                    command,
                    m_gui_text_pipeline,
                    m_gui_text_layout,
                    requiredValue(m_text_descriptors).set(),
                    text,
                    glm::mat4{ 1.0F },
                    extent,
                    textShaderMode(text.space),
                    first_instance,
                    m_debug_hud_draw_count
                );
                first_instance += static_cast<uint32_t>(text.glyphs.size());
            }
            m_end_rendering(command);
        }
    }

    void recordMapImage(VkCommandBuffer const command, VkExtent2D const extent)
    {
        if (m_map_image_data.empty()) {
            return;
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_map_image_pipeline);
        VkDescriptorSet const descriptor_set = requiredValue(m_map_image_texture).set();
        vkCmdBindDescriptorSets(
            command,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_map_image_pipeline_layout,
            0U,
            1U,
            &descriptor_set,
            0U,
            nullptr
        );
        MapImagePushConstants const push_constants{
            .viewport_size = { extent.width, extent.height },
            .image_size = { m_map_image_data.width(), m_map_image_data.height() },
            .encode_srgb = m_context->info().srgb ? 0U : 1U,
        };
        vkCmdPushConstants(
            command,
            m_map_image_pipeline_layout,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0U,
            static_cast<uint32_t>(sizeof(push_constants)),
            &push_constants
        );
        vkCmdDraw(command, 3U, 1U, 0U, 0U);
    }

    void recordMapImageUpload(VkCommandBuffer const command)
    {
        if (m_map_image_data.empty()
            || m_map_image_data.revision() == m_map_image_uploaded_revision) {
            return;
        }
        m_map_image_recorded_revision = m_map_image_data.revision();
        m_map_image_recorded_layout = requiredValue(m_map_image_texture).recordUpload(
            command,
            m_current_frame_slot,
            m_map_image_data,
            m_map_texture_layout
        );
    }

    [[nodiscard]] VkPipelineLayout createLayout(uint32_t const push_constant_size) const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0U;
        range.size = push_constant_size;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        checkResult(vkCreatePipelineLayout(requiredValue(m_resources).device(), &info, nullptr, &layout), "vkCreatePipelineLayout");
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createTextLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(TextPushConstants);
        VkDescriptorSetLayout const descriptor_layout = requiredValue(m_text_descriptors).layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(vkCreatePipelineLayout(requiredValue(m_resources).device(), &info, nullptr, &layout),
            "vkCreatePipelineLayout text");
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createMapImageLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.size = static_cast<uint32_t>(sizeof(MapImagePushConstants));
        VkDescriptorSetLayout const descriptor_layout = requiredValue(m_map_image_texture).layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(vkCreatePipelineLayout(requiredValue(m_resources).device(), &info, nullptr, &layout),
            "vkCreatePipelineLayout map");
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createStoneLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.size = sizeof(StonePushConstants);
        VkDescriptorSetLayout const descriptor_layout = requiredValue(m_stone_descriptors).layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(
            vkCreatePipelineLayout(requiredValue(m_resources).device(), &info, nullptr, &layout),
            "vkCreatePipelineLayout stone"
        );
        return layout;
    }

    [[nodiscard]] OcclusionIdentity occlusionIdentity() const noexcept
    {
        VkExtent2D const extent = requiredValue(m_resources).extent();
        return { m_camera.pose(), m_camera.projection(), m_occlusion_mesh_generation, extent.width, extent.height };
    }

    void prepareOcclusionQueries(VkCommandBuffer const command)
    {
        m_current_occlusion_pool = VK_NULL_HANDLE;
        if (!m_occlusion_enabled) {
            return;
        }
        OcclusionIdentity const identity = occlusionIdentity();
        if (m_occlusion_results && m_occlusion_results->identity != identity) {
            m_occlusion_results.reset();
        }
        OcclusionQuerySlot& slot = m_occlusion_query_slots[m_current_frame_slot];
        VkDevice const device = requiredValue(m_resources).device();
        if (slot.submitted) {
            uint32_t const count = static_cast<uint32_t>(slot.textured.size() + slot.solid.size());
            std::vector<std::array<uint64_t, 2>> samples(count);
            VkResult const result = vkGetQueryPoolResults(
                device, slot.pool, 0U, count, samples.size() * sizeof(samples.front()), samples.data(),
                sizeof(samples.front()), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
            );
            if (result == VK_SUCCESS && slot.identity == identity) {
                m_occlusion_results = OcclusionResults{ slot.identity, slot.textured, slot.solid, std::move(samples) };
            } else if (result != VK_SUCCESS && result != VK_NOT_READY) {
                m_occlusion_enabled = false;
                m_occlusion_results.reset();
            }
            slot.submitted = false;
        }
        if (!m_occlusion_enabled || m_occlusion_results) {
            return;
        }
        uint64_t const count = m_visible_stone_draw_ranges.size() + m_visible_solid_stone_draw_ranges.size();
        static constexpr uint32_t MAXIMUM_QUERY_COUNT = 1'048'576U;
        if (count == 0U || count > MAXIMUM_QUERY_COUNT) {
            return;
        }
        if (slot.capacity < count) {
            if (slot.pool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device, slot.pool, nullptr);
                slot.pool = VK_NULL_HANDLE;
            }
            VkQueryPoolCreateInfo const info{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0U,
                .queryType = VK_QUERY_TYPE_OCCLUSION,
                .queryCount = static_cast<uint32_t>(count),
                .pipelineStatistics = 0U,
            };
            if (vkCreateQueryPool(device, &info, nullptr, &slot.pool) != VK_SUCCESS) {
                m_occlusion_enabled = false;
                return;
            }
            slot.capacity = static_cast<uint32_t>(count);
        }
        vkCmdResetQueryPool(command, slot.pool, 0U, slot.capacity);
        m_current_occlusion_pool = slot.pool;
    }

    void filterOcclusionRanges()
    {
        if (!m_occlusion_enabled || !m_occlusion_results || m_occlusion_results->identity != occlusionIdentity()) {
            return;
        }
        auto const same_ranges = [](std::vector<StoneDrawRange> const& left, std::vector<StoneDrawRange> const& right) {
            return std::ranges::equal(left, right, [](StoneDrawRange const a, StoneDrawRange const b) {
                return a.first_instance == b.first_instance && a.instance_count == b.instance_count;
            });
        };
        if (!same_ranges(m_visible_stone_draw_ranges, m_occlusion_results->textured)
            || !same_ranges(m_visible_solid_stone_draw_ranges, m_occlusion_results->solid)) {
            m_occlusion_results.reset();
            return;
        }
        uint32_t query_index = 0U;
        auto const filter = [&](std::vector<StoneDrawRange>& ranges) {
            auto output = ranges.begin();
            for (StoneDrawRange const range : ranges) {
                std::array<uint64_t, 2> const sample = m_occlusion_results->samples.at(query_index++);
                if (detail::diagnosticTerrainRangeVisible(true, true, VK_SUCCESS, sample[0], sample[1])) {
                    *output++ = range;
                } else {
                    m_occlusion_removed_quads += range.instance_count;
                    ++m_occlusion_removed_ranges;
                    m_occlusion_removed_commands += m_stone_quad_mode == StoneIndirectDraws::QuadMode::IndexedBlock
                        ? range.instance_count / StoneIndirectDraws::QUADS_PER_INDEX_BLOCK
                            + (range.instance_count % StoneIndirectDraws::QUADS_PER_INDEX_BLOCK != 0U ? 1U : 0U)
                        : 1U;
                }
            }
            ranges.erase(output, ranges.end());
        };
        filter(m_visible_stone_draw_ranges);
        filter(m_visible_solid_stone_draw_ranges);
        m_occlusion_filtered_frame = true;
    }

    void initializeDiagnosticTimestamps(VkPhysicalDeviceProperties const& properties)
    {
        m_diagnostic_gpu_timestamp_reason = "not requested or timestamp capability unsupported";
        std::optional<std::string> const requested = environmentVariable("MC_DIAGNOSTIC_GPU_TIMESTAMPS");
        if (!requested.has_value() || *requested != "1"
            || !std::isfinite(properties.limits.timestampPeriod) || properties.limits.timestampPeriod <= 0.0F) {
            return;
        }
        VkPhysicalDeviceDriverProperties driver_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
            .pNext = nullptr,
            .driverID = VK_DRIVER_ID_AMD_PROPRIETARY,
            .driverName = {},
            .driverInfo = {},
            .conformanceVersion = {},
        };
        VkPhysicalDeviceProperties2 driver_query{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver_properties,
            .properties = {},
        };
        vkGetPhysicalDeviceProperties2(requiredValue(m_resources).physicalDevice(), &driver_query);
        if (driver_properties.driverID == VK_DRIVER_ID_MOLTENVK) {
            // Stage-based Metal counters may defer both terrain markers to the end of one render encoder.
            m_diagnostic_gpu_timestamp_reason = "unsupported: MoltenVK may defer in-render-pass timestamps";
            return;
        }
        uint32_t family_count = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties(requiredValue(m_resources).physicalDevice(), &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(requiredValue(m_resources).physicalDevice(), &family_count, families.data());
        uint32_t common_valid_bits = 64U;
        bool found_graphics = false;
        for (uint32_t index = 0U; index < family_count; ++index) {
            VkQueueFamilyProperties const& family = families[index];
            if (family.queueCount == 0U || (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                continue;
            }
            found_graphics = true;
            if (family.timestampValidBits == 0U) {
                return;
            }
            common_valid_bits = std::min(common_valid_bits, family.timestampValidBits);
        }
        if (found_graphics) {
            m_diagnostic_timestamp_valid_bits = common_valid_bits;
            m_diagnostic_timestamp_period = properties.limits.timestampPeriod;
            m_diagnostic_gpu_timestamp_reason = "enabled: asynchronous terrain interval, not whole frame";
        }
    }

    void acquireDiagnosticQuerySlot()
    {
        if (m_diagnostic_timestamp_valid_bits == 0U) {
            return;
        }
        DiagnosticQuerySlot& slot = m_diagnostic_query_slots[m_current_frame_slot];
        if (slot.pool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo const info{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0U,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 2U,
                .pipelineStatistics = 0U,
            };
            checkResult(vkCreateQueryPool(requiredValue(m_resources).device(), &info, nullptr, &slot.pool),
                "vkCreateQueryPool diagnostic terrain timestamps");
        }
        if (slot.submitted) {
            std::array<uint64_t, 2> values{};
            VkResult const result = vkGetQueryPoolResults(
                requiredValue(m_resources).device(), slot.pool, 0U, 2U, sizeof(values), values.data(), sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT
            );
            m_diagnostic_gpu_terrain_duration.reset();
            m_diagnostic_gpu_sample_attempt_id = slot.attempt_id;
            m_diagnostic_gpu_sample_slot = m_current_frame_slot;
            m_diagnostic_gpu_sample_quad_count = slot.quad_count;
            m_diagnostic_gpu_sample_draw_count = slot.draw_count;
            if (result == VK_SUCCESS) {
                uint64_t const mask = m_diagnostic_timestamp_valid_bits == 64U
                    ? std::numeric_limits<uint64_t>::max() : (uint64_t{ 1U } << m_diagnostic_timestamp_valid_bits) - 1U;
                uint64_t const elapsed_ticks = (values[1] - values[0]) & mask;
                long double const elapsed_ns = static_cast<long double>(elapsed_ticks) * m_diagnostic_timestamp_period;
                if (std::isfinite(elapsed_ns)
                    && elapsed_ns <= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
                    m_diagnostic_gpu_terrain_duration = std::chrono::nanoseconds{ static_cast<int64_t>(elapsed_ns) };
                }
            } else if (result != VK_NOT_READY) {
                checkResult(result, "vkGetQueryPoolResults diagnostic terrain timestamps");
            }
            slot.submitted = false;
        }
        m_current_diagnostic_query_pool = slot.pool;
    }

    void createResources()
    {
        m_resources.emplace(m_context->resources());
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(requiredValue(m_resources).physicalDevice(), &properties);
        if (m_gpu_name == "presentation-context" && properties.deviceName[0] != '\0') {
            m_gpu_name = properties.deviceName;
        }
        initializeDiagnosticTimestamps(properties);
        VkDevice const indirect_device = requiredValue(m_resources).device();
        VkPhysicalDevice const indirect_physical_device = requiredValue(m_resources).physicalDevice();
        CORE_INFO("Stone draw mode={} quads_per_index_block={}",
            static_cast<uint32_t>(m_stone_quad_mode), StoneIndirectDraws::QUADS_PER_INDEX_BLOCK);
        if (m_stone_quad_mode != StoneIndirectDraws::QuadMode::SixVertex) {
            m_stone_quad_indices.emplace(indirect_device, indirect_physical_device, 1U, StoneDrawBufferKind::QuadIndices);
        }
        StoneDrawBufferKind const indirect_kind = m_stone_quad_mode != StoneIndirectDraws::QuadMode::SixVertex
            ? StoneDrawBufferKind::IndexedIndirect : StoneDrawBufferKind::Indirect;
        std::optional<std::string> const direct_requested = environmentVariable("MC_DIAGNOSTIC_DIRECT_STONE_DRAWS");
        bool const force_direct = direct_requested.has_value() && *direct_requested == "1";
        m_stone_indirect_draws.emplace(
            MAXIMUM_RENDERED_STONE_FACE_COUNT,
            properties.limits.maxDrawIndirectCount,
            m_context->info().multi_draw_indirect_enabled && !force_direct,
            m_context->info().draw_indirect_first_instance_enabled,
            [indirect_device, indirect_physical_device, indirect_kind](uint32_t const capacity) {
                return std::make_unique<VulkanStoneDrawBuffer>(
                    indirect_device, indirect_physical_device, capacity, indirect_kind
                );
            },
            m_stone_quad_mode
        );
        auto const dynamic_rendering = requiredValue(m_resources).dynamicRenderingCommands();
        m_begin_rendering = dynamic_rendering.begin;
        m_end_rendering = dynamic_rendering.end;
        if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
            throw std::runtime_error("Vulkan presentation device does not expose dynamic rendering commands");
        }
        selectDepthFormat();
        m_stone_face_buffer.emplace(
            requiredValue(m_resources).device(),
            requiredValue(m_resources).physicalDevice(),
            m_stone_face_capacity
        );
        m_stone_face_growth.actual_capacity = requiredValue(m_stone_face_buffer).capacity();
        m_stone_face_growth.actual_capacity_high_water = std::max(
            m_stone_face_growth.actual_capacity_high_water, m_stone_face_growth.actual_capacity
        );
        m_stone_texture_buffer.emplace(requiredValue(m_resources).device(), requiredValue(m_resources).physicalDevice());
        m_disabled_terrain_field = std::make_shared<CanonicalTerrainFieldBuffer>(
            requiredValue(m_resources).device(), requiredValue(m_resources).physicalDevice());
        m_active_terrain_field = fieldForScene(m_terrain_lod_scene);
        m_pending_terrain_field = fieldForScene(m_pending_terrain_lod_scene);
        m_stone_descriptors.emplace(requiredValue(m_resources).device(), requiredValue(m_stone_face_buffer),
            requiredValue(m_stone_texture_buffer), *m_disabled_terrain_field);
        m_text_glyph_buffer.emplace(requiredValue(m_resources).device(), requiredValue(m_resources).physicalDevice());
        m_text_descriptors.emplace(requiredValue(m_resources).device(), requiredValue(m_text_glyph_buffer));
        m_map_image_texture.emplace(
            requiredValue(m_resources).device(),
            requiredValue(m_resources).physicalDevice()
        );
        m_map_texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_map_image_uploaded_revision = 0U;
        m_map_image_recorded_revision = 0U;
        uploadStoredMeshes();
        auto const grid = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("grid.vert.spv")
        );
        auto const player = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("player.vert.spv")
        );
        auto const text_vertex = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("text.vert.spv")
        );
        auto const stone = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load(stoneVertexShaderName(m_stone_quad_mode))
        );
        auto const fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("trivial.frag.spv")
        );
        auto const text_fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("text.frag.spv")
        );
        auto const stone_fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("stone.frag.spv")
        );
        auto const stone_solid_fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("stone_solid.frag.spv")
        );
        auto const sky_vertex = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("sky.vert.spv")
        );
        auto const sky_fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("sky.frag.spv")
        );
        auto const map_vertex = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("map.vert.spv")
        );
        auto const map_fragment = std::make_shared<core::kernel::SpirvModule const>(
            m_shader_assets.load("map.frag.spv")
        );
        m_grid_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = grid,
                .entrypoint = "main",
                .required_bindings = {},
            },
            {
                .module = fragment,
                .entrypoint = "main",
                .required_bindings = {},
            }
        ));
        m_player_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = player,
                .entrypoint = "main",
                .required_bindings = {},
            },
            {
                .module = std::make_shared<core::kernel::SpirvModule const>(m_shader_assets.load("player.frag.spv")),
                .entrypoint = "main",
                .required_bindings = {},
            }
        ));
        m_text_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = text_vertex,
                .entrypoint = "main",
                .required_bindings = { { .set = 0U, .binding = 0U } },
            },
            {
                .module = text_fragment,
                .entrypoint = "main",
                .required_bindings = {},
            }
        ));
        m_stone_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = stone,
                .entrypoint = "main",
                .required_bindings = { { .set = 0U, .binding = 0U } },
            },
            {
                .module = stone_fragment,
                .entrypoint = "main",
                .required_bindings = { { .set = 0U, .binding = 1U }, { .set = 0U, .binding = 2U } },
            }
        ));
        m_stone_solid_program.emplace(core::kernel::GraphicsProgram::create(
            {
                .module = stone,
                .entrypoint = "main",
                .required_bindings = { { .set = 0U, .binding = 0U } },
            },
            {
                .module = stone_solid_fragment,
                .entrypoint = "main",
                .required_bindings = { { .set = 0U, .binding = 1U }, { .set = 0U, .binding = 2U } },
            }
        ));
        m_sky_program.emplace(core::kernel::GraphicsProgram::create(
            { .module = sky_vertex, .entrypoint = "main", .required_bindings = {} },
            { .module = sky_fragment, .entrypoint = "main", .required_bindings = {} }
        ));
        m_map_image_program.emplace(core::kernel::GraphicsProgram::create(
            { .module = map_vertex, .entrypoint = "main", .required_bindings = {} },
            { .module = map_fragment, .entrypoint = "main", .required_bindings = { { .set = 0U, .binding = 0U } } }
        ));
        m_grid_layout = createLayout(static_cast<uint32_t>(sizeof(GridPushConstants)));
        m_player_layout = createLayout(static_cast<uint32_t>(sizeof(BoxPushConstants)));
        m_world_text_layout = createTextLayout();
        m_gui_text_layout = createTextLayout();
        m_stone_layout = createStoneLayout();
        m_map_image_pipeline_layout = createMapImageLayout();
        m_kernel_cache.emplace(KERNEL_CACHE_CAPACITY);
        core::graphics::vulkan::VulkanDeviceReference const device = requiredValue(m_resources).deviceReference();
        PresentationPipelineDescriptors const descriptors = presentationPipelineDescriptors(
            m_grid_layout,
            m_player_layout,
            m_world_text_layout,
            m_gui_text_layout,
            requiredValue(m_resources).format(),
            m_depth_format
        );
        m_grid_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_grid_program,
            descriptors.grid
        );
        m_player_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_player_program,
            descriptors.player
        );
        m_player_overlay_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_player_program,
            descriptors.player_overlay
        );
        m_world_text_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_text_program,
            descriptors.world_text
        );
        m_gui_text_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_text_program,
            descriptors.gui_text
        );
        m_stone_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_stone_program,
            {
                .layout = m_stone_layout,
                .color_format = requiredValue(m_resources).format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = VK_COMPARE_OP_LESS,
            }
        );
        m_stone_solid_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_stone_solid_program,
            {
                .layout = m_stone_layout,
                .color_format = requiredValue(m_resources).format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = VK_COMPARE_OP_LESS,
            }
        );
        if (m_occlusion_enabled) {
            try {
                core::graphics::vulkan::PipelineDescriptor const descriptor{
                    .layout = m_stone_layout,
                    .color_format = requiredValue(m_resources).format(),
                    .depth_format = m_depth_format,
                    .depth_test_enabled = true,
                    .depth_write_enabled = false,
                    .depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL,
                };
                m_stone_occlusion_pipeline = requiredValue(m_kernel_cache).pipelineFor(
                    device, *m_stone_program, descriptor
                );
                m_stone_solid_occlusion_pipeline = requiredValue(m_kernel_cache).pipelineFor(
                    device, *m_stone_solid_program, descriptor
                );
            } catch (std::exception const&) {
                m_occlusion_enabled = false;
            }
        }
        m_sky_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_sky_program,
            {
                .layout = m_grid_layout,
                .color_format = requiredValue(m_resources).format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = false,
                .depth_write_enabled = false,
            }
        );
        m_map_image_pipeline = requiredValue(m_kernel_cache).pipelineFor(
            device,
            *m_map_image_program,
            {
                .layout = m_map_image_pipeline_layout,
                .color_format = requiredValue(m_resources).format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = false,
                .depth_write_enabled = false,
            }
        );
    }

    void destroyResources() noexcept
    {
        if (!m_resources.has_value()) {
            return;
        }
        VkDevice const device = requiredValue(m_resources).device();
        destroyDepthTargets(device);
        if (m_kernel_cache.has_value()) {
            requiredValue(m_kernel_cache).invalidate(requiredValue(m_resources).deviceReference().identity());
            m_kernel_cache.reset();
        }
        if (m_grid_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_grid_layout, nullptr); }
        if (m_player_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, m_player_layout, nullptr); }
        if (m_world_text_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_world_text_layout, nullptr);
        }
        if (m_gui_text_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_gui_text_layout, nullptr);
        }
        if (m_stone_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_stone_layout, nullptr);
        }
        if (m_map_image_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_map_image_pipeline_layout, nullptr);
        }
        m_grid_pipeline = VK_NULL_HANDLE;
        m_player_pipeline = VK_NULL_HANDLE;
        m_player_overlay_pipeline = VK_NULL_HANDLE;
        m_world_text_pipeline = VK_NULL_HANDLE;
        m_gui_text_pipeline = VK_NULL_HANDLE;
        m_stone_pipeline = VK_NULL_HANDLE;
        m_stone_solid_pipeline = VK_NULL_HANDLE;
        m_stone_occlusion_pipeline = VK_NULL_HANDLE;
        m_stone_solid_occlusion_pipeline = VK_NULL_HANDLE;
        m_sky_pipeline = VK_NULL_HANDLE;
        m_map_image_pipeline = VK_NULL_HANDLE;
        m_grid_layout = VK_NULL_HANDLE;
        m_player_layout = VK_NULL_HANDLE;
        m_world_text_layout = VK_NULL_HANDLE;
        m_gui_text_layout = VK_NULL_HANDLE;
        m_stone_layout = VK_NULL_HANDLE;
        m_map_image_pipeline_layout = VK_NULL_HANDLE;
        m_grid_program.reset();
        m_player_program.reset();
        m_text_program.reset();
        m_stone_program.reset();
        m_stone_solid_program.reset();
        m_sky_program.reset();
        m_map_image_program.reset();
        m_stone_frame_descriptors.clear();
        m_active_terrain_field.reset();
        m_pending_terrain_field.reset();
        m_stone_descriptors.reset();
        m_disabled_terrain_field.reset();
        m_stone_texture_buffer.reset();
        m_stone_face_buffer.reset();
        m_stone_indirect_draws.reset();
        m_stone_quad_indices.reset();
        for (auto const& [slot_index, slot] : m_diagnostic_query_slots) {
            static_cast<void>(slot_index);
            vkDestroyQueryPool(device, slot.pool, nullptr);
        }
        m_diagnostic_query_slots.clear();
        for (auto const& [slot_index, slot] : m_occlusion_query_slots) {
            static_cast<void>(slot_index);
            if (slot.pool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device, slot.pool, nullptr);
            }
        }
        m_occlusion_query_slots.clear();
        m_occlusion_results.reset();
        m_current_occlusion_pool = VK_NULL_HANDLE;
        m_current_diagnostic_query_pool = VK_NULL_HANDLE;
        m_diagnostic_timestamp_valid_bits = 0U;
        m_diagnostic_timestamp_period = 0.0F;
        m_diagnostic_gpu_terrain_duration.reset();
        m_diagnostic_gpu_sample_attempt_id = 0U;
        m_diagnostic_gpu_sample_slot = 0U;
        m_diagnostic_gpu_sample_quad_count = 0U;
        m_diagnostic_gpu_sample_draw_count = 0U;
        m_text_descriptors.reset();
        m_text_glyph_buffer.reset();
        m_map_image_texture.reset();
        m_map_image_uploaded_revision = 0U;
        m_map_image_recorded_revision = 0U;
        m_map_texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_begin_rendering = nullptr;
        m_end_rendering = nullptr;
        m_resources.reset();
    }

    void selectDepthFormat()
    {
        std::array<DepthFormatSupport, DEPTH_FORMAT_CANDIDATES.size()> supported{};
        for (size_t index = 0U; index < DEPTH_FORMAT_CANDIDATES.size(); ++index) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(
                requiredValue(m_resources).physicalDevice(),
                DEPTH_FORMAT_CANDIDATES[index],
                &properties
            );
            supported[index] = {
                .format = DEPTH_FORMAT_CANDIDATES[index],
                .depth_attachment_supported = (properties.optimalTilingFeatures
                    & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U,
            };
        }
        std::optional<VkFormat> const selected = selectDepthAttachmentFormat(supported);
        if (!selected.has_value()) {
            throw std::runtime_error("presentation device has no supported depth attachment format");
        }
        m_depth_format = *selected;
    }

    [[nodiscard]] DepthTarget& depthTargetFor(VkImage const color_image)
    {
        bool const needs_creation = m_depth_target_selection.select(color_image);
        auto const existing = m_depth_targets.find(color_image);
        if (existing != m_depth_targets.end()) {
            if (needs_creation) {
                throw std::logic_error("depth target selection is inconsistent with its resources");
            }
            return existing->second;
        }
        if (!needs_creation) {
            throw std::logic_error("depth target resource is missing for an acquired swapchain image");
        }
        auto const [created, inserted] = m_depth_targets.emplace(color_image, DepthTarget{});
        try {
            createDepthTarget(created->second);
        } catch (std::exception const&) {
            m_depth_targets.erase(created);
            m_depth_target_selection.remove(color_image);
            throw;
        }
        return created->second;
    }

    void createDepthTarget(DepthTarget& target)
    {
        VkDevice const device = requiredValue(m_resources).device();
        try {
            VkImageCreateInfo const image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0U,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = m_depth_format,
                .extent = {
                    .width = requiredValue(m_resources).extent().width,
                    .height = requiredValue(m_resources).extent().height,
                    .depth = 1U,
                },
                .mipLevels = 1U,
                .arrayLayers = 1U,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0U,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            checkResult(vkCreateImage(device, &image_info, nullptr, &target.image), "vkCreateImage depth");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, target.image, &requirements);
            VkPhysicalDeviceMemoryProperties memory_properties{};
            vkGetPhysicalDeviceMemoryProperties(requiredValue(m_resources).physicalDevice(), &memory_properties);
            uint32_t memory_type = memory_properties.memoryTypeCount;
            for (uint32_t index = 0U; index < memory_properties.memoryTypeCount; ++index) {
                if ((requirements.memoryTypeBits & (1U << index)) != 0U
                    && (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                    memory_type = index;
                    break;
                }
            }
            if (memory_type == memory_properties.memoryTypeCount) {
                throw std::runtime_error("presentation device has no local depth-image memory type");
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type;
            checkResult(vkAllocateMemory(device, &allocation, nullptr, &target.memory), "vkAllocateMemory depth");
            checkResult(vkBindImageMemory(device, target.image, target.memory, 0U), "vkBindImageMemory depth");
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = target.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = m_depth_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            checkResult(vkCreateImageView(device, &view_info, nullptr, &target.view), "vkCreateImageView depth");
            target.layout_initialized = false;
        } catch (std::exception const&) {
            destroyDepthTarget(device, target);
            throw;
        }
    }

    static void destroyDepthTarget(VkDevice const device, DepthTarget& target) noexcept
    {
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.view, nullptr);
        }
        if (target.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, target.image, nullptr);
        }
        if (target.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, target.memory, nullptr);
        }
        target.view = VK_NULL_HANDLE;
        target.image = VK_NULL_HANDLE;
        target.memory = VK_NULL_HANDLE;
        target.layout_initialized = false;
    }

    void destroyDepthTargets(VkDevice const device) noexcept
    {
        for (auto& [color_image, target] : m_depth_targets) {
            static_cast<void>(color_image);
            destroyDepthTarget(device, target);
        }
        m_depth_targets.clear();
        m_depth_target_selection.clear();
        m_current_depth_target = nullptr;
    }

    void refreshCaptureState() noexcept
    {
        if (m_last_capture.has_value()) {
            m_capture_state = FrameCaptureState::Completed;
            return;
        }
        m_capture_state = m_options.enable_frame_capture && m_context->info().transfer_source_enabled
            ? FrameCaptureState::Ready
            : m_options.enable_frame_capture
                ? FrameCaptureState::UnsupportedSwapchainUsage
                : FrameCaptureState::Disabled;
    }

    std::shared_ptr<PresentationContext> m_context;
    bool m_recreation_resources_destroyed = false;
    ShaderAssets const& m_shader_assets;
    VulkanRendererOptions m_options;
    std::optional<ResourceScope> m_resources;
    std::optional<core::graphics::vulkan::VulkanKernelCache> m_kernel_cache;
    std::span<PlayerRenderData const> m_players;
    Camera m_camera{
        { .position = { 16.0, -20.0, 22.0 }, .angles = { .pitch_degrees = -35.0 } },
    };
    VkImageView m_image_view = VK_NULL_HANDLE;
    VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
    std::unordered_map<VkImage, DepthTarget> m_depth_targets;
    DepthTargetSelection m_depth_target_selection;
    DepthTarget* m_current_depth_target = nullptr;
    std::optional<core::kernel::GraphicsProgram> m_grid_program;
    std::optional<core::kernel::GraphicsProgram> m_player_program;
    std::optional<core::kernel::GraphicsProgram> m_text_program;
    std::optional<core::kernel::GraphicsProgram> m_stone_program;
    std::optional<core::kernel::GraphicsProgram> m_stone_solid_program;
    std::optional<core::kernel::GraphicsProgram> m_sky_program;
    std::optional<core::kernel::GraphicsProgram> m_map_image_program;
    std::optional<StoneFaceBuffer> m_stone_face_buffer;
    std::optional<StoneIndirectDraws> m_stone_indirect_draws;
    std::optional<VulkanStoneDrawBuffer> m_stone_quad_indices;
    StoneIndirectDraws::QuadMode m_stone_quad_mode = diagnosticStoneQuadMode();
    bool m_directional_stone_culling = diagnosticDirectionalStoneCulling();
    bool m_diagnostic_presentation_stages = diagnosticPresentationStages();
    bool m_frame_completion_tracking = false;
    bool m_occlusion_enabled = environmentVariable("MC_DIAGNOSTIC_TERRAIN_OCCLUSION") == "1";
    bool m_occlusion_oracle_frame = false;
    bool m_occlusion_filtered_frame = false;
    uint64_t m_occlusion_mesh_generation = 0U;
    uint64_t m_occlusion_removed_quads = 0U;
    uint32_t m_occlusion_removed_ranges = 0U;
    uint32_t m_occlusion_removed_commands = 0U;
    std::unordered_map<uint32_t, OcclusionQuerySlot> m_occlusion_query_slots;
    std::optional<OcclusionResults> m_occlusion_results;
    VkQueryPool m_current_occlusion_pool = VK_NULL_HANDLE;
    core::graphics::vulkan::PresentationAcquireDiagnostics m_diagnostic_presentation_acquire{};
    detail::PresentationFrameProgressTracker m_frame_progress_tracker;
    uint64_t m_diagnostic_render_attempt_id = 0U;
    bool m_diagnostic_frame_recorded = false;
    bool m_diagnostic_stone_indirect = false;
    uint64_t m_diagnostic_submitted_stone_quad_count = 0U;
    std::unordered_map<uint32_t, DiagnosticQuerySlot> m_diagnostic_query_slots;
    VkQueryPool m_current_diagnostic_query_pool = VK_NULL_HANDLE;
    uint32_t m_diagnostic_timestamp_valid_bits = 0U;
    std::string_view m_diagnostic_gpu_timestamp_reason = "not requested or timestamp capability unsupported";
    float m_diagnostic_timestamp_period = 0.0F;
    uint64_t m_diagnostic_gpu_sample_attempt_id = 0U;
    uint32_t m_diagnostic_gpu_sample_slot = 0U;
    uint64_t m_diagnostic_gpu_sample_quad_count = 0U;
    uint32_t m_diagnostic_gpu_sample_draw_count = 0U;
    std::optional<std::chrono::nanoseconds> m_diagnostic_gpu_terrain_duration;
    uint32_t m_current_frame_slot = 0U;
    std::optional<StoneTextureBuffer> m_stone_texture_buffer;
    struct StoneFrameDescriptors final {
        std::unique_ptr<StoneDescriptorSet> descriptors;
        std::shared_ptr<CanonicalTerrainFieldBuffer> field;
    };
    std::unordered_map<uint32_t, StoneFrameDescriptors> m_stone_frame_descriptors;
    std::shared_ptr<CanonicalTerrainFieldBuffer> m_disabled_terrain_field;
    std::shared_ptr<CanonicalTerrainFieldBuffer> m_active_terrain_field;
    std::shared_ptr<CanonicalTerrainFieldBuffer> m_pending_terrain_field;
    std::optional<StoneDescriptorSet> m_stone_descriptors;
    std::optional<MapImageTexture> m_map_image_texture;
    MapImageData m_map_image_data;
    uint64_t m_map_image_uploaded_revision = 0U;
    uint64_t m_map_image_recorded_revision = 0U;
    VkImageLayout m_map_texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout m_map_image_recorded_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_world_text_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_gui_text_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_stone_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_map_image_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_overlay_pipeline = VK_NULL_HANDLE;
    VkPipeline m_world_text_pipeline = VK_NULL_HANDLE;
    VkPipeline m_gui_text_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_solid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_occlusion_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_solid_occlusion_pipeline = VK_NULL_HANDLE;
    VkPipeline m_sky_pipeline = VK_NULL_HANDLE;
    VkPipeline m_map_image_pipeline = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR m_begin_rendering = nullptr;
    PFN_vkCmdEndRenderingKHR m_end_rendering = nullptr;
    FrameCaptureState m_capture_state = FrameCaptureState::Disabled;
    std::optional<RendererFrameCapture> m_last_capture;
    std::string m_gpu_name = "presentation-context";
    std::chrono::nanoseconds m_cpu_acquire_wait_duration{ 0 };
    std::chrono::nanoseconds m_cpu_presentation_recreation_duration{ 0 };
    bool m_presentation_recreation_attempted = false;
    bool m_presentation_recreation_deferred = false;
    std::chrono::nanoseconds m_cpu_command_record_duration{ 0 };
    std::chrono::nanoseconds m_cpu_visible_range_collection_duration{ 0 };
    std::chrono::nanoseconds m_cpu_height_tile_group_and_tile_traversal_duration{ 0 };
    std::chrono::nanoseconds m_cpu_height_tile_pending_range_order_duration{ 0 };
    std::chrono::nanoseconds m_cpu_height_tile_visible_range_merge_duration{ 0 };
    std::chrono::nanoseconds m_cpu_indirect_prepare_duration{ 0 };
    std::chrono::nanoseconds m_cpu_stone_scene_record_duration{ 0 };
    std::chrono::nanoseconds m_cpu_command_record_residual_duration{ 0 };
    std::chrono::nanoseconds m_cpu_complete_present_wait_duration{ 0 };
    std::chrono::nanoseconds m_cpu_frame_duration{ 0 };
    uint64_t m_submitted_frame_count = 0U;
    std::shared_ptr<TerrainLodScene const> m_last_submitted_terrain_lod_scene;
    bool m_last_submitted_debug_hud_enabled = false;
    bool m_capture_requested = false;
    DebugHudState m_debug_hud_state;
    DebugHudText m_debug_hud_text;
    TextRenderer m_gui_text;
    TextRenderer m_world_text;
    GuiRenderer m_gui_renderer;
    TextBatch m_world_text_batch;
    std::vector<TextGlyph> m_text_glyph_upload;
    std::optional<TextGlyphBuffer> m_text_glyph_buffer;
    std::optional<TextDescriptorSet> m_text_descriptors;
    bool m_last_presented = false;
    std::optional<uint32_t> m_successful_presentation_id = std::nullopt;
    uint64_t m_successful_presentation_swapchain_generation = 0U;
    uint32_t m_debug_hud_draw_count = 0U;
    std::vector<StoneFaceInstance> m_stone_faces;
    uint32_t m_stone_face_capacity = INITIAL_RENDERED_STONE_FACE_CAPACITY;
    StoneFaceGrowthDiagnostics m_stone_face_growth;
    bool m_stone_face_growth_in_progress = false;
    detail::PendingStoneFaceGrowth m_pending_stone_face_growth;
    StoneDrawRange m_legacy_draw_range;
    std::shared_ptr<TerrainLodScene const> m_terrain_lod_scene;
    std::vector<StoneDrawRange> m_terrain_lod_ranges;
    detail::TerrainLodUploadPlanner m_terrain_lod_upload_planner;
    std::shared_ptr<TerrainLodScene const> m_pending_terrain_lod_scene;
    std::vector<StoneDrawRange> m_pending_terrain_lod_ranges;
    std::vector<std::optional<uint32_t>> m_pending_terrain_lod_reusable;
    std::vector<uint32_t> m_pending_terrain_lod_uploaded;
    uint64_t m_terrain_lod_uploaded_face_count = 0U;
    uint32_t m_terrain_lod_reused_group_count = 0U;
    HeightTileSlots m_height_tile_slots;
    HeightTileInstanceReaper<HeightTileSlotNode> m_height_tile_slot_reaper;
    std::vector<shared::HeightTileCoordinate> m_height_tile_removal_scratch;
    RendererHeightTileRemovalDiagnostics m_diagnostic_height_tile_removal;
    uint32_t m_height_tile_face_count = 0U;
    HeightTileDrawIndex m_height_tile_draw_index;
    bool m_height_tile_draw_index_valid = true;
    std::vector<StoneDrawRange> m_visible_stone_draw_ranges;
    std::vector<StoneDrawRange> m_visible_solid_stone_draw_ranges;
    std::vector<StoneDrawRange> m_free_stone_ranges;
    std::vector<StoneDrawRange> m_free_stone_ranges_scratch;
    std::deque<RetiredStoneRange> m_retired_stone_ranges;
    uint32_t m_next_stone_face = 0U;
    uint64_t m_chunk_mesh_upload_count = 0U;
    uint32_t m_chunk_draw_count = 0U;
    uint32_t m_chunk_draw_face_count = 0U;
    bool m_chunk_scene_enabled = false;
    bool m_fallback_scene_enabled = true;
};

namespace {

constexpr uint32_t OFFSCREEN_WIDTH = 640U;
constexpr uint32_t OFFSCREEN_HEIGHT = 480U;

class OffscreenDepthTarget final {
public:
    OffscreenDepthTarget(
        std::shared_ptr<core::graphics::vulkan::VulkanDevice const> device,
        VkFormat const format,
        VkExtent2D const extent
    )
        : m_device(std::move(device))
        , m_format(format)
    {
        try {
            VkImageCreateInfo const image_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0U,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = m_format,
                .extent = { .width = extent.width, .height = extent.height, .depth = 1U },
                .mipLevels = 1U,
                .arrayLayers = 1U,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0U,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            checkResult(vkCreateImage(m_device->handle(), &image_info, nullptr, &m_image), "vkCreateImage offscreen depth");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_device->handle(), m_image, &requirements);
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(m_device->physicalDevice(), &properties);
            uint32_t memory_type = properties.memoryTypeCount;
            for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
                if ((requirements.memoryTypeBits & (1U << index)) != 0U
                    && (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
                    memory_type = index;
                    break;
                }
            }
            if (memory_type == properties.memoryTypeCount) {
                throw std::runtime_error("offscreen device has no local depth-image memory type");
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type;
            checkResult(vkAllocateMemory(m_device->handle(), &allocation, nullptr, &m_memory), "vkAllocateMemory offscreen depth");
            checkResult(vkBindImageMemory(m_device->handle(), m_image, m_memory, 0U), "vkBindImageMemory offscreen depth");
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = m_image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = m_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            checkResult(vkCreateImageView(m_device->handle(), &view_info, nullptr, &m_view), "vkCreateImageView offscreen depth");
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~OffscreenDepthTarget() { reset(); }
    OffscreenDepthTarget(OffscreenDepthTarget const&) = delete;
    OffscreenDepthTarget& operator=(OffscreenDepthTarget const&) = delete;

    [[nodiscard]] VkImage image() const noexcept { return m_image; }
    [[nodiscard]] VkImageView view() const noexcept { return m_view; }
    [[nodiscard]] VkFormat format() const noexcept { return m_format; }
private:
    void reset() noexcept
    {
        if (!m_device || m_device->handle() == VK_NULL_HANDLE) {
            return;
        }
        if (m_view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device->handle(), m_view, nullptr);
        }
        if (m_image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device->handle(), m_image, nullptr);
        }
        if (m_memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device->handle(), m_memory, nullptr);
        }
        m_view = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    std::shared_ptr<core::graphics::vulkan::VulkanDevice const> m_device;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
};

class OffscreenDrawResources final {
public:
    OffscreenDrawResources(
        std::shared_ptr<core::graphics::vulkan::VulkanDevice const> device,
        ShaderAssets const& shader_assets
    )
        : m_device(std::move(device))
        , m_depth_format(selectDepthFormat())
        , m_depth(m_device, m_depth_format, { .width = OFFSCREEN_WIDTH, .height = OFFSCREEN_HEIGHT })
        , m_stone_face_buffer(
            m_device->handle(),
            m_device->physicalDevice(),
            shared::ChunkMesh::MAXIMUM_FACE_COUNT
        )
        , m_stone_texture_buffer(m_device->handle(), m_device->physicalDevice())
        , m_canonical_terrain_field(std::make_shared<CanonicalTerrainFieldBuffer>(
            m_device->handle(), m_device->physicalDevice()))
        , m_stone_descriptors(m_device->handle(), m_stone_face_buffer, m_stone_texture_buffer, *m_canonical_terrain_field)
    {
        try {
            auto const dynamic_rendering = m_device->dynamicRenderingCommands();
            m_begin_rendering = dynamic_rendering.begin;
            m_end_rendering = dynamic_rendering.end;
            if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
                throw std::runtime_error("offscreen device does not expose dynamic rendering commands");
            }
            if (m_stone_quad_mode != StoneIndirectDraws::QuadMode::SixVertex) {
                m_stone_quad_indices.emplace(
                    m_device->handle(), m_device->physicalDevice(), 1U, StoneDrawBufferKind::QuadIndices
                );
            }
            m_text_glyph_buffer.emplace(m_device->handle(), m_device->physicalDevice());
            m_text_descriptors.emplace(m_device->handle(), *m_text_glyph_buffer);
            auto const grid = std::make_shared<core::kernel::SpirvModule const>(shader_assets.load("grid.vert.spv"));
            auto const player = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load("player.vert.spv")
            );
            auto const stone = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load(stoneVertexShaderName(m_stone_quad_mode))
            );
            auto const text_vertex = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load("text.vert.spv")
            );
            auto const fragment = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load("trivial.frag.spv")
            );
            auto const text_fragment = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load("text.frag.spv")
            );
            auto const stone_fragment = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load("stone.frag.spv")
            );
            m_grid_program.emplace(core::kernel::GraphicsProgram::create(
                { .module = grid, .entrypoint = "main", .required_bindings = {} },
                { .module = fragment, .entrypoint = "main", .required_bindings = {} }
            ));
            m_player_program.emplace(core::kernel::GraphicsProgram::create(
                { .module = player, .entrypoint = "main", .required_bindings = {} },
                { .module = fragment, .entrypoint = "main", .required_bindings = {} }
            ));
            m_text_program.emplace(core::kernel::GraphicsProgram::create(
                {
                    .module = text_vertex,
                    .entrypoint = "main",
                    .required_bindings = { { .set = 0U, .binding = 0U } },
                },
                { .module = text_fragment, .entrypoint = "main", .required_bindings = {} }
            ));
            m_stone_program.emplace(core::kernel::GraphicsProgram::create(
                {
                    .module = stone,
                    .entrypoint = "main",
                    .required_bindings = { { .set = 0U, .binding = 0U } },
                },
                {
                    .module = stone_fragment,
                    .entrypoint = "main",
                    .required_bindings = { { .set = 0U, .binding = 1U }, { .set = 0U, .binding = 2U } },
                }
            ));
            m_grid_layout = createLayout(static_cast<uint32_t>(sizeof(GridPushConstants)));
            m_player_layout = createLayout(static_cast<uint32_t>(sizeof(BoxPushConstants)));
            m_gui_text_layout = createTextLayout();
            m_stone_layout = createStoneLayout();
            core::graphics::vulkan::VulkanDeviceReference const reference = m_device->reference();
            m_grid_pipeline = m_cache.pipelineFor(reference, *m_grid_program, pipelineDescriptor(m_grid_layout));
            m_player_pipeline = m_cache.pipelineFor(reference, *m_player_program, pipelineDescriptor(m_player_layout));
            m_player_overlay_pipeline = m_cache.pipelineFor(
                reference,
                *m_player_program,
                pipelineDescriptor(m_player_layout, false, false)
            );
            m_gui_text_pipeline = m_cache.pipelineFor(
                reference,
                *m_text_program,
                {
                    .layout = m_gui_text_layout,
                    .color_format = VK_FORMAT_R8G8B8A8_UNORM,
                    .depth_format = VK_FORMAT_UNDEFINED,
                    .depth_test_enabled = false,
                    .depth_write_enabled = false,
                }
            );
            m_stone_pipeline = m_cache.pipelineFor(reference, *m_stone_program, pipelineDescriptor(m_stone_layout));
        } catch (std::exception const&) {
            reset();
            throw;
        }
    }

    ~OffscreenDrawResources()
    {
        reset();
    }

    OffscreenDrawResources(OffscreenDrawResources const&) = delete;
    OffscreenDrawResources& operator=(OffscreenDrawResources const&) = delete;

    [[nodiscard]] OffscreenDepthTarget const& depth() const noexcept { return m_depth; }
    [[nodiscard]] VkPipeline gridPipeline() const noexcept { return m_grid_pipeline; }
    [[nodiscard]] VkPipelineLayout gridLayout() const noexcept { return m_grid_layout; }
    [[nodiscard]] VkPipeline playerPipeline() const noexcept { return m_player_pipeline; }
    [[nodiscard]] VkPipeline playerOverlayPipeline() const noexcept { return m_player_overlay_pipeline; }
    [[nodiscard]] VkPipelineLayout playerLayout() const noexcept { return m_player_layout; }
    [[nodiscard]] VkPipeline guiTextPipeline() const noexcept { return m_gui_text_pipeline; }
    [[nodiscard]] VkPipelineLayout guiTextLayout() const noexcept { return m_gui_text_layout; }
    [[nodiscard]] VkDescriptorSet textDescriptorSet() const noexcept { return requiredValue(m_text_descriptors).set(); }
    [[nodiscard]] VkPipeline stonePipeline() const noexcept { return m_stone_pipeline; }
    [[nodiscard]] VkBuffer stoneQuadIndices() const noexcept
    {
        return m_stone_quad_indices ? m_stone_quad_indices->handle() : VK_NULL_HANDLE;
    }

    [[nodiscard]] StoneIndirectDraws::QuadMode stoneQuadMode() const noexcept { return m_stone_quad_mode; }
    [[nodiscard]] VkPipelineLayout stoneLayout() const noexcept { return m_stone_layout; }
    [[nodiscard]] VkDescriptorSet stoneDescriptorSet() const noexcept { return m_stone_descriptors.set(); }
    [[nodiscard]] PFN_vkCmdBeginRenderingKHR beginRendering() const noexcept { return m_begin_rendering; }
    [[nodiscard]] PFN_vkCmdEndRenderingKHR endRendering() const noexcept { return m_end_rendering; }

    void setTerrainAtlas(std::shared_ptr<generation::GlobalAtlas const> atlas)
    {
        checkResult(vkDeviceWaitIdle(m_device->handle()), "vkDeviceWaitIdle offscreen canonical field");
        auto field = std::make_shared<CanonicalTerrainFieldBuffer>(
            m_device->handle(), m_device->physicalDevice(), std::move(atlas));
        m_stone_descriptors.setField(*field);
        m_canonical_terrain_field = std::move(field);
    }

    void uploadStoneFaces(std::span<StoneFaceInstance const> const faces)
    {
        m_stone_face_buffer.upload(0U, faces);
    }

    void uploadTextGlyphs(VkCommandBuffer const command, std::span<TextGlyph const> const glyphs) const
    {
        requiredValue(m_text_glyph_buffer).update(command, glyphs);
    }

private:
    void reset() noexcept
    {
        if (!m_device || m_device->handle() == VK_NULL_HANDLE) {
            return;
        }
        m_cache.invalidate(m_device->identity());
        if (m_grid_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device->handle(), m_grid_layout, nullptr);
        }
        if (m_player_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device->handle(), m_player_layout, nullptr);
        }
        if (m_gui_text_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device->handle(), m_gui_text_layout, nullptr);
        }
        if (m_stone_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device->handle(), m_stone_layout, nullptr);
        }
        m_grid_layout = VK_NULL_HANDLE;
        m_player_layout = VK_NULL_HANDLE;
        m_gui_text_layout = VK_NULL_HANDLE;
        m_stone_layout = VK_NULL_HANDLE;
        m_grid_pipeline = VK_NULL_HANDLE;
        m_player_pipeline = VK_NULL_HANDLE;
        m_player_overlay_pipeline = VK_NULL_HANDLE;
        m_gui_text_pipeline = VK_NULL_HANDLE;
        m_stone_pipeline = VK_NULL_HANDLE;
        m_text_program.reset();
        m_text_descriptors.reset();
        m_text_glyph_buffer.reset();
        m_stone_descriptors.destroy();
        m_stone_texture_buffer.destroy();
        m_stone_face_buffer.destroy();
        m_stone_quad_indices.reset();
    }

    [[nodiscard]] VkFormat selectDepthFormat() const
    {
        std::array<DepthFormatSupport, DEPTH_FORMAT_CANDIDATES.size()> supported{};
        for (size_t index = 0U; index < DEPTH_FORMAT_CANDIDATES.size(); ++index) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(m_device->physicalDevice(), DEPTH_FORMAT_CANDIDATES[index], &properties);
            supported[index] = {
                .format = DEPTH_FORMAT_CANDIDATES[index],
                .depth_attachment_supported = (
                    properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                ) != 0U,
            };
        }
        std::optional<VkFormat> const selected = selectDepthAttachmentFormat(supported);
        if (!selected.has_value()) {
            throw std::runtime_error("offscreen device has no supported depth attachment format");
        }
        return *selected;
    }

    [[nodiscard]] VkPipelineLayout createLayout(uint32_t const size) const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = size;
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(vkCreatePipelineLayout(m_device->handle(), &info, nullptr, &layout), "vkCreatePipelineLayout offscreen");
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createStoneLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.size = sizeof(StonePushConstants);
        VkDescriptorSetLayout const descriptor_layout = m_stone_descriptors.layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(
            vkCreatePipelineLayout(m_device->handle(), &info, nullptr, &layout),
            "vkCreatePipelineLayout offscreen stone"
        );
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createTextLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(TextPushConstants);
        VkDescriptorSetLayout const descriptor_layout = requiredValue(m_text_descriptors).layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(
            vkCreatePipelineLayout(m_device->handle(), &info, nullptr, &layout),
            "vkCreatePipelineLayout offscreen text"
        );
        return layout;
    }

    [[nodiscard]] core::graphics::vulkan::PipelineDescriptor pipelineDescriptor(
        VkPipelineLayout const layout,
        bool const depth_test_enabled = true,
        bool const depth_write_enabled = true
    ) const noexcept
    {
        return {
            .layout = layout,
            .color_format = VK_FORMAT_R8G8B8A8_UNORM,
            .depth_format = m_depth_format,
            .depth_test_enabled = depth_test_enabled,
            .depth_write_enabled = depth_write_enabled,
            .depth_compare_op = VK_COMPARE_OP_LESS,
        };
    }

    std::shared_ptr<core::graphics::vulkan::VulkanDevice const> m_device;
    VkFormat m_depth_format = VK_FORMAT_UNDEFINED;
    OffscreenDepthTarget m_depth;
    core::graphics::vulkan::VulkanKernelCache m_cache{ KERNEL_CACHE_CAPACITY };
    std::optional<core::kernel::GraphicsProgram> m_grid_program;
    std::optional<core::kernel::GraphicsProgram> m_player_program;
    std::optional<core::kernel::GraphicsProgram> m_text_program;
    std::optional<core::kernel::GraphicsProgram> m_stone_program;
    StoneFaceBuffer m_stone_face_buffer;
    std::optional<VulkanStoneDrawBuffer> m_stone_quad_indices;
    StoneIndirectDraws::QuadMode m_stone_quad_mode = diagnosticStoneQuadMode();
    StoneTextureBuffer m_stone_texture_buffer;
    std::shared_ptr<CanonicalTerrainFieldBuffer> m_canonical_terrain_field;
    StoneDescriptorSet m_stone_descriptors;
    std::optional<TextGlyphBuffer> m_text_glyph_buffer;
    std::optional<TextDescriptorSet> m_text_descriptors;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_gui_text_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_stone_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_overlay_pipeline = VK_NULL_HANDLE;
    VkPipeline m_gui_text_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_pipeline = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR m_begin_rendering = nullptr;
    PFN_vkCmdEndRenderingKHR m_end_rendering = nullptr;
};

} // namespace

struct VulkanOffscreenRenderer::Impl final {
    explicit Impl(ShaderAssets const& shader_assets, bool const require_validation)
        : m_instance(core::graphics::vulkan::VulkanInstance::create({}, REQUIRE_VALIDATION || require_validation))
        , m_device(core::graphics::vulkan::VulkanDevice::create(m_instance))
        , m_resources(std::make_shared<OffscreenDrawResources>(m_device, shader_assets))
        , m_target(std::make_unique<core::graphics::vulkan::VulkanOffscreenTarget>(
            m_device,
            core::graphics::TextureDescriptor{ .width = OFFSCREEN_WIDTH, .height = OFFSCREEN_HEIGHT }
        ))
    { }

    [[nodiscard]] RendererFrameCapture render(
        std::span<PlayerRenderData const> const players,
        std::chrono::steady_clock::time_point const deadline
    )
    {
        m_players = players;
        m_debug_hud_state.updateAt(0.0, {});
        m_gui_renderer.begin();
        if (m_debug_hud_state.formatText(m_debug_hud_text)) {
            DebugHudSnapshot const snapshot = m_debug_hud_state.snapshot();
            static_cast<void>(m_gui_renderer.submit(
                m_debug_hud_text,
                m_debug_hud_state.lineColors(),
                {},
                TextPlacement{ .position = { 8.0F, 8.0F, 0.0F }, .scale = snapshot.dpi_scale }
            ));
        }
        m_text_glyph_upload.clear();
        for (TextBatch const& text : m_gui_renderer.batches()) {
            m_text_glyph_upload.insert(
                m_text_glyph_upload.end(),
                text.glyphs.begin(),
                text.glyphs.end()
            );
        }
        m_target->recordAndReadback(m_resources, &Impl::record, this, remaining(deadline));
        std::span<uint8_t const> const readback = m_target->readback();
        return {
            .width = OFFSCREEN_WIDTH,
            .height = OFFSCREEN_HEIGHT,
            .srgb_encoded = false,
            .rgba8 = std::vector<uint8_t>(readback.begin(), readback.end()),
        };
    }

    void setCamera(CameraPose const pose) noexcept
    {
        static_cast<void>(m_camera.setPosition(pose.position));
        static_cast<void>(m_camera.setAngles(pose.angles));
    }

    void setChunkMesh(shared::ChunkMesh const& mesh)
    {
        std::vector<StoneFaceInstance> instances = stoneFaceInstances(mesh);
        if (m_chunk_scene_enabled && !m_terrain_lod_scene && instances == m_stone_faces) {
            return;
        }
        m_resources->setTerrainAtlas(nullptr);
        m_terrain_lod_scene.reset();
        m_resources->uploadStoneFaces(instances);
        m_stone_faces = std::move(instances);
        m_chunk_scene_enabled = true;
    }

    void setTerrainLodScene(std::shared_ptr<TerrainLodScene const> scene)
    {
        m_resources->setTerrainAtlas(scene ? scene->shading_atlas : nullptr);
        m_resources->uploadStoneFaces(scene ? std::span<StoneFaceInstance const>{scene->faces}
            : std::span<StoneFaceInstance const>{});
        m_stone_faces = scene ? scene->faces : std::vector<StoneFaceInstance>{};
        m_terrain_lod_scene = std::move(scene);
        m_chunk_scene_enabled = true;
    }

    void setDebugHudEnabled(bool const enabled) noexcept
    {
        m_debug_hud_state.setEnabled(enabled);
    }

    [[nodiscard]] bool validationEnabled() const noexcept { return m_instance->validationEnabled(); }
    [[nodiscard]] uint32_t validationErrorCount() const noexcept { return m_instance->validationErrorCount(); }
private:
    static void record(core::graphics::vulkan::VulkanOffscreenTarget::Recording const& recording, void* const user_data)
    {
        Impl& self = *static_cast<Impl*>(user_data);
        OffscreenDrawResources const& resources = *self.m_resources;
        if (!self.m_text_glyph_upload.empty()) {
            resources.uploadTextGlyphs(recording.command_buffer, self.m_text_glyph_upload);
        }
        if (!self.m_depth_initialized) {
            VkImageMemoryBarrier depth_barrier{};
            depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depth_barrier.image = resources.depth().image();
            depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth_barrier.subresourceRange.levelCount = 1U;
            depth_barrier.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(
                recording.command_buffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0U,
                0U,
                nullptr,
                0U,
                nullptr,
                1U,
                &depth_barrier
            );
            self.m_depth_initialized = true;
        }
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = recording.color_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.float32[0] = 0.38F;
        color.clearValue.color.float32[1] = 0.62F;
        color.clearValue.color.float32[2] = 0.88F;
        color.clearValue.color.float32[3] = 1.0F;
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = resources.depth().view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil.depth = 1.0F;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = recording.extent;
        info.layerCount = 1U;
        info.colorAttachmentCount = 1U;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        resources.beginRendering()(recording.command_buffer, &info);
        VkViewport viewport{};
        viewport.width = static_cast<float>(recording.extent.width);
        viewport.height = static_cast<float>(recording.extent.height);
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{};
        scissor.extent = recording.extent;
        vkCmdSetViewport(recording.command_buffer, 0U, 1U, &viewport);
        vkCmdSetScissor(recording.command_buffer, 0U, 1U, &scissor);
        if (self.m_chunk_scene_enabled) {
            StoneDrawRange const range{ .first_instance = 0U, .instance_count = static_cast<uint32_t>(self.m_stone_faces.size()) };
            static_cast<void>(recordStoneScene(
                recording.command_buffer,
                resources.stonePipeline(),
                resources.stonePipeline(),
                resources.stoneLayout(),
                resources.stoneDescriptorSet(),
                resources.playerPipeline(),
                resources.playerOverlayPipeline(),
                resources.playerLayout(),
                self.m_camera,
                self.m_players,
                std::span<StoneDrawRange const>{ &range, self.m_stone_faces.empty() ? 0U : 1U },
                {},
                recording.extent,
                false,
                {},
                resources.stoneQuadIndices(),
                VK_NULL_HANDLE,
                resources.stoneQuadMode(),
                VK_NULL_HANDLE,
                false,
                self.m_terrain_lod_scene ? static_cast<float>(self.m_terrain_lod_scene->block_sea_level) : 0.0F
            ));
        } else {
            recordFlat3dScene(
                recording.command_buffer,
                resources.gridPipeline(),
                resources.gridLayout(),
                resources.playerPipeline(),
                resources.playerOverlayPipeline(),
                resources.playerLayout(),
                self.m_camera,
                self.m_players,
                recording.extent
            );
        }
        resources.endRendering()(recording.command_buffer);
        if (!self.m_gui_renderer.empty()) {
            VkMemoryBarrier color_barrier{};
            color_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            color_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            color_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(
                recording.command_buffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0U,
                1U,
                &color_barrier,
                0U,
                nullptr,
                0U,
                nullptr
            );
            VkRenderingAttachmentInfo gui_color{};
            gui_color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            gui_color.imageView = recording.color_view;
            gui_color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            gui_color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            gui_color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo gui_info{};
            gui_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            gui_info.renderArea.extent = recording.extent;
            gui_info.layerCount = 1U;
            gui_info.colorAttachmentCount = 1U;
            gui_info.pColorAttachments = &gui_color;
            resources.beginRendering()(recording.command_buffer, &gui_info);
            VkViewport gui_viewport{};
            gui_viewport.width = static_cast<float>(recording.extent.width);
            gui_viewport.height = static_cast<float>(recording.extent.height);
            gui_viewport.maxDepth = 1.0F;
            VkRect2D gui_scissor{};
            gui_scissor.extent = recording.extent;
            vkCmdSetViewport(recording.command_buffer, 0U, 1U, &gui_viewport);
            vkCmdSetScissor(recording.command_buffer, 0U, 1U, &gui_scissor);
            uint32_t gui_text_draw_count = 0U;
            uint32_t first_instance = 0U;
            for (TextBatch const& text : self.m_gui_renderer.batches()) {
                recordText(
                    recording.command_buffer,
                    resources.guiTextPipeline(),
                    resources.guiTextLayout(),
                    resources.textDescriptorSet(),
                    text,
                    glm::mat4{ 1.0F },
                    recording.extent,
                    textShaderMode(text.space),
                    first_instance,
                    gui_text_draw_count
                );
                first_instance += static_cast<uint32_t>(text.glyphs.size());
            }
            resources.endRendering()(recording.command_buffer);
        }
    }

    std::shared_ptr<core::graphics::vulkan::VulkanInstance> m_instance;
    std::shared_ptr<core::graphics::vulkan::VulkanDevice> m_device;
    std::shared_ptr<OffscreenDrawResources> m_resources;
    std::unique_ptr<core::graphics::vulkan::VulkanOffscreenTarget> m_target;
    std::span<PlayerRenderData const> m_players;
    std::shared_ptr<TerrainLodScene const> m_terrain_lod_scene;
    std::vector<StoneFaceInstance> m_stone_faces;
    DebugHudState m_debug_hud_state;
    DebugHudText m_debug_hud_text;
    GuiRenderer m_gui_renderer;
    std::vector<TextGlyph> m_text_glyph_upload;
    bool m_depth_initialized = false;
    bool m_chunk_scene_enabled = false;
    Camera m_camera{
        { .position = { 16.0, -20.0, 22.0 }, .angles = { .pitch_degrees = -35.0 } },
    };
};

#if !defined(__ANDROID__)
std::shared_ptr<core::graphics::vulkan::PresentationContext> VulkanRenderer::createPresentationContext(
    core::platform::glfw::GlfwWindow const& window,
    VulkanRendererOptions const options
)
{
    std::shared_ptr<core::graphics::vulkan::VulkanInstance> const instance =
        core::graphics::vulkan::VulkanInstance::create(
            core::graphics::vulkan::glfw::requiredInstanceExtensions(),
            REQUIRE_VALIDATION || options.require_validation
        );
    uint32_t width = 0U;
    uint32_t height = 0U;
    window.framebufferSize(width, height);
    return core::graphics::vulkan::PresentationContext::create(
        core::graphics::vulkan::glfw::createSurface(instance, window),
        {
            .enable_validation = REQUIRE_VALIDATION || options.require_validation,
            .prefer_mesh_shaders = false,
            .enable_transfer_source = options.enable_frame_capture,
            .require_transfer_source = options.enable_frame_capture,
            .prefer_immediate_present = options.require_immediate_present_mode,
            .require_immediate_present = options.require_immediate_present_mode,
            .enable_presentation_history = options.enable_presentation_history,
        },
        { .width = width, .height = height }
    );
}
#endif

#if defined(__ANDROID__)
std::shared_ptr<core::graphics::vulkan::PresentationContext> VulkanRenderer::createPresentationContext(
    ANativeWindow* const window,
    uint32_t const width,
    uint32_t const height,
    VulkanRendererOptions const options
)
{
    std::shared_ptr<core::graphics::vulkan::VulkanInstance> const instance =
        core::graphics::vulkan::VulkanInstance::create(
            core::graphics::vulkan::android::requiredInstanceExtensions(),
            REQUIRE_VALIDATION || options.require_validation
        );
    return core::graphics::vulkan::PresentationContext::create(
        core::graphics::vulkan::android::createSurface(instance, window),
        {
            .enable_validation = REQUIRE_VALIDATION || options.require_validation,
            .prefer_mesh_shaders = false,
            .enable_transfer_source = options.enable_frame_capture,
            .require_transfer_source = options.enable_frame_capture,
            .prefer_immediate_present = options.require_immediate_present_mode,
            .require_immediate_present = options.require_immediate_present_mode,
            .enable_presentation_history = options.enable_presentation_history,
        },
        { .width = width, .height = height }
    );
}
#endif

VulkanRenderer::VulkanRenderer(
    std::shared_ptr<core::graphics::vulkan::PresentationContext> context,
    ShaderAssets const& shader_assets,
    VulkanRendererOptions const options
)
    : m_impl(std::make_unique<Impl>(std::move(context), shader_assets, options))
{ }

VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::render(std::span<PlayerRenderData const> const players, std::chrono::steady_clock::time_point const deadline)
{
    return render(players, DebugHudInput{}, 1.0F, deadline);
}

bool VulkanRenderer::render(
    std::span<PlayerRenderData const> const players,
    DebugHudInput const debug_hud_input,
    float const debug_hud_dpi_scale,
    std::chrono::steady_clock::time_point const deadline
)
{
    return m_impl->render(players, debug_hud_input, debug_hud_dpi_scale, deadline);
}

void VulkanRenderer::setDebugHudEnabled(bool const enabled) noexcept
{
    m_impl->setDebugHudEnabled(enabled);
}

void VulkanRenderer::toggleDebugHud() noexcept
{
    m_impl->toggleDebugHud();
}

void VulkanRenderer::setFallbackSceneEnabled(bool const enabled) noexcept
{
    m_impl->setFallbackSceneEnabled(enabled);
}

void VulkanRenderer::setGuiText(std::string_view const text, TextColor const color)
{
    m_impl->setGuiText(text, color);
}

void VulkanRenderer::addGuiText(std::string_view const text, TextPlacement const placement, TextColor const color)
{
    m_impl->addGuiText(text, placement, color);
}

void VulkanRenderer::clearGuiText() noexcept
{
    m_impl->clearGuiText();
}

bool VulkanRenderer::setMapImage(
    uint32_t const width,
    uint32_t const height,
    std::span<std::array<uint8_t, 4> const> const rgba8
)
{
    return m_impl->setMapImage(width, height, rgba8);
}

void VulkanRenderer::clearMapImage() noexcept
{
    m_impl->clearMapImage();
}

void VulkanRenderer::setFrameCompletionTracking(bool const enabled) noexcept
{
    m_impl->setFrameCompletionTracking(enabled);
}

RendererFrameProgress VulkanRenderer::frameProgress() const noexcept
{
    return m_impl->frameProgress();
}

void VulkanRenderer::setWorldText(std::string_view const text, TextPlacement const placement, TextColor const color)
{
    m_impl->setWorldText(text, placement, color);
}

void VulkanRenderer::addWorldText(std::string_view const text, TextPlacement const placement, TextColor const color)
{
    m_impl->addWorldText(text, placement, color);
}

void VulkanRenderer::clearWorldText() noexcept
{
    m_impl->clearWorldText();
}

void VulkanRenderer::setCamera(CameraPose const pose) noexcept
{
    m_impl->setCamera(pose);
}

void VulkanRenderer::setCamera(CameraPose const pose, CameraProjection const projection) noexcept
{
    m_impl->setCamera(pose, projection);
}

void VulkanRenderer::setChunkMesh(shared::ChunkMesh const& mesh)
{
    m_impl->setChunkMesh(mesh);
}

void VulkanRenderer::setChunkMeshes(std::span<shared::ChunkMesh const> const meshes)
{
    m_impl->setChunkMeshes(meshes);
}

bool VulkanRenderer::upsertTerrainLodScene(
    std::shared_ptr<TerrainLodScene const> scene,
    std::chrono::steady_clock::time_point const deadline
)
{
    return m_impl->upsertTerrainLodScene(std::move(scene), deadline);
}

void VulkanRenderer::clearTerrainLodScene()
{
    m_impl->clearTerrainLodScene();
}

bool VulkanRenderer::upsertHeightTileMesh(
    shared::HeightTileSurfaceMesh const& mesh,
    std::chrono::steady_clock::time_point const deadline
)
{
    return m_impl->upsertHeightTileMesh(mesh, deadline);
}

bool VulkanRenderer::upsertHeightTileMeshes(
    std::span<shared::HeightTileSurfaceMesh const> const meshes,
    std::chrono::steady_clock::time_point const deadline,
    RendererHeightTileUpsertDiagnostics* const diagnostics
)
{
    return m_impl->upsertHeightTileMeshes(meshes, deadline, diagnostics);
}

bool VulkanRenderer::removeHeightTileMesh(shared::HeightTileCoordinate const coordinate)
{
    return m_impl->removeHeightTileMesh(coordinate);
}

uint32_t VulkanRenderer::removeHeightTileMeshes(
    std::span<shared::HeightTileCoordinate const> const coordinates,
    bool const capture_diagnostics
)
{
    return m_impl->removeHeightTileMeshes(coordinates, capture_diagnostics);
}

void VulkanRenderer::hotReload()
{
    m_impl->hotReload();
}

void VulkanRenderer::recreate(uint32_t const width, uint32_t const height)
{
    m_impl->resize(width, height);
}

bool VulkanRenderer::waitForSubmittedFrames(std::chrono::steady_clock::time_point const deadline)
{
    return m_impl->waitForSubmittedFrames(deadline);
}

void VulkanRenderer::requestFrameCapture()
{
    m_impl->requestFrameCapture();
}

FrameCaptureState VulkanRenderer::captureState() const
{
    return m_impl->captureState();
}

std::optional<RendererFrameCapture> VulkanRenderer::takeFrameCapture()
{
    return m_impl->takeFrameCapture();
}

RendererRuntimeInfo VulkanRenderer::runtimeInfo() const
{
    return m_impl->runtimeInfo();
}

core::graphics::vulkan::PresentationHistoryResult VulkanRenderer::queryPresentationHistory(
    std::span<core::graphics::vulkan::PresentationTiming> const records
)
{
    return m_impl->queryPresentationHistory(records);
}

uint32_t VulkanRenderer::validationErrorCount() const noexcept
{
    return m_impl->validationErrorCount();
}

VulkanOffscreenRenderer::VulkanOffscreenRenderer(
    ShaderAssets const& shader_assets,
    bool const require_validation
)
    : m_impl(std::make_unique<Impl>(shader_assets, require_validation))
{ }

VulkanOffscreenRenderer::~VulkanOffscreenRenderer() = default;

RendererFrameCapture VulkanOffscreenRenderer::render(
    std::span<PlayerRenderData const> const players,
    std::chrono::steady_clock::time_point const deadline
)
{
    return m_impl->render(players, deadline);
}

void VulkanOffscreenRenderer::setCamera(CameraPose const pose) noexcept
{
    m_impl->setCamera(pose);
}

void VulkanOffscreenRenderer::setChunkMesh(shared::ChunkMesh const& mesh)
{
    m_impl->setChunkMesh(mesh);
}

void VulkanOffscreenRenderer::setTerrainLodScene(std::shared_ptr<TerrainLodScene const> scene)
{
    m_impl->setTerrainLodScene(std::move(scene));
}

void VulkanOffscreenRenderer::setDebugHudEnabled(bool const enabled) noexcept
{
    m_impl->setDebugHudEnabled(enabled);
}

bool VulkanOffscreenRenderer::validationEnabled() const noexcept
{
    return m_impl->validationEnabled();
}

uint32_t VulkanOffscreenRenderer::validationErrorCount() const noexcept
{
    return m_impl->validationErrorCount();
}

} // namespace client
