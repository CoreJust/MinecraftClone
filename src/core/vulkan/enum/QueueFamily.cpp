#include <core/vulkan/enum/QueueFamily.hpp>

#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>

#include <volk.h>

CORE_ENUM_FUNCTIONS_IMPL(vk::QueueFamily);

namespace core::vk {

QueueFamilies QueueFamilies::query(VkPhysicalDevice const device, VkSurfaceKHR const surface) {
    QueueFamilies families{ };

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    for (uint32_t i = 0; i < queue_family_count; ++i) {
        for (QueueFamily const family : valuesOf<QueueFamily>()) {
            std::optional flag_bit = queueFamilyFlagBitOf(family);
            if (!flag_bit.has_value()) {
                continue;
            }
            if ((queue_families[i].queueFlags & *flag_bit) && families[family] == NO_INDEX) {
                families[family] = i;
            }
        }

        if (surface != VK_NULL_HANDLE) {
            VkBool32 present_supported = VK_FALSE;
            if (true
                && VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_supported))
                && present_supported == VK_TRUE
                && families[QueueFamily::Present] == NO_INDEX
            ) {
                families[QueueFamily::Present] = i;
            }
        }
    }

    return families;
}

} // namespace core::vk
