#include <core/vulkan/QueueFamily.hpp>

#include <core/common/IterEnum.hpp>
#include <core/vulkan/Check.hpp>

#include <volk.h>

namespace core {

std::string to_string(QueueFamily const family) {
    switch (family) {
        case QueueFamily::Graphics:      return "Graphics";
        case QueueFamily::Compute:       return "Compute";
        case QueueFamily::Transfer:      return "Transfer";
        case QueueFamily::SparseBinding: return "SparseBinding";
        case QueueFamily::Protected:     return "Protected";
        case QueueFamily::VideoDecode:   return "VideoDecode";
        case QueueFamily::VideoEncode:   return "VideoEncode";
        case QueueFamily::Present:       return "Present";
    case QueueFamily::Count: return "Count";
    }
}

QueueFamilies QueueFamilies::query(VkPhysicalDevice const device, VkSurfaceKHR const surface) {
    QueueFamilies families{ };

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    for (uint32_t i = 0; i < queue_family_count; ++i) {
        for (QueueFamily const family : iterEnum<QueueFamily>()) {
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

} // namespace core
