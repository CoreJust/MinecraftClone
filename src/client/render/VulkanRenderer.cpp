#include <client/render/VulkanRenderer.hpp>

#include <core/IO/File.hpp>
#include <core/IO/Log.hpp>
#include <core/common/Assert.hpp>

#include <vulkan/vulkan.h>
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

constexpr std::uint32_t kWorldSize = 32;
constexpr std::uint32_t kGridWorkgroupsX = 32;
constexpr std::uint32_t kGridWorkgroupsY = 32;
constexpr std::uint32_t kMaxFramesInFlight = 2;
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

struct QueueFamilyIndices final {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    [[nodiscard]] constexpr bool complete() const noexcept {
        return graphics.has_value() && present.has_value();
    }
};

struct SwapchainBundle final {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
};

[[nodiscard]] std::vector<std::uint32_t> toSpirv(std::vector<std::uint8_t> const& bytes) {
    ASSERT(bytes.size() % 4 == 0);
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

[[nodiscard]] VkShaderModule loadShaderModule(VkDevice const device, std::string const& path) {
    auto const bytes = core::readFile(path);
    if (!bytes.has_value()) {
        MC_CRITICAL("Failed to read shader file: {}", path);
        ASSERT(false);
    }

    auto const words = toSpirv(*bytes);
    VkShaderModuleCreateInfo const info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size() * sizeof(std::uint32_t),
        .pCode = words.data(),
    };

    VkShaderModule module = VK_NULL_HANDLE;
    auto const result = vkCreateShaderModule(device, &info, nullptr, &module);
    if (result != VK_SUCCESS) {
        MC_CRITICAL("vkCreateShaderModule failed for {} (error {})", path, static_cast<int>(result));
        ASSERT(false);
    }
    return module;
}

} // namespace

struct VulkanRenderer::Impl final {
public:
    explicit Impl(core::Window const& window)
        : m_window(window) {
        createInstance();
        createSurface();
        pickPhysicalDevice();
        createDevice();
        loadDeviceDispatch();
        createSwapchain();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
        createPipelines();
    }

    ~Impl() {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
        }

        destroyPipelines();
        destroySyncObjects();
        destroyCommandBuffers();
        destroyCommandPool();
        destroySwapchain();
        destroyDevice();
        destroySurface();
        destroyInstance();
    }

    void render(std::span<PlayerRenderData const> const players) {
        if (m_window.isFramebufferSizeZero()) {
            return;
        }
        drawFrame(players);
    }

private:
    using PFN_vkCmdBeginRendering = void(VKAPI_PTR*)(VkCommandBuffer, const VkRenderingInfo*);
    using PFN_vkCmdEndRendering = void(VKAPI_PTR*)(VkCommandBuffer);
    using PFN_vkCmdPipelineBarrier2 = void(VKAPI_PTR*)(VkCommandBuffer, const VkDependencyInfo*);
    using PFN_vkCmdDrawMeshTasksEXT = void(VKAPI_PTR*)(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t);

    void createInstance() {
        if (glfwVulkanSupported() != GLFW_TRUE) {
            MC_CRITICAL("GLFW reports Vulkan is not supported");
            ASSERT(false);
        }

        std::uint32_t glfwExtensionCount = 0;
        auto const* glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
            MC_CRITICAL("glfwGetRequiredInstanceExtensions returned nothing");
            ASSERT(false);
        }

        std::vector<char const*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

#if !defined(NDEBUG)
        std::vector<char const*> layers{"VK_LAYER_KHRONOS_validation"};
#else
        std::vector<char const*> layers{};
