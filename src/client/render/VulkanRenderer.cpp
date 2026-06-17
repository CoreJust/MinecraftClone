#include <client/render/VulkanRenderer.hpp>

#include <shared/ProjectInfo.hpp>

#include <core/common/Assert.hpp>
#include <core/IO/File.hpp>
#include <core/IO/Log.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/ErrorCallbacks.hpp>
#include <core/vulkan/InstanceBuilder.hpp>
#include <core/vulkan/PhysicalDeviceSelector.hpp>
#include <core/vulkan/Surface.hpp>
#include <core/vulkan/VulkanVersion.hpp>

// DONT_CHECK INCLUDE_ORDER
#include <volk.h>
// DONT_CHECK INCLUDE_ORDER
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace client {

namespace {

constexpr uint32_t kWorldSize = 32;
constexpr uint32_t kGridWorkgroupsX = 32;
constexpr uint32_t kGridWorkgroupsY = 32;
constexpr uint32_t kMaxFramesInFlight = 2;
constexpr char const* kGridShaderPath = "build/debug/src/client/grid.mesh.spv";
constexpr char const* kPlayerShaderPath = "build/debug/src/client/player.mesh.spv";
constexpr char const* kFragmentShaderPath = "build/debug/src/client/trivial.frag.spv";

struct alignas(16) GridPushConstants final {
    float worldSize = static_cast<float>(kWorldSize);
    float lineWidth = 0.03f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    std::array<float, 4> lineColor{0.16f, 0.16f, 0.18f, 1.0f};
};
static_assert(sizeof(GridPushConstants) == 32);

struct alignas(16) PlayerPushConstants final {
    std::array<float, 2> origin{0.0f, 0.0f};
    float size = 2.0f;
    float pad0 = 0.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(PlayerPushConstants) == 32);

struct SwapchainBundle final {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
};

[[nodiscard]] std::vector<uint32_t> toSpirv(std::vector<uint8_t> const& bytes) {
    ASSERT(bytes.size() % 4 == 0);
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

[[nodiscard]] VkShaderModule loadShaderModule(VkDevice const device, std::string const& path) {
    auto const bytes = core::readFile(path);
    ASSERT(bytes.has_value(), "Failed to read shader file: {}", path);

    auto const words = toSpirv(*bytes);
    VkShaderModuleCreateInfo const info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size() * sizeof(uint32_t),
        .pCode = words.data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    CORE_VK_ASSERT(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

} // namespace

struct VulkanRenderer::Impl final {
public:
    explicit Impl(core::Window const& window)
        : m_window(window)
        , m_instance(core::InstanceBuilder()
            .project(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
            .engine(std::string{ shared::PROJECT_NAME }, shared::PROJECT_VERSION)
            .requireVersion(core::Version{ 0, 1, 3, 0 })
            .requireWindowExtensions()
            .portabilityEnumeration()
            .requireValidation()
            .build(&m_caps))
        , m_surface(m_instance, window)
        , m_physical_device(core::PhysicalDeviceSelector(m_instance)
            .requireQueueFamilies(core::QueueFamily::Graphics)
            .requirePresentQueueFamily(m_surface)
            .requireExtensions(
                core::VulkanExtension::Swapchain,
                core::VulkanExtension::MeshShader)
            .requireFeatures(
                core::VulkanFeature::DynamicRendering,
                core::VulkanFeature::Synchronization2,
                core::VulkanFeature::MeshShader)
            .requireApiVersion(core::Version{ 0, 1, 3, 0 })
            .preferDeviceType(core::PhysicalDeviceType::Discrete)
            .select(&m_caps))
    {
        CORE_INFO("Loaded Vulkan:\n{}", m_caps.toString());
        createDevice();
        createSwapchain();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
        createPipelines();
        core::setOutOfDateKHRCallback([this]{ recreateSwapchain(); return true; });
        core::setSuboptimalKHRCallback([this]{ recreateSwapchain(); return true; });
    }

    ~Impl() {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
        }

        core::setOutOfDateKHRCallback(nullptr);
        core::setSuboptimalKHRCallback(nullptr);
        destroyPipelines();
        destroySyncObjects();
        destroyCommandBuffers();
        destroyCommandPool();
        destroySwapchain();
        destroyDevice();
    }

    void render(std::span<PlayerRenderData const> const players) {
        if (m_window.isFramebufferSizeZero()) {
            return;
        }
        drawFrame(players);
    }

private:
    void createDevice() {
        uint32_t uniqueFamilies[2]{};
        uint32_t uniqueCount = 0;
        auto const addUnique = [&](uint32_t const family) {
            for (uint32_t i = 0; i < uniqueCount; ++i) {
                if (uniqueFamilies[i] == family) {
                    return;
                }
            }
            uniqueFamilies[uniqueCount++] = family;
        };
        addUnique(*m_physical_device.queueFamily(core::QueueFamily::Graphics));
        addUnique(*m_physical_device.queueFamily(core::QueueFamily::Present));

        float const priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfos[2]{};
        for (uint32_t i = 0; i < uniqueCount; ++i) {
            queueInfos[i] = VkDeviceQueueCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = uniqueFamilies[i],
                .queueCount = 1,
                .pQueuePriorities = &priority,
            };
        }

        std::vector<char const*> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_MESH_SHADER_EXTENSION_NAME,
        };

        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            .taskShader = VK_FALSE,
            .meshShader = VK_TRUE,
        };
        VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &meshFeatures,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE,
            .maintenance4 = VK_TRUE,
        };

        VkDeviceCreateInfo const createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features13,
            .queueCreateInfoCount = uniqueCount,
            .pQueueCreateInfos = queueInfos,
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        CORE_VK_ASSERT(vkCreateDevice(m_physical_device.handle(), &createInfo, nullptr, &m_device));
        volkLoadDevice(m_device);
        vkGetDeviceQueue(m_device, *m_physical_device.queueFamily(core::QueueFamily::Graphics), 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, *m_physical_device.queueFamily(core::QueueFamily::Present), 0, &m_presentQueue);
    }

    [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(std::span<VkSurfaceFormatKHR const> const formats) const {
        for (auto const& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return formats.front();
    }

    [[nodiscard]] VkPresentModeKHR choosePresentMode(std::span<VkPresentModeKHR const> const modes) const {
        for (auto const mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    [[nodiscard]] VkExtent2D chooseExtent(VkSurfaceCapabilitiesKHR const& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        auto const [width, height] = m_window.framebufferSize();
        return VkExtent2D{
            std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
        };
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        CORE_VK_ASSERT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device.handle(), m_surface.handle(), &capabilities));

        uint32_t formatCount = 0;
        CORE_VK_ASSERT(vkGetPhysicalDeviceSurfaceFormatsKHR(
            m_physical_device.handle(), m_surface.handle(), &formatCount, nullptr));
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        CORE_VK_ASSERT(vkGetPhysicalDeviceSurfaceFormatsKHR(
            m_physical_device.handle(), m_surface.handle(), &formatCount, formats.data()));

        uint32_t presentModeCount = 0;
        CORE_VK_ASSERT(vkGetPhysicalDeviceSurfacePresentModesKHR(
            m_physical_device.handle(), m_surface.handle(), &presentModeCount, nullptr));
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        CORE_VK_ASSERT(vkGetPhysicalDeviceSurfacePresentModesKHR(
            m_physical_device.handle(), m_surface.handle(), &presentModeCount, presentModes.data()));

        ASSERT(!formats.empty());
        auto const chosenFormat = chooseSurfaceFormat(formats);
        m_swapchain.format = chosenFormat.format;
        m_swapchain.extent = chooseExtent(capabilities);

        uint32_t imageCount = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0U) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }

        uint32_t queueFamilyIndices[2]{
            *m_physical_device.queueFamily(core::QueueFamily::Graphics),
            *m_physical_device.queueFamily(core::QueueFamily::Present),
        };

        VkSwapchainCreateInfoKHR const createInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = m_surface.handle(),
            .minImageCount = imageCount,
            .imageFormat = chosenFormat.format,
            .imageColorSpace = chosenFormat.colorSpace,
            .imageExtent = m_swapchain.extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = (queueFamilyIndices[0] != queueFamilyIndices[1])
                ? VK_SHARING_MODE_CONCURRENT
                : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = (queueFamilyIndices[0] != queueFamilyIndices[1]) ? 2U : 0U,
            .pQueueFamilyIndices = (queueFamilyIndices[0] != queueFamilyIndices[1]) ? queueFamilyIndices : nullptr,
            .preTransform = capabilities.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = choosePresentMode(presentModes),
            .clipped = VK_TRUE,
            .oldSwapchain = m_swapchain.swapchain,
        };

        VkSwapchainKHR const oldSwapchain = m_swapchain.swapchain;
        VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
        CORE_VK_ASSERT(vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain));

        uint32_t newImageCount = 0;
        CORE_VK_ASSERT(vkGetSwapchainImagesKHR(m_device, newSwapchain, &newImageCount, nullptr));
        std::vector<VkImage> images(newImageCount);
        CORE_VK_ASSERT(vkGetSwapchainImagesKHR(m_device, newSwapchain, &newImageCount, images.data()));

        if (oldSwapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, oldSwapchain, nullptr);
        }

        m_swapchain.swapchain = newSwapchain;
        m_swapchain.images = std::move(images);
        m_swapchain.views.clear();
        m_swapchain.views.reserve(m_swapchain.images.size());

        for (auto const image : m_swapchain.images) {
            VkImageViewCreateInfo const viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = m_swapchain.format,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };

            VkImageView view = VK_NULL_HANDLE;
            CORE_VK_ASSERT(vkCreateImageView(m_device, &viewInfo, nullptr, &view));
            m_swapchain.views.push_back(view);
        }
    }

    void recreateSwapchain() {
        CORE_VK_ASSERT(vkDeviceWaitIdle(m_device));
        destroyPipelines();
        destroyCommandBuffers();
        destroySwapchain();
        createSwapchain();
        createCommandBuffers();
        createPipelines();
    }

    void destroySwapchain() {
        for (auto const view : m_swapchain.views) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, view, nullptr);
            }
        }
        m_swapchain.views.clear();

        if (m_swapchain.swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain.swapchain, nullptr);
            m_swapchain.swapchain = VK_NULL_HANDLE;
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo const info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = *m_physical_device.queueFamily(core::QueueFamily::Graphics),
        };
        CORE_VK_ASSERT(vkCreateCommandPool(m_device, &info, nullptr, &m_commandPool));
    }

    void destroyCommandPool() {
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }
    }

    void createCommandBuffers() {
        VkCommandBufferAllocateInfo const info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = kMaxFramesInFlight,
        };
        m_commandBuffers.resize(kMaxFramesInFlight);
        CORE_VK_ASSERT(vkAllocateCommandBuffers(m_device, &info, m_commandBuffers.data()));
    }

    void destroyCommandBuffers() {
        if (!m_commandBuffers.empty() && m_commandPool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_device, m_commandPool, static_cast<uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
            m_commandBuffers.clear();
        }
    }

    void createSyncObjects() {
        VkSemaphoreCreateInfo const semInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkFenceCreateInfo const fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        m_imageAvailable.resize(kMaxFramesInFlight);
        m_renderFinished.resize(kMaxFramesInFlight);
        m_inFlight.resize(kMaxFramesInFlight);

        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            CORE_VK_ASSERT(vkCreateSemaphore(m_device, &semInfo, nullptr, &m_imageAvailable[i]));
            CORE_VK_ASSERT(vkCreateSemaphore(m_device, &semInfo, nullptr, &m_renderFinished[i]));
            CORE_VK_ASSERT(vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlight[i]));
        }
    }

    void destroySyncObjects() {
        for (auto const fence : m_inFlight) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, fence, nullptr);
            }
        }
        for (auto const semaphore : m_imageAvailable) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        for (auto const semaphore : m_renderFinished) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        m_inFlight.clear();
        m_imageAvailable.clear();
        m_renderFinished.clear();
    }

    void createPipelines() {
        m_gridMeshShader = loadShaderModule(m_device, kGridShaderPath);
        m_playerMeshShader = loadShaderModule(m_device, kPlayerShaderPath);
        m_fragmentShader = loadShaderModule(m_device, kFragmentShaderPath);

        VkPushConstantRange const gridRange{
            .stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT,
            .offset = 0,
            .size = sizeof(GridPushConstants),
        };
        VkPushConstantRange const playerRange{
            .stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT,
            .offset = 0,
            .size = sizeof(PlayerPushConstants),
        };

        VkPipelineLayoutCreateInfo const gridLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &gridRange,
        };
        VkPipelineLayoutCreateInfo const playerLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &playerRange,
        };

        CORE_VK_ASSERT(vkCreatePipelineLayout(m_device, &gridLayoutInfo, nullptr, &m_gridLayout));
        CORE_VK_ASSERT(vkCreatePipelineLayout(m_device, &playerLayoutInfo, nullptr, &m_playerLayout));

        VkPipelineRenderingCreateInfo const gridRendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &m_swapchain.format,
        };
        VkPipelineRenderingCreateInfo const playerRendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &m_swapchain.format,
        };

        VkPipelineShaderStageCreateInfo const gridStages[2]{
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
                .module = m_gridMeshShader,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = m_fragmentShader,
                .pName = "main",
            },
        };
        VkPipelineShaderStageCreateInfo const playerStages[2]{
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
                .module = m_playerMeshShader,
                .pName = "main",
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = m_fragmentShader,
                .pName = "main",
            },
        };

        VkPipelineVertexInputStateCreateInfo const vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo const inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };
        VkPipelineViewportStateCreateInfo const viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo const rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo const multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState const attachment{
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                | VK_COLOR_COMPONENT_G_BIT
                | VK_COLOR_COMPONENT_B_BIT
                | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo const blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &attachment,
        };
        VkDynamicState const dynamicStates[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo const dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates,
        };

        VkGraphicsPipelineCreateInfo const gridInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &gridRendering,
            .stageCount = 2,
            .pStages = gridStages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamicState,
            .layout = m_gridLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
        };
        VkGraphicsPipelineCreateInfo const playerInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &playerRendering,
            .stageCount = 2,
            .pStages = playerStages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamicState,
            .layout = m_playerLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
        };

        VkGraphicsPipelineCreateInfo const infos[2]{gridInfo, playerInfo};
        VkPipeline pipelines[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        CORE_VK_ASSERT(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 2, infos, nullptr, pipelines));

        m_gridPipeline = pipelines[0];
        m_playerPipeline = pipelines[1];
    }

    void destroyPipelines() {
        if (m_device != VK_NULL_HANDLE) {
            if (m_gridPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_gridPipeline, nullptr);
                m_gridPipeline = VK_NULL_HANDLE;
            }
            if (m_playerPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_playerPipeline, nullptr);
                m_playerPipeline = VK_NULL_HANDLE;
            }
            if (m_gridLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_gridLayout, nullptr);
                m_gridLayout = VK_NULL_HANDLE;
            }
            if (m_playerLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_playerLayout, nullptr);
                m_playerLayout = VK_NULL_HANDLE;
            }
            if (m_gridMeshShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_device, m_gridMeshShader, nullptr);
                m_gridMeshShader = VK_NULL_HANDLE;
            }
            if (m_playerMeshShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_device, m_playerMeshShader, nullptr);
                m_playerMeshShader = VK_NULL_HANDLE;
            }
            if (m_fragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_device, m_fragmentShader, nullptr);
                m_fragmentShader = VK_NULL_HANDLE;
            }
        }
    }

    void drawFrame(std::span<PlayerRenderData const> const players) {
        uint32_t const frame = static_cast<uint32_t>(m_frameIndex % kMaxFramesInFlight);
        CORE_VK_ASSERT(vkWaitForFences(m_device, 1, &m_inFlight[frame], VK_TRUE, std::numeric_limits<uint64_t>::max()));

        uint32_t imageIndex = 0;
        CORE_VK_ASSERT(vkAcquireNextImageKHR(
            m_device,
            m_swapchain.swapchain,
            std::numeric_limits<uint64_t>::max(),
            m_imageAvailable[frame],
            VK_NULL_HANDLE,
            &imageIndex));

        CORE_VK_ASSERT(vkResetFences(m_device, 1, &m_inFlight[frame]));
        CORE_VK_ASSERT(vkResetCommandBuffer(m_commandBuffers[frame], 0));
        record(m_commandBuffers[frame], imageIndex, players);

        VkPipelineStageFlags const waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo const submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &m_imageAvailable[frame],
            .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1,
            .pCommandBuffers = &m_commandBuffers[frame],
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &m_renderFinished[frame],
        };
        CORE_VK_ASSERT(vkQueueSubmit(m_graphicsQueue, 1, &submit, m_inFlight[frame]));

        VkPresentInfoKHR const present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &m_renderFinished[frame],
            .swapchainCount = 1,
            .pSwapchains = &m_swapchain.swapchain,
            .pImageIndices = &imageIndex,
        };

        CORE_VK_ASSERT(vkQueuePresentKHR(m_presentQueue, &present));

        ++m_frameIndex;
    }

    void record(VkCommandBuffer const cmd, uint32_t const imageIndex, std::span<PlayerRenderData const> const players) {
        VkCommandBufferBeginInfo const beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        CORE_VK_ASSERT(vkBeginCommandBuffer(cmd, &beginInfo));

        VkImageMemoryBarrier2 const toColor{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = m_swapchain.images[imageIndex],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkDependencyInfo const depToColor{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toColor,
        };
        vkCmdPipelineBarrier2(cmd, &depToColor);

        VkClearValue const clearValue{
            .color = VkClearColorValue{{0.06f, 0.06f, 0.08f, 1.0f}},
        };
        VkRenderingAttachmentInfo const colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = m_swapchain.views[imageIndex],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clearValue,
        };
        VkRenderingInfo const renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {
                .offset = {0, 0},
                .extent = m_swapchain.extent,
            },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
        };

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport const viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(m_swapchain.extent.width),
            .height = static_cast<float>(m_swapchain.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D const scissor{
            .offset = {0, 0},
            .extent = m_swapchain.extent,
        };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        GridPushConstants const gridPush{};
        vkCmdPushConstants(cmd, m_gridLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(GridPushConstants), &gridPush);
        vkCmdDrawMeshTasksEXT(cmd, kGridWorkgroupsX, kGridWorkgroupsY, 1);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_playerPipeline);
        for (auto const& player : players) {
            PlayerPushConstants push{};
            push.origin = {static_cast<float>(player.x), static_cast<float>(player.y)};
            push.size = 2.0f;
            push.color = player.color;
            vkCmdPushConstants(cmd, m_playerLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(PlayerPushConstants), &push);
            vkCmdDrawMeshTasksEXT(cmd, 1, 1, 1);
        }

        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 const toPresent{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = VK_ACCESS_2_NONE,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .image = m_swapchain.images[imageIndex],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkDependencyInfo const depToPresent{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toPresent,
        };
        vkCmdPipelineBarrier2(cmd, &depToPresent);

        CORE_VK_ASSERT(vkEndCommandBuffer(cmd));
    }

    void destroyDevice() {
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
    }
private:
    core::Window const& m_window;

    core::VulkanCaps m_caps;
    core::Instance m_instance;
    core::Surface m_surface;
    core::PhysicalDevice m_physical_device;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;

    SwapchainBundle m_swapchain{};
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore> m_imageAvailable;
    std::vector<VkSemaphore> m_renderFinished;
    std::vector<VkFence> m_inFlight;

    VkShaderModule m_gridMeshShader = VK_NULL_HANDLE;
    VkShaderModule m_playerMeshShader = VK_NULL_HANDLE;
    VkShaderModule m_fragmentShader = VK_NULL_HANDLE;
    VkPipelineLayout m_gridLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_playerLayout = VK_NULL_HANDLE;
    VkPipeline m_gridPipeline = VK_NULL_HANDLE;
    VkPipeline m_playerPipeline = VK_NULL_HANDLE;

    uint64_t m_frameIndex = 0;
};

VulkanRenderer::VulkanRenderer(core::Window const& window)
    : m_impl(std::make_unique<Impl>(window))
{ }

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::render(std::span<PlayerRenderData const> const players) {
    m_impl->render(players);
}

} // namespace client
