#include <core/vulkan/builder/PhysicalDeviceSelector.hpp>

#include <gtest/gtest.h>
#include <volk.h>

#include <algorithm>
#include <cstring>

using namespace core::vk;

namespace {

class PhysicalDeviceQueries final {
public:
    explicit PhysicalDeviceQueries(uint32_t const api_version, bool const rendering_extensions = false)
    {
        s_api_version = api_version;
        s_rendering_extensions = rendering_extensions;
        vkEnumeratePhysicalDevices = [](VkInstance, uint32_t* count, VkPhysicalDevice* devices) {
            *count = 1;
            if (devices) {
                *devices = reinterpret_cast<VkPhysicalDevice>(&s_api_version);
            }
            return VK_SUCCESS;
        };
        vkGetPhysicalDeviceQueueFamilyProperties = [](VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties*) {
            *count = 0;
        };
        vkGetPhysicalDeviceProperties = [](VkPhysicalDevice, VkPhysicalDeviceProperties* properties) {
            *properties = {};
            properties->apiVersion = s_api_version;
        };
        vkGetPhysicalDeviceMemoryProperties = [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* properties) {
            *properties = {};
        };
        vkGetPhysicalDeviceFeatures2 = [](VkPhysicalDevice, VkPhysicalDeviceFeatures2*) {};
        vkEnumerateDeviceExtensionProperties = [](
            VkPhysicalDevice,
            char const*,
            uint32_t* count,
            VkExtensionProperties* properties
        ) {
            *count = s_rendering_extensions ? 2 : 0;
            if (properties && s_rendering_extensions) {
                std::strcpy(properties[0].extensionName, "VK_KHR_dynamic_rendering");
                std::strcpy(properties[1].extensionName, "VK_KHR_synchronization2");
                properties[0].specVersion = 1;
                properties[1].specVersion = 1;
            }
            return VK_SUCCESS;
        };
    }

    ~PhysicalDeviceQueries()
    {
        vkEnumeratePhysicalDevices = m_enumerate_devices;
        vkGetPhysicalDeviceQueueFamilyProperties = m_queue_properties;
        vkGetPhysicalDeviceProperties = m_properties;
        vkGetPhysicalDeviceMemoryProperties = m_memory_properties;
        vkGetPhysicalDeviceFeatures2 = m_features;
        vkEnumerateDeviceExtensionProperties = m_extensions;
    }

private:
    inline static uint32_t s_api_version = VK_API_VERSION_1_0;
    inline static bool s_rendering_extensions = false;
    PFN_vkEnumeratePhysicalDevices const m_enumerate_devices = vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties const m_queue_properties = vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceProperties const m_properties = vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties const m_memory_properties = vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFeatures2 const m_features = vkGetPhysicalDeviceFeatures2;
    PFN_vkEnumerateDeviceExtensionProperties const m_extensions = vkEnumerateDeviceExtensionProperties;
};

} // namespace

TEST(PhysicalDeviceSelectorTest, SuccessiveExtensionRequirementsPreserveSwapchain)
{
    PhysicalDeviceSelector selector;
    VulkanExtension swapchain[] = { VulkanExtension::Swapchain };
    VulkanExtension rendering[] = { VulkanExtension::DynamicRendering, VulkanExtension::Synchronization2 };
    selector.requireExtensions(swapchain);
    selector.requireExtensions(rendering);

    EXPECT_TRUE(std::ranges::contains(selector.requiredExtensions(), VulkanExtension::Swapchain));
    EXPECT_TRUE(std::ranges::contains(selector.requiredExtensions(), VulkanExtension::DynamicRendering));
    EXPECT_TRUE(std::ranges::contains(selector.requiredExtensions(), VulkanExtension::Synchronization2));
}

TEST(PhysicalDeviceSelectorTest, SuccessiveExtensionPreferencesAccumulate)
{
    PhysicalDeviceSelector selector;
    VulkanExtension mesh_shader[] = { VulkanExtension::MeshShader };
    VulkanExtension maintenance4[] = { VulkanExtension::Maintenance4 };
    selector.preferExtensions(mesh_shader);
    selector.preferExtensions(maintenance4);

    EXPECT_TRUE(std::ranges::contains(selector.preferredExtensions(), VulkanExtension::MeshShader));
    EXPECT_TRUE(std::ranges::contains(selector.preferredExtensions(), VulkanExtension::Maintenance4));
}

