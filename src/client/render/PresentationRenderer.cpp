#include <client/render/DepthFormat.hpp>
#include <client/render/GuiRenderer.hpp>
#include <client/render/StoneTexture.hpp>
#include <client/render/VulkanRenderer.hpp>

#include <shared/net/Message.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/HeightTileSurfaceMesher.hpp>

#if defined(__ANDROID__)
#include <core/graphics/vulkan/android/AndroidSurface.hpp>
#else
#include <core/graphics/vulkan/glfw/GlfwSurface.hpp>
#endif

#include <core/kernel/Program.hpp>

#if !defined(__ANDROID__)
#include <core/platform/glfw/GlfwWindow.hpp>
#endif

#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace client {
namespace {

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
constexpr uint32_t MAXIMUM_RENDERED_STONE_FACE_COUNT =
    shared::HeightTileSurfaceMesh::MAXIMUM_QUAD_COUNT * shared::HEIGHT_TILE_INTEREST_COUNT;
constexpr double WORLD_WRAP_PERIOD = 65'536.0;
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
};
static_assert(sizeof(BoxPushConstants) == 112U);

struct alignas(16) TextPushConstants final {
    glm::mat4 projection_view{ 1.0F };
    std::array<float, 2> resolution{ 0.0F, 0.0F };
    float scale = 1.0F;
    uint32_t mode = 0U;
};
static_assert(sizeof(TextPushConstants) == 80U);

void checkResult(VkResult const result, char const* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string{ operation } + " failed with Vulkan result " + std::to_string(result));
    }
}

struct alignas(16) StoneFaceInstance final {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t direction = 0U;
    uint32_t u_extent = 1U;
    uint32_t v_extent = 1U;
    uint32_t reserved_0 = 0U;
    uint32_t reserved_1 = 0U;

    constexpr bool operator==(StoneFaceInstance const&) const noexcept = default;
};
static_assert(sizeof(StoneFaceInstance) == 32U);

struct alignas(16) StonePushConstants final {
    glm::mat4 projection_view{ 1.0F };
    std::array<int32_t, 4> world_origin{ 0, 0, 0, 0 };
    glm::vec4 camera_fog{ 0.0F, 0.0F, 0.0F, 720.0F };
};
static_assert(sizeof(StonePushConstants) == 96U);

struct StoneDrawRange final {
    uint32_t first_instance = 0U;
    uint32_t instance_count = 0U;
};

struct WrappedBounds final {
    glm::vec3 minimum{};
    glm::vec3 maximum{};
};

[[nodiscard]] WrappedBounds boundsNearestToCamera(
    glm::vec3 const minimum,
    glm::vec3 const maximum,
    glm::dvec3 const camera_position
) noexcept {
    glm::vec3 const center = (minimum + maximum) * 0.5F;
    float const x_shift = static_cast<float>(
        std::round((camera_position.x - static_cast<double>(center.x)) / WORLD_WRAP_PERIOD)
        * WORLD_WRAP_PERIOD
    );
    float const y_shift = static_cast<float>(
        std::round((camera_position.y - static_cast<double>(center.y)) / WORLD_WRAP_PERIOD)
        * WORLD_WRAP_PERIOD
    );
    return {
        .minimum = { minimum.x + x_shift, minimum.y + y_shift, minimum.z },
        .maximum = { maximum.x + x_shift, maximum.y + y_shift, maximum.z },
    };
}