#endif

        VkApplicationInfo const appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName = "Minimal Vulkan Renderer",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "Core",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = VK_API_VERSION_1_3,
        };

        VkInstanceCreateInfo const createInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        auto const result = vkCreateInstance(&createInfo, nullptr, &m_instance);
        ASSERT(result == VK_SUCCESS, "vkCreateInstance failed (error {})", static_cast<int>(result));
    }

    void createSurface() {
        auto const result = glfwCreateWindowSurface(m_instance, m_window.nativeHandle(), nullptr, &m_surface);
        ASSERT(result == VK_SUCCESS, "glfwCreateWindowSurface failed (error {})", static_cast<int>(result));
    }

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice const device) const {
        QueueFamilyIndices indices{};
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

        for (std::uint32_t i = 0; i < count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                indices.graphics = i;
            }

            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &supported) == VK_SUCCESS && supported == VK_TRUE) {
                indices.present = i;
            }

            if (indices.complete()) {
                break;
            }
        }

        return indices;
    }

    [[nodiscard]] bool supportsRequiredExtensions(VkPhysicalDevice const device) const {
        std::uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
            return false;
        }

        std::vector<VkExtensionProperties> props(count);
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data()) != VK_SUCCESS) {
            return false;
        }

        bool hasSwapchain = false;
        bool hasMeshShader = false;
        for (auto const& prop : props) {
            if (std::strcmp(prop.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchain = true;
            }
            if (std::strcmp(prop.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
                hasMeshShader = true;
            }
        }
        return hasSwapchain && hasMeshShader;
    }

    void pickPhysicalDevice() {
        std::uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(m_instance, &count, nullptr) != VK_SUCCESS || count == 0U) {
            MC_CRITICAL("No Vulkan physical devices found");
            ASSERT(false);
        }

        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

        for (auto const device : devices) {
            if (!supportsRequiredExtensions(device)) {
                continue;
            }

            auto const indices = findQueueFamilies(device);
            if (!indices.complete()) {
                continue;
            }

            VkPhysicalDeviceFeatures2 features2{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            };
            VkPhysicalDeviceVulkan13Features features13{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            };
            VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            };

            features2.pNext = &features13;
            features13.pNext = &meshFeatures;
            vkGetPhysicalDeviceFeatures2(device, &features2);

            if (features13.dynamicRendering != VK_TRUE) {
                continue;
            }
            if (features13.synchronization2 != VK_TRUE) {
                continue;
            }
            if (meshFeatures.meshShader != VK_TRUE) {
                continue;
            }

            m_physicalDevice = device;
            m_queueFamilies = indices;
            MC_INFO("Selected Vulkan physical device");
            return;
        }

        MC_CRITICAL("No suitable Vulkan device with mesh shader and Vulkan 1.3 features was found");
        ASSERT(false);
    }

    void createDevice() {
        std::uint32_t uniqueFamilies[2]{};
        std::uint32_t uniqueCount = 0;
        auto const addUnique = [&](std::uint32_t const family) {
            for (std::uint32_t i = 0; i < uniqueCount; ++i) {
                if (uniqueFamilies[i] == family) {
                    return;
                }
            }
            uniqueFamilies[uniqueCount++] = family;
        };
        addUnique(*m_queueFamilies.graphics);
        addUnique(*m_queueFamilies.present);

        float const priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfos[2]{};
        for (std::uint32_t i = 0; i < uniqueCount; ++i) {
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
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        auto const result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
        if (result != VK_SUCCESS) {
            MC_CRITICAL("vkCreateDevice failed (error {})", static_cast<int>(result));
            ASSERT(false);
        }

        vkGetDeviceQueue(m_device, *m_queueFamilies.graphics, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, *m_queueFamilies.present, 0, &m_presentQueue);
    }

    void loadDeviceDispatch() {
        m_vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(m_device, "vkCmdBeginRendering"));
        m_vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(m_device, "vkCmdEndRendering"));
        m_vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(m_device, "vkCmdPipelineBarrier2"));
        m_vkCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(m_device, "vkCmdDrawMeshTasksEXT"));

        if (m_vkCmdBeginRendering == nullptr ||
            m_vkCmdEndRendering == nullptr ||
            m_vkCmdPipelineBarrier2 == nullptr ||
            m_vkCmdDrawMeshTasksEXT == nullptr) {
            MC_CRITICAL("Required Vulkan device functions were not loaded");
            ASSERT(false);
        }
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
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
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
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data());

        ASSERT(!formats.empty());
        auto const chosenFormat = chooseSurfaceFormat(formats);
        m_swapchain.format = chosenFormat.format;
        m_swapchain.extent = chooseExtent(capabilities);

        std::uint32_t imageCount = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0U) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }

        std::uint32_t queueFamilyIndices[2]{*m_queueFamilies.graphics, *m_queueFamilies.present};

        VkSwapchainCreateInfoKHR const createInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = m_surface,
            .minImageCount = imageCount,
            .imageFormat = chosenFormat.format,
            .imageColorSpace = chosenFormat.colorSpace,
            .imageExtent = m_swapchain.extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .imageSharingMode = (*m_queueFamilies.graphics != *m_queueFamilies.present) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = (*m_queueFamilies.graphics != *m_queueFamilies.present) ? 2U : 0U,
            .pQueueFamilyIndices = (*m_queueFamilies.graphics != *m_queueFamilies.present) ? queueFamilyIndices : nullptr,
            .preTransform = capabilities.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = choosePresentMode(presentModes),
            .clipped = VK_TRUE,
            .oldSwapchain = m_swapchain.swapchain,
        };

        VkSwapchainKHR const oldSwapchain = m_swapchain.swapchain;
        VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
        auto const result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain);
        if (result != VK_SUCCESS) {
            MC_CRITICAL("vkCreateSwapchainKHR failed (error {})", static_cast<int>(result));
            ASSERT(false);
        }

        std::uint32_t newImageCount = 0;
        vkGetSwapchainImagesKHR(m_device, newSwapchain, &newImageCount, nullptr);
        std::vector<VkImage> images(newImageCount);
        vkGetSwapchainImagesKHR(m_device, newSwapchain, &newImageCount, images.data());

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
            auto const viewResult = vkCreateImageView(m_device, &viewInfo, nullptr, &view);
            if (viewResult != VK_SUCCESS) {
                MC_CRITICAL("vkCreateImageView failed (error {})", static_cast<int>(viewResult));
                ASSERT(false);
            }
            m_swapchain.views.push_back(view);
        }
    }

    void recreateSwapchain() {
        vkDeviceWaitIdle(m_device);
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
            .queueFamilyIndex = *m_queueFamilies.graphics,
        };
        auto const result = vkCreateCommandPool(m_device, &info, nullptr, &m_commandPool);
        if (result != VK_SUCCESS) {
            MC_CRITICAL("vkCreateCommandPool failed");
            ASSERT(false);
        }
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
        auto const result = vkAllocateCommandBuffers(m_device, &info, m_commandBuffers.data());
        if (result != VK_SUCCESS) {
            MC_CRITICAL("vkAllocateCommandBuffers failed");
            ASSERT(false);
        }
    }

    void destroyCommandBuffers() {
        if (!m_commandBuffers.empty() && m_commandPool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_device, m_commandPool, static_cast<std::uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
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

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (vkCreateSemaphore(m_device, &semInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_device, &semInfo, nullptr, &m_renderFinished[i]) != VK_SUCCESS ||
                vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS) {
                MC_CRITICAL("Failed to create synchronization objects");
                ASSERT(false);
            }
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

        if (vkCreatePipelineLayout(m_device, &gridLayoutInfo, nullptr, &m_gridLayout) != VK_SUCCESS ||
            vkCreatePipelineLayout(m_device, &playerLayoutInfo, nullptr, &m_playerLayout) != VK_SUCCESS) {
            MC_CRITICAL("vkCreatePipelineLayout failed");
            ASSERT(false);
        }

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
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                              VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
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
        auto const result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 2, infos, nullptr, pipelines);
        if (result != VK_SUCCESS) {
            MC_CRITICAL("vkCreateGraphicsPipelines failed (error {})", static_cast<int>(result));
            ASSERT(false);
        }

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
        std::uint32_t const frame = static_cast<std::uint32_t>(m_frameIndex % kMaxFramesInFlight);
        vkWaitForFences(m_device, 1, &m_inFlight[frame], VK_TRUE, std::numeric_limits<std::uint64_t>::max());

        std::uint32_t imageIndex = 0;
        auto const acquire = vkAcquireNextImageKHR(
            m_device,
            m_swapchain.swapchain,
            std::numeric_limits<std::uint64_t>::max(),
            m_imageAvailable[frame],
            VK_NULL_HANDLE,
            &imageIndex);

        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            MC_CRITICAL("vkAcquireNextImageKHR failed (error {})", static_cast<int>(acquire));
            ASSERT(false);
        }

        vkResetFences(m_device, 1, &m_inFlight[frame]);
        vkResetCommandBuffer(m_commandBuffers[frame], 0);
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
        if (vkQueueSubmit(m_graphicsQueue, 1, &submit, m_inFlight[frame]) != VK_SUCCESS) {
            MC_CRITICAL("vkQueueSubmit failed");
            ASSERT(false);
        }

        VkPresentInfoKHR const present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &m_renderFinished[frame],
            .swapchainCount = 1,
            .pSwapchains = &m_swapchain.swapchain,
            .pImageIndices = &imageIndex,
        };

        auto const presentResult = vkQueuePresentKHR(m_presentQueue, &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || m_window.isFramebufferSizeZero()) {
            recreateSwapchain();
        } else if (presentResult != VK_SUCCESS) {
            MC_CRITICAL("vkQueuePresentKHR failed (error {})", static_cast<int>(presentResult));
            ASSERT(false);
        }

        ++m_frameIndex;
    }

    void record(VkCommandBuffer const cmd, std::uint32_t const imageIndex, std::span<PlayerRenderData const> const players) {
        VkCommandBufferBeginInfo const beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            MC_CRITICAL("vkBeginCommandBuffer failed");
            ASSERT(false);
        }

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
        m_vkCmdPipelineBarrier2(cmd, &depToColor);

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

        m_vkCmdBeginRendering(cmd, &renderingInfo);

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
        m_vkCmdDrawMeshTasksEXT(cmd, kGridWorkgroupsX, kGridWorkgroupsY, 1);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_playerPipeline);
        for (auto const& player : players) {
            PlayerPushConstants push{};
            push.origin = {static_cast<float>(player.x), static_cast<float>(player.y)};
            push.size = 2.0f;
            push.color = player.color;
            vkCmdPushConstants(cmd, m_playerLayout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(PlayerPushConstants), &push);
            m_vkCmdDrawMeshTasksEXT(cmd, 1, 1, 1);
        }

        m_vkCmdEndRendering(cmd);

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
        m_vkCmdPipelineBarrier2(cmd, &depToPresent);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            MC_CRITICAL("vkEndCommandBuffer failed");
            ASSERT(false);
        }
    }

    void destroyDevice() {
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
    }

    void destroySurface() {
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
    }

    void destroyInstance() {
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
    }

private:
    core::Window const& m_window;

    PFN_vkCmdBeginRendering m_vkCmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering m_vkCmdEndRendering = nullptr;
    PFN_vkCmdPipelineBarrier2 m_vkCmdPipelineBarrier2 = nullptr;
    PFN_vkCmdDrawMeshTasksEXT m_vkCmdDrawMeshTasksEXT = nullptr;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    QueueFamilyIndices m_queueFamilies{};
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

    std::uint64_t m_frameIndex = 0;
};

VulkanRenderer::VulkanRenderer(core::Window const& window)
    : m_impl(std::make_unique<Impl>(window))
{ }

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::render(std::span<PlayerRenderData const> const players) {
    m_impl->render(players);
}

} // namespace client