TEST(PhysicalDeviceSelectorTest, Core13SatisfiesRenderingRequirementsWithoutEnablingAbsentExtensions)
{
    static constexpr core::Version API_VERSION{ 0, 1, 3, 0 };

    PhysicalDeviceQueries const queries{ VK_API_VERSION_1_3 };
    VulkanCaps caps;
    caps.commitInstanceCaps(API_VERSION, false, {}, {}, {}, {});
    PhysicalDeviceSelector selector;
    VulkanExtension rendering[] = { VulkanExtension::DynamicRendering, VulkanExtension::Synchronization2 };
    selector.requireExtensions(rendering);

    EXPECT_FALSE(selector.select(caps, Instance{}).isNull());
    EXPECT_TRUE(caps.supportedDeviceExtensionsAsVec().empty());
}

TEST(PhysicalDeviceSelectorTest, Core12StillRequiresRenderingExtensions)
{
    static constexpr core::Version API_VERSION{ 0, 1, 3, 0 };

    PhysicalDeviceQueries const queries{ VK_API_VERSION_1_2 };
    VulkanCaps caps;
    caps.commitInstanceCaps(API_VERSION, false, {}, {}, {}, {});
    PhysicalDeviceSelector selector;
    VulkanExtension rendering[] = { VulkanExtension::DynamicRendering, VulkanExtension::Synchronization2 };
    selector.requireExtensions(rendering);

    EXPECT_THROW(static_cast<void>(selector.select(caps, Instance{})), PhysicalDeviceSelectionError);
}

TEST(PhysicalDeviceSelectorTest, Core12KeepsEnumeratedRenderingExtensionsForDeviceEnablement)
{
    static constexpr core::Version API_VERSION{ 0, 1, 2, 0 };

    PhysicalDeviceQueries const queries{ VK_API_VERSION_1_2, true };
    VulkanCaps caps;
    caps.commitInstanceCaps(API_VERSION, false, {}, {}, {}, {});
    PhysicalDeviceSelector selector;
    VulkanExtension rendering[] = { VulkanExtension::DynamicRendering, VulkanExtension::Synchronization2 };
    selector.requireExtensions(rendering);

    EXPECT_FALSE(selector.select(caps, Instance{}).isNull());
    EXPECT_TRUE(std::ranges::contains(caps.supportedDeviceExtensionsAsVec(), VulkanExtension::DynamicRendering));
    EXPECT_TRUE(std::ranges::contains(caps.supportedDeviceExtensionsAsVec(), VulkanExtension::Synchronization2));
}

TEST(PhysicalDeviceSelectorTest, LowerInstanceVersionCannotUseDeviceCorePromotion)
{
    static constexpr core::Version API_VERSION{ 0, 1, 2, 0 };

    PhysicalDeviceQueries const queries{ VK_API_VERSION_1_3 };
    VulkanCaps caps;
    caps.commitInstanceCaps(API_VERSION, false, {}, {}, {}, {});
    PhysicalDeviceSelector selector;
    VulkanExtension rendering[] = { VulkanExtension::DynamicRendering };
    selector.requireExtensions(rendering);

    EXPECT_THROW(static_cast<void>(selector.select(caps, Instance{})), PhysicalDeviceSelectionError);
}

TEST(PhysicalDeviceSelectorTest, Core13DoesNotReplaceNonPromotedSwapchainExtension)
{
    static constexpr core::Version API_VERSION{ 0, 1, 3, 0 };

    PhysicalDeviceQueries const queries{ VK_API_VERSION_1_3 };
    VulkanCaps caps;
    caps.commitInstanceCaps(API_VERSION, false, {}, {}, {}, {});
    PhysicalDeviceSelector selector;
    VulkanExtension swapchain[] = { VulkanExtension::Swapchain };
    selector.requireExtensions(swapchain);

    EXPECT_THROW(static_cast<void>(selector.select(caps, Instance{})), PhysicalDeviceSelectionError);
}