[[nodiscard]]
std::vector<StoneFaceInstance> stoneFaceInstances(shared::ChunkMesh const& mesh)
{
    if (mesh.faces.size() > shared::ChunkMesh::MAXIMUM_FACE_COUNT) {
        throw std::invalid_argument("chunk mesh exceeds its face limit");
    }
    std::vector<StoneFaceInstance> instances;
    instances.reserve(mesh.faces.size());
    for (shared::MeshFace const& face : mesh.faces) {
        uint32_t const direction = static_cast<uint32_t>(face.direction);
        if (face.material != shared::Block::Stone || direction >= 6U) {
            throw std::invalid_argument("chunk mesh contains an unsupported face");
        }
        instances.emplace_back(StoneFaceInstance{
            .x = mesh.coordinate.x * static_cast<int32_t>(shared::Chunk::SIDE_LENGTH) + face.local_origin.x,
            .y = mesh.coordinate.y * static_cast<int32_t>(shared::Chunk::SIDE_LENGTH) + face.local_origin.y,
            .z = mesh.coordinate.z * static_cast<int32_t>(shared::Chunk::SIDE_LENGTH) + face.local_origin.z,
            .direction = direction,
        });
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
    instances.reserve(mesh.quads.size());
    for (shared::HeightTileSurfaceQuad const& quad : mesh.quads) {
        uint32_t const direction = static_cast<uint32_t>(quad.direction);
        if (direction >= 6U || quad.u_extent == 0U || quad.v_extent == 0U) {
            throw std::invalid_argument("height tile mesh contains an invalid surface quad");
        }
        instances.push_back({
            .x = quad.x,
            .y = quad.y,
            .z = quad.z,
            .direction = direction,
            .u_extent = quad.u_extent,
            .v_extent = quad.v_extent,
        });
    }
    return instances;
}

class StoneFaceBuffer final {
public:
    StoneFaceBuffer(VkDevice const device, VkPhysicalDevice const physical_device)
        : m_device(device)
        , m_physical_device(physical_device)
    {
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
        if (first_instance > MAXIMUM_RENDERED_STONE_FACE_COUNT
            || instances.size() > MAXIMUM_RENDERED_STONE_FACE_COUNT - first_instance) {
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
                && (properties.memoryTypes[index].propertyFlags
                    & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                return index;
            }
        }
        throw std::runtime_error("Vulkan device has no coherent host-visible memory for stone faces");
    }

    [[nodiscard]] static constexpr VkDeviceSize byteSize() noexcept
    {
        return static_cast<VkDeviceSize>(MAXIMUM_RENDERED_STONE_FACE_COUNT)
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
};

class StoneTextureBuffer final {
public:
    StoneTextureBuffer(VkDevice const device, VkPhysicalDevice const physical_device)
        : m_device(device)
        , m_physical_device(physical_device)
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = sizeof(STONE_TEXTURE_RGBA8);
        buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkResult(vkCreateBuffer(m_device, &buffer_info, nullptr, &m_buffer), "vkCreateBuffer stone texture");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, m_buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(m_physical_device, &properties);
        uint32_t memory_type = properties.memoryTypeCount;
        for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
            VkMemoryPropertyFlags constexpr required =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1U << index)) != 0U
                && (properties.memoryTypes[index].propertyFlags & required) == required) {
                memory_type = index;
                break;
            }
        }
        if (memory_type == properties.memoryTypeCount) {
            reset();
            throw std::runtime_error("Vulkan device has no coherent host-visible memory for stone texture");
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        checkResult(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "vkAllocateMemory stone texture");
        checkResult(vkBindBufferMemory(m_device, m_buffer, m_memory, 0U), "vkBindBufferMemory stone texture");
        void* mapped = nullptr;
        checkResult(vkMapMemory(m_device, m_memory, 0U, sizeof(STONE_TEXTURE_RGBA8), 0U, &mapped),
            "vkMapMemory stone texture");
        std::memcpy(mapped, STONE_TEXTURE_RGBA8.data(), sizeof(STONE_TEXTURE_RGBA8));
        vkUnmapMemory(m_device, m_memory);
        VkBufferViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        view_info.buffer = m_buffer;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.range = sizeof(STONE_TEXTURE_RGBA8);
        checkResult(vkCreateBufferView(m_device, &view_info, nullptr, &m_view), "vkCreateBufferView stone texture");
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

class StoneDescriptorSet final {
public:
    StoneDescriptorSet(
        VkDevice const device,
        StoneFaceBuffer const& buffer,
        StoneTextureBuffer const& texture
    )
        : m_device(device)
    {
        try {
            std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
            bindings[0].binding = 0U;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1U;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            bindings[1].binding = 1U;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            bindings[1].descriptorCount = 1U;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
            layout_info.pBindings = bindings.data();
            checkResult(
                vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_layout),
                "vkCreateDescriptorSetLayout stone faces"
            );
            std::array<VkDescriptorPoolSize, 2> pool_sizes{
                VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U},
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
            std::array<VkWriteDescriptorSet, 2> const writes{write, texture_write};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0U, nullptr);
        } catch (std::exception const&) {
            reset();
            throw;
        }
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

    void update(VkCommandBuffer const command, std::span<TextGlyph const> glyphs) const
    {
        if (glyphs.empty()) {
            return;
        }
        if (glyphs.size() > MAXIMUM_TEXT_GLYPH_COUNT) {
            throw std::invalid_argument("text glyph upload exceeds its bounded GPU buffer");
        }
        size_t const bytes = glyphs.size() * sizeof(TextGlyph);
        for (size_t offset = 0U; offset < glyphs.size(); offset += MAXIMUM_TEXT_UPDATE_GLYPHS) {
            size_t const count = std::min(MAXIMUM_TEXT_UPDATE_GLYPHS, glyphs.size() - offset);
            vkCmdUpdateBuffer(
                command,
                m_buffer,
                static_cast<VkDeviceSize>(offset * sizeof(TextGlyph)),
                static_cast<VkDeviceSize>(count * sizeof(TextGlyph)),
                glyphs.data() + offset
            );
        }
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.buffer = m_buffer;
        barrier.size = static_cast<VkDeviceSize>(bytes);
        vkCmdPipelineBarrier(
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

void recordPlayers(
    VkCommandBuffer const command,
    VkPipeline const player_pipeline,
    VkPipelineLayout const player_layout,
    glm::mat4 const& projection_view,
    std::span<PlayerRenderData const> const players
)
{
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, player_pipeline);
    for (PlayerRenderData const& player : players) {
        BoxPushConstants const push{
            .projection_view = projection_view,
            .origin = {
                player.x,
                player.y,
                player.z,
                0.0F,
            },
            .extent = { 2.0F, 2.0F, 2.0F, 0.0F },
            .color = player.color,
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
}

void recordFlat3dScene(
    VkCommandBuffer const command,
    VkPipeline const grid_pipeline,
    VkPipelineLayout const grid_layout,
    VkPipeline const player_pipeline,
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

    recordPlayers(command, player_pipeline, player_layout, projection_view, players);
}

[[nodiscard]]
uint32_t recordStoneScene(
    VkCommandBuffer const command,
    VkPipeline const pipeline,
    VkPipeline const solid_pipeline,
    VkPipelineLayout const layout,
    VkDescriptorSet const descriptor_set,
    VkPipeline const player_pipeline,
    VkPipelineLayout const player_layout,
    Camera const& camera,
    std::span<PlayerRenderData const> const players,
    std::span<StoneDrawRange const> const draw_ranges,
    std::span<StoneDrawRange const> const solid_draw_ranges,
    VkExtent2D const extent
)
{
    std::optional<glm::mat4> const projection = camera.projectionMatrix(extent.width, extent.height);
    if (!projection.has_value()) {
        return 0U;
    }
    CameraPose const camera_pose = camera.pose();
    std::array<int32_t, 4> const world_origin{
        static_cast<int32_t>(std::floor(camera_pose.position.x)),
        static_cast<int32_t>(std::floor(camera_pose.position.y)),
        static_cast<int32_t>(std::floor(camera_pose.position.z)),
        0,
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
        .camera_fog = { relative_eye, 720.0F },
    };
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
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(push), &push);
        for (StoneDrawRange const range : draw_ranges) {
            vkCmdDraw(command, STONE_FACE_VERTEX_COUNT, range.instance_count, 0U, range.first_instance);
        }
    }
    if (!solid_draw_ranges.empty()) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline);
        vkCmdBindDescriptorSets(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0U, 1U, &descriptor_set, 0U, nullptr
        );
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(push), &push);
        for (StoneDrawRange const range : solid_draw_ranges) {
            vkCmdDraw(command, STONE_FACE_VERTEX_COUNT, range.instance_count, 0U, range.first_instance);
        }
    }
    recordPlayers(command, player_pipeline, player_layout, *projection * camera.viewMatrix(), players);
    return static_cast<uint32_t>(draw_ranges.size() + solid_draw_ranges.size());
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
    uint32_t const mode,
    uint32_t const first_instance,
    uint32_t& draw_count
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
        .mode = mode,
    };
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(push), &push);
    vkCmdDraw(command, QUAD_VERTEX_COUNT, static_cast<uint32_t>(count), 0U, first_instance);
    ++draw_count;
}

[[nodiscard]]
bool isVisibleInFrustum(glm::vec3 const minimum, glm::vec3 const maximum, glm::mat4 const& projection_view)
{
    std::array<glm::vec4, 8> const corners{
        glm::vec4{ minimum.x, minimum.y, minimum.z, 1.0F },
        glm::vec4{ maximum.x, minimum.y, minimum.z, 1.0F },
        glm::vec4{ minimum.x, maximum.y, minimum.z, 1.0F },
        glm::vec4{ maximum.x, maximum.y, minimum.z, 1.0F },
        glm::vec4{ minimum.x, minimum.y, maximum.z, 1.0F },
        glm::vec4{ maximum.x, minimum.y, maximum.z, 1.0F },
        glm::vec4{ minimum.x, maximum.y, maximum.z, 1.0F },
        glm::vec4{ maximum.x, maximum.y, maximum.z, 1.0F },
    };
    std::array<bool, 6> outside{ true, true, true, true, true, true };
    for (glm::vec4 const corner : corners) {
        glm::vec4 const clip = projection_view * corner;
        outside[0] = outside[0] && clip.x < -clip.w;
        outside[1] = outside[1] && clip.x > clip.w;
        outside[2] = outside[2] && clip.y < -clip.w;
        outside[3] = outside[3] && clip.y > clip.w;
        outside[4] = outside[4] && clip.z < 0.0F;
        outside[5] = outside[5] && clip.z > clip.w;
    }
    return std::ranges::none_of(outside, [](bool const value) { return value; });
}

} // namespace

struct VulkanRenderer::Impl final {
    using PresentationContext = core::graphics::vulkan::PresentationContext;
    using ResourceScope = PresentationContext::PresentationResourceScope;

    struct HeightTileCoordinateHash final {
        [[nodiscard]] size_t operator()(shared::HeightTileCoordinate const coordinate) const noexcept
        {
            uint64_t const x = static_cast<uint32_t>(coordinate.x);
            uint64_t const y = static_cast<uint32_t>(coordinate.y);
            return static_cast<size_t>((x << 32U) ^ y);
        }
    };

    struct HeightTileSlot final {
        StoneDrawRange range;
        std::vector<StoneFaceInstance> instances;
        glm::vec3 minimum;
        glm::vec3 maximum;
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
        core::graphics::vulkan::PresentationAcquireResult const acquired = m_context->acquire(
            remaining(deadline),
            frame
        );
        m_cpu_acquire_wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - acquire_started_at
        );
        if (acquired == core::graphics::vulkan::PresentationAcquireResult::NeedsRecreation) {
            m_last_presented = false;
            recreate(m_context->info().extent, std::chrono::steady_clock::time_point::max());
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
        m_current_depth_target = &depthTargetFor(frame->image());
        frame->record(&Impl::recordFrame, this);
        m_cpu_command_record_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - command_record_started_at
        );
        std::chrono::steady_clock::time_point const complete_present_started_at =
            std::chrono::steady_clock::now();
        m_context->complete(*frame);
        m_cpu_complete_present_wait_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - complete_present_started_at
        );
        m_current_depth_target = nullptr;
        m_last_presented = true;
        m_cpu_frame_duration = m_cpu_command_record_duration + m_cpu_complete_present_wait_duration;
        ++m_submitted_frame_count;

        if (m_capture_requested) {
            std::optional<std::span<uint8_t const>> const readback = m_context->takeCompletedReadback(
                remaining(deadline)
            );
            if (readback.has_value()) {
                core::graphics::vulkan::PresentationInfo const& info = m_context->info();
                m_last_capture = RendererFrameCapture{
                    .width = info.extent.width,
                    .height = info.extent.height,
                    .srgb_encoded = info.srgb,
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

    void setChunkMesh(shared::ChunkMesh const& mesh)
    {
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
        m_stone_face_buffer->upload(0U, m_stone_faces);
        ++m_chunk_mesh_upload_count;
    }

    void setChunkMeshes(std::span<shared::ChunkMesh const> const meshes)
    {
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
        m_stone_face_buffer->upload(0U, m_stone_faces);
        ++m_chunk_mesh_upload_count;
    }

    void upsertHeightTileMesh(shared::HeightTileSurfaceMesh const& mesh)
    {
        std::vector<StoneFaceInstance> instances = stoneFaceInstances(mesh);
        if (!m_stone_faces.empty()) {
            m_stone_faces.clear();
            m_legacy_draw_range = {};
        }
        HeightTileSlot* const existing = heightTileSlot(mesh.coordinate);
        if (existing != nullptr && existing->instances == instances) {
            return;
        }
        if (existing != nullptr) {
            releaseRange(existing->range);
            m_height_tile_slots.erase(mesh.coordinate);
        }
        if (instances.empty()) {
            return;
        }
        StoneDrawRange const range = allocateRange(static_cast<uint32_t>(instances.size()));
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
        m_stone_face_buffer->upload(range.first_instance, instances);
        m_height_tile_slots.emplace(mesh.coordinate, HeightTileSlot{
            .range = range,
            .instances = std::move(instances),
            .minimum = minimum,
            .maximum = maximum,
        });
        m_chunk_scene_enabled = true;
        ++m_chunk_mesh_upload_count;
    }

    [[nodiscard]] bool removeHeightTileMesh(shared::HeightTileCoordinate const coordinate)
    {
        auto const existing = m_height_tile_slots.find(coordinate);
        if (existing == m_height_tile_slots.end()) {
            return false;
        }
        releaseRange(existing->second.range);
        m_height_tile_slots.erase(existing);
        if (m_height_tile_slots.empty() && m_stone_faces.empty()) {
            m_chunk_scene_enabled = false;
        }
        return true;
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
            .gpu_name = "presentation-context",
            .vulkan_api_version = {
                .major = VK_VERSION_MAJOR(info.api_version),
                .minor = VK_VERSION_MINOR(info.api_version),
                .patch = VK_VERSION_PATCH(info.api_version),
            },
            .validation_enabled = info.validation_enabled,
            .present_mode = rendererPresentMode(info.present_mode),
            .pipeline_path = RendererPipelinePath::Vertex,
            .cpu_acquire_wait_duration = m_cpu_acquire_wait_duration,
            .cpu_command_record_duration = m_cpu_command_record_duration,
            .cpu_complete_present_wait_duration = m_cpu_complete_present_wait_duration,
            .cpu_frame_duration = m_cpu_frame_duration,
            .gpu_frame_duration = std::nullopt,
            .submitted_frame_count = m_submitted_frame_count,
            .debug_hud_draw_count = m_debug_hud_draw_count,
            .chunk_face_count = m_height_tile_slots.empty()
                ? static_cast<uint32_t>(m_stone_faces.size()) : heightTileFaceCount(),
            .chunk_draw_count = m_chunk_draw_count,
            .chunk_mesh_upload_count = m_chunk_mesh_upload_count,
            .height_tile_mesh_count = static_cast<uint32_t>(m_height_tile_slots.size()),
        };
    }

    void recreate(VkExtent2D const extent, std::chrono::steady_clock::time_point const deadline)
    {
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

private:
    [[nodiscard]] HeightTileSlot* heightTileSlot(shared::HeightTileCoordinate const coordinate) noexcept
    {
        auto const existing = m_height_tile_slots.find(coordinate);
        return existing == m_height_tile_slots.end() ? nullptr : &existing->second;
    }

    [[nodiscard]] uint32_t heightTileFaceCount() const noexcept
    {
        uint32_t result = 0U;
        for (auto const& [coordinate, slot] : m_height_tile_slots) {
            static_cast<void>(coordinate);
            result += static_cast<uint32_t>(slot.instances.size());
        }
        return result;
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
            [instance_count](StoneDrawRange const range) { return range.instance_count >= instance_count; }
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
        std::vector<StoneDrawRange> merged;
        merged.reserve(m_free_stone_ranges.size());
        for (StoneDrawRange const free_range : m_free_stone_ranges) {
            if (!merged.empty()
                && merged.back().first_instance + merged.back().instance_count == free_range.first_instance) {
                merged.back().instance_count += free_range.instance_count;
            } else {
                merged.push_back(free_range);
            }
        }
        m_free_stone_ranges = std::move(merged);
    }

    void reclaimRetiredRanges()
    {
        auto iterator = m_retired_stone_ranges.begin();
        while (iterator != m_retired_stone_ranges.end()) {
            if (iterator->reusable_after_frame > m_submitted_frame_count) {
                ++iterator;
                continue;
            }
            makeRangeReusable(iterator->range);
            iterator = m_retired_stone_ranges.erase(iterator);
        }
    }

    void clearHeightTileMeshes()
    {
        m_height_tile_slots.clear();
        m_free_stone_ranges.clear();
        m_retired_stone_ranges.clear();
        m_next_stone_face = 0U;
    }

    void uploadStoredMeshes()
    {
        if (!m_stone_faces.empty()) {
            m_stone_face_buffer->upload(0U, m_stone_faces);
            ++m_chunk_mesh_upload_count;
            return;
        }
        for (auto const& [coordinate, slot] : m_height_tile_slots) {
            static_cast<void>(coordinate);
            m_stone_face_buffer->upload(slot.range.first_instance, slot.instances);
            ++m_chunk_mesh_upload_count;
        }
    }

    static void beforeRecreate(void* const user_data)
    {
        static_cast<Impl*>(user_data)->destroyResources();
    }

    static void afterRecreate(
        core::graphics::vulkan::PresentationInfo const&,
        void* const user_data
    )
    {
        Impl& self = *static_cast<Impl*>(user_data);
        self.createResources();
        self.refreshCaptureState();
    }

    static void recordFrame(VkCommandBuffer const command, void* const user_data)
    {
        static_cast<Impl*>(user_data)->record(command);
    }

    void record(VkCommandBuffer const command)
    {
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
            m_text_glyph_buffer->update(command, m_text_glyph_upload);
        }
        uint32_t const gui_text_first_instance = static_cast<uint32_t>(m_world_text_batch.glyphs.size());
        DepthTarget& depth_target = *m_current_depth_target;
        if (!depth_target.layout_initialized) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barrier.srcAccessMask = 0U;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.image = depth_target.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier.subresourceRange.levelCount = 1U;
            barrier.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(
                command,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                0U,
                0U,
                nullptr,
                0U,
                nullptr,
                1U,
                &barrier
            );
            depth_target.layout_initialized = true;
        }
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
        VkExtent2D const extent = m_resources->extent();
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth_target.view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil.depth = 1.0F;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1U;
        rendering.colorAttachmentCount = 1U;
        rendering.pColorAttachments = &color_attachment;
        rendering.pDepthAttachment = &depth_attachment;
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


        if (m_chunk_scene_enabled) {
            if (m_height_tile_slots.empty()) {
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
                    glm::mat4 const projection_view = *projection * m_camera.viewMatrix();
                    for (auto const& [coordinate, slot] : m_height_tile_slots) {
                        WrappedBounds const wrapped = boundsNearestToCamera(
                            slot.minimum,
                            slot.maximum,
                            camera_position
                        );
                        if (!isVisibleInFrustum(wrapped.minimum, wrapped.maximum, projection_view)) {
                            continue;
                        }
                        if (coordinate.x < 0 || coordinate.y < 0) {
                            fallback_draw_range = slot.range;
                            continue;
                        }
                        m_visible_stone_draw_ranges.push_back(slot.range);
                    }
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
                merge_ranges(m_visible_stone_draw_ranges);
                merge_ranges(m_visible_solid_stone_draw_ranges);
                if (fallback_draw_range.has_value()) {
                    m_visible_stone_draw_ranges.push_back(*fallback_draw_range);
                }
            }
            m_chunk_draw_count = recordStoneScene(
                command,
                m_stone_pipeline,
                m_stone_solid_pipeline,
                m_stone_layout,
                m_stone_descriptors->set(),
                m_player_pipeline,
                m_player_layout,
                m_camera,
                m_players,
                m_visible_stone_draw_ranges,
                m_visible_solid_stone_draw_ranges,
                extent
            );
        } else {
            recordFlat3dScene(
                command,
                m_grid_pipeline,
                m_grid_layout,
                m_player_pipeline,
                m_player_layout,
                m_camera,
                m_players,
                extent
            );
        }
        if (!m_world_text_batch.glyphs.empty()) {
            std::optional<glm::mat4> const projection = m_camera.projectionMatrix(extent.width, extent.height);
            if (projection.has_value()) {
                uint32_t world_text_draw_count = 0U;
                recordText(
                    command,
                    m_world_text_pipeline,
                    m_world_text_layout,
                    m_text_descriptors->set(),
                    m_world_text_batch,
                    *projection * m_camera.viewMatrix(),
                    extent,
                    static_cast<uint32_t>(textShaderMode(m_world_text_batch.space)),
                    0U,
                    world_text_draw_count
                );
            }
        }
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
                    m_text_descriptors->set(),
                    text,
                    glm::mat4{ 1.0F },
                    extent,
                    static_cast<uint32_t>(textShaderMode(text.space)),
                    first_instance,
                    m_debug_hud_draw_count
                );
                first_instance += static_cast<uint32_t>(text.glyphs.size());
            }
            m_end_rendering(command);
        }
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
        checkResult(vkCreatePipelineLayout(m_resources->device(), &info, nullptr, &layout), "vkCreatePipelineLayout");
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createTextLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(TextPushConstants);
        VkDescriptorSetLayout const descriptor_layout = m_text_descriptors->layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(vkCreatePipelineLayout(m_resources->device(), &info, nullptr, &layout),
            "vkCreatePipelineLayout text");
        return layout;
    }

    [[nodiscard]] VkPipelineLayout createStoneLayout() const
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(StonePushConstants);
        VkDescriptorSetLayout const descriptor_layout = m_stone_descriptors->layout();
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1U;
        info.pSetLayouts = &descriptor_layout;
        info.pushConstantRangeCount = 1U;
        info.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        checkResult(
            vkCreatePipelineLayout(m_resources->device(), &info, nullptr, &layout),
            "vkCreatePipelineLayout stone"
        );
        return layout;
    }

    void createResources()
    {
        m_resources.emplace(m_context->resources());
        auto const dynamic_rendering = m_resources->dynamicRenderingCommands();
        m_begin_rendering = dynamic_rendering.begin;
        m_end_rendering = dynamic_rendering.end;
        if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
            throw std::runtime_error("Vulkan presentation device does not expose dynamic rendering commands");
        }
        selectDepthFormat();
        m_stone_face_buffer.emplace(m_resources->device(), m_resources->physicalDevice());
        m_stone_texture_buffer.emplace(m_resources->device(), m_resources->physicalDevice());
        m_stone_descriptors.emplace(m_resources->device(), *m_stone_face_buffer, *m_stone_texture_buffer);
        m_text_glyph_buffer.emplace(m_resources->device(), m_resources->physicalDevice());
        m_text_descriptors.emplace(m_resources->device(), *m_text_glyph_buffer);
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
            m_shader_assets.load("stone.vert.spv")
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
                .module = fragment,
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
                .required_bindings = { { .set = 0U, .binding = 1U } },
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
                .required_bindings = {},
            }
        ));
        m_sky_program.emplace(core::kernel::GraphicsProgram::create(
            { .module = sky_vertex, .entrypoint = "main", .required_bindings = {} },
            { .module = sky_fragment, .entrypoint = "main", .required_bindings = {} }
        ));
        m_grid_layout = createLayout(static_cast<uint32_t>(sizeof(GridPushConstants)));
        m_player_layout = createLayout(static_cast<uint32_t>(sizeof(BoxPushConstants)));
        m_world_text_layout = createTextLayout();
        m_gui_text_layout = createTextLayout();
        m_stone_layout = createStoneLayout();
        m_kernel_cache.emplace(KERNEL_CACHE_CAPACITY);
        core::graphics::vulkan::VulkanDeviceReference const device = m_resources->deviceReference();
        PresentationPipelineDescriptors const descriptors = presentationPipelineDescriptors(
            m_grid_layout,
            m_player_layout,
            m_world_text_layout,
            m_gui_text_layout,
            m_resources->format(),
            m_depth_format
        );
        m_grid_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_grid_program,
            descriptors.grid
        );
        m_player_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_player_program,
            descriptors.player
        );
        m_world_text_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_text_program,
            descriptors.world_text
        );
        m_gui_text_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_text_program,
            descriptors.gui_text
        );
        m_stone_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_stone_program,
            {
                .layout = m_stone_layout,
                .color_format = m_resources->format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = VK_COMPARE_OP_LESS,
            }
        );
        m_stone_solid_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_stone_solid_program,
            {
                .layout = m_stone_layout,
                .color_format = m_resources->format(),
                .depth_format = m_depth_format,
                .depth_test_enabled = true,
                .depth_write_enabled = true,
                .depth_compare_op = VK_COMPARE_OP_LESS,
            }
        );
        m_sky_pipeline = m_kernel_cache->pipelineFor(
            device,
            *m_sky_program,
            {
                .layout = m_grid_layout,
                .color_format = m_resources->format(),
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
        VkDevice const device = m_resources->device();
        destroyDepthTargets(device);
        if (m_kernel_cache.has_value()) {
            m_kernel_cache->invalidate(m_resources->deviceReference().identity());
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
        m_grid_pipeline = VK_NULL_HANDLE;
        m_player_pipeline = VK_NULL_HANDLE;
        m_world_text_pipeline = VK_NULL_HANDLE;
        m_gui_text_pipeline = VK_NULL_HANDLE;
        m_stone_pipeline = VK_NULL_HANDLE;
        m_stone_solid_pipeline = VK_NULL_HANDLE;
        m_sky_pipeline = VK_NULL_HANDLE;
        m_grid_layout = VK_NULL_HANDLE;
        m_player_layout = VK_NULL_HANDLE;
        m_world_text_layout = VK_NULL_HANDLE;
        m_gui_text_layout = VK_NULL_HANDLE;
        m_stone_layout = VK_NULL_HANDLE;
        m_grid_program.reset();
        m_player_program.reset();
        m_text_program.reset();
        m_stone_program.reset();
        m_stone_solid_program.reset();
        m_sky_program.reset();
        m_stone_descriptors.reset();
        m_stone_texture_buffer.reset();
        m_stone_face_buffer.reset();
        m_text_descriptors.reset();
        m_text_glyph_buffer.reset();
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
                m_resources->physicalDevice(),
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
        VkDevice const device = m_resources->device();
        try {
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = m_depth_format;
            image_info.extent = { .width = m_resources->extent().width, .height = m_resources->extent().height, .depth = 1U };
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            checkResult(vkCreateImage(device, &image_info, nullptr, &target.image), "vkCreateImage depth");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, target.image, &requirements);
            VkPhysicalDeviceMemoryProperties memory_properties{};
            vkGetPhysicalDeviceMemoryProperties(m_resources->physicalDevice(), &memory_properties);
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
    std::optional<StoneFaceBuffer> m_stone_face_buffer;
    std::optional<StoneTextureBuffer> m_stone_texture_buffer;
    std::optional<StoneDescriptorSet> m_stone_descriptors;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_world_text_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_gui_text_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_stone_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
    VkPipeline m_world_text_pipeline = VK_NULL_HANDLE;
    VkPipeline m_gui_text_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_pipeline = VK_NULL_HANDLE;
    VkPipeline m_stone_solid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_sky_pipeline = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR m_begin_rendering = nullptr;
    PFN_vkCmdEndRenderingKHR m_end_rendering = nullptr;
    FrameCaptureState m_capture_state = FrameCaptureState::Disabled;
    std::optional<RendererFrameCapture> m_last_capture;
    std::chrono::nanoseconds m_cpu_acquire_wait_duration{ 0 };
    std::chrono::nanoseconds m_cpu_command_record_duration{ 0 };
    std::chrono::nanoseconds m_cpu_complete_present_wait_duration{ 0 };
    std::chrono::nanoseconds m_cpu_frame_duration{ 0 };
    uint64_t m_submitted_frame_count = 0U;
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
    uint32_t m_debug_hud_draw_count = 0U;
    std::vector<StoneFaceInstance> m_stone_faces;
    StoneDrawRange m_legacy_draw_range;
    std::unordered_map<shared::HeightTileCoordinate, HeightTileSlot, HeightTileCoordinateHash> m_height_tile_slots;
    std::vector<StoneDrawRange> m_visible_stone_draw_ranges;
    std::vector<StoneDrawRange> m_visible_solid_stone_draw_ranges;
    std::vector<StoneDrawRange> m_free_stone_ranges;
    std::vector<RetiredStoneRange> m_retired_stone_ranges;
    uint32_t m_next_stone_face = 0U;
    uint64_t m_chunk_mesh_upload_count = 0U;
    uint32_t m_chunk_draw_count = 0U;
    bool m_chunk_scene_enabled = false;
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
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = m_format;
            image_info.extent = { .width = extent.width, .height = extent.height, .depth = 1U };
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
        , m_stone_face_buffer(m_device->handle(), m_device->physicalDevice())
        , m_stone_texture_buffer(m_device->handle(), m_device->physicalDevice())
        , m_stone_descriptors(m_device->handle(), m_stone_face_buffer, m_stone_texture_buffer)
    {
        try {
            auto const dynamic_rendering = m_device->dynamicRenderingCommands();
            m_begin_rendering = dynamic_rendering.begin;
            m_end_rendering = dynamic_rendering.end;
            if (m_begin_rendering == nullptr || m_end_rendering == nullptr) {
                throw std::runtime_error("offscreen device does not expose dynamic rendering commands");
            }
            m_text_glyph_buffer.emplace(m_device->handle(), m_device->physicalDevice());
            m_text_descriptors.emplace(m_device->handle(), *m_text_glyph_buffer);
            auto const grid = std::make_shared<core::kernel::SpirvModule const>(shader_assets.load("grid.vert.spv"));
            auto const player = std::make_shared<core::kernel::SpirvModule const>(
                shader_assets.load("player.vert.spv")
            );
            auto const stone = std::make_shared<core::kernel::SpirvModule const>(shader_assets.load("stone.vert.spv"));
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
                    .required_bindings = { { .set = 0U, .binding = 1U } },
                }
            ));
            m_grid_layout = createLayout(static_cast<uint32_t>(sizeof(GridPushConstants)));
            m_player_layout = createLayout(static_cast<uint32_t>(sizeof(BoxPushConstants)));
            m_gui_text_layout = createTextLayout();
            m_stone_layout = createStoneLayout();
            core::graphics::vulkan::VulkanDeviceReference const reference = m_device->reference();
            m_grid_pipeline = m_cache.pipelineFor(reference, *m_grid_program, pipelineDescriptor(m_grid_layout));
            m_player_pipeline = m_cache.pipelineFor(reference, *m_player_program, pipelineDescriptor(m_player_layout));
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
    [[nodiscard]] VkPipelineLayout playerLayout() const noexcept { return m_player_layout; }
    [[nodiscard]] VkPipeline guiTextPipeline() const noexcept { return m_gui_text_pipeline; }
    [[nodiscard]] VkPipelineLayout guiTextLayout() const noexcept { return m_gui_text_layout; }
    [[nodiscard]] VkDescriptorSet textDescriptorSet() const noexcept { return m_text_descriptors->set(); }
    [[nodiscard]] VkPipeline stonePipeline() const noexcept { return m_stone_pipeline; }
    [[nodiscard]] VkPipelineLayout stoneLayout() const noexcept { return m_stone_layout; }
    [[nodiscard]] VkDescriptorSet stoneDescriptorSet() const noexcept { return m_stone_descriptors.set(); }
    [[nodiscard]] PFN_vkCmdBeginRenderingKHR beginRendering() const noexcept { return m_begin_rendering; }
    [[nodiscard]] PFN_vkCmdEndRenderingKHR endRendering() const noexcept { return m_end_rendering; }

    void uploadStoneFaces(std::span<StoneFaceInstance const> const faces)
    {
        m_stone_face_buffer.upload(0U, faces);
    }

    void uploadTextGlyphs(VkCommandBuffer const command, std::span<TextGlyph const> const glyphs) const
    {
        m_text_glyph_buffer->update(command, glyphs);
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
        m_gui_text_pipeline = VK_NULL_HANDLE;
        m_stone_pipeline = VK_NULL_HANDLE;
        m_text_program.reset();
        m_text_descriptors.reset();
        m_text_glyph_buffer.reset();
        m_stone_descriptors.destroy();
        m_stone_texture_buffer.destroy();
        m_stone_face_buffer.destroy();
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
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
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
        VkDescriptorSetLayout const descriptor_layout = m_text_descriptors->layout();
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

    [[nodiscard]] core::graphics::vulkan::PipelineDescriptor pipelineDescriptor(VkPipelineLayout const layout) const noexcept
    {
        return {
            .layout = layout,
            .color_format = VK_FORMAT_R8G8B8A8_UNORM,
            .depth_format = m_depth_format,
            .depth_test_enabled = true,
            .depth_write_enabled = true,
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
    StoneTextureBuffer m_stone_texture_buffer;
    StoneDescriptorSet m_stone_descriptors;
    std::optional<TextGlyphBuffer> m_text_glyph_buffer;
    std::optional<TextDescriptorSet> m_text_descriptors;
    VkPipelineLayout m_grid_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_player_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_gui_text_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_stone_layout = VK_NULL_HANDLE;
    VkPipeline m_grid_pipeline = VK_NULL_HANDLE;
    VkPipeline m_player_pipeline = VK_NULL_HANDLE;
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
        if (m_chunk_scene_enabled && instances == m_stone_faces) {
            return;
        }
        m_resources->uploadStoneFaces(instances);
        m_stone_faces = std::move(instances);
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
                resources.playerLayout(),
                self.m_camera,
                self.m_players,
                std::span<StoneDrawRange const>{ &range, self.m_stone_faces.empty() ? 0U : 1U },
                {},
                recording.extent
            ));
        } else {
            recordFlat3dScene(
                recording.command_buffer,
                resources.gridPipeline(),
                resources.gridLayout(),
                resources.playerPipeline(),
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
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = recording.color_view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea.extent = recording.extent;
            info.layerCount = 1U;
            info.colorAttachmentCount = 1U;
            info.pColorAttachments = &color;
            resources.beginRendering()(recording.command_buffer, &info);
            VkViewport viewport{};
            viewport.width = static_cast<float>(recording.extent.width);
            viewport.height = static_cast<float>(recording.extent.height);
            viewport.maxDepth = 1.0F;
            VkRect2D scissor{};
            scissor.extent = recording.extent;
            vkCmdSetViewport(recording.command_buffer, 0U, 1U, &viewport);
            vkCmdSetScissor(recording.command_buffer, 0U, 1U, &scissor);
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
                    static_cast<uint32_t>(textShaderMode(text.space)),
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

void VulkanRenderer::setChunkMesh(shared::ChunkMesh const& mesh)
{
    m_impl->setChunkMesh(mesh);
}

void VulkanRenderer::setChunkMeshes(std::span<shared::ChunkMesh const> const meshes)
{
    m_impl->setChunkMeshes(meshes);
}

void VulkanRenderer::upsertHeightTileMesh(shared::HeightTileSurfaceMesh const& mesh)
{
    m_impl->upsertHeightTileMesh(mesh);
}

bool VulkanRenderer::removeHeightTileMesh(shared::HeightTileCoordinate const coordinate)
{
    return m_impl->removeHeightTileMesh(coordinate);
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
