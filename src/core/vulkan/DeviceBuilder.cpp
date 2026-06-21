#include <core/vulkan/DeviceBuilder.hpp>

#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/internal/PhysicalDeviceCapsStruct.hpp>

#include <volk.h>

#include <unordered_map>

namespace core {
namespace {

std::vector<TrivialPair<uint32_t, float>> collectUniqueQueueFamilies(
    std::vector<TrivialPair<QueueFamily, float>> const& families,
    PhysicalDevice const& device
) {
    std::unordered_map<uint32_t, float> unique_families{ };
    unique_families.reserve(families.size());
    for (auto const [family, priority] : families) {
        std::optional<uint32_t> const family_index = device.queueFamily(family);
        if (!family_index) {
            throw DeviceCreationError(
                DeviceCreationError::PhysicalDeviceHasNoSuchFamily,
                "{}", family
            );
        }
        if (auto it = unique_families.find(*family_index); it != unique_families.end()) {
            if (priority > it->second) {
                it->second = priority;
            }
        } else {
            unique_families.emplace(*family_index, priority);
        }
    }

    std::vector<TrivialPair<uint32_t, float>> result{ };
    result.reserve(unique_families.size());
    for (auto const [family, priority] : unique_families) {
        result.push_back({ family, priority });
    }
    return result;
}

internal::PhysicalDeviceCapsStruct collectFeatures(VulkanCaps const& caps) {
    internal::PhysicalDeviceCapsStruct physical_caps{ };
    VulkanFeatures const& supported_features = caps.supportedFeatures();
    for (VulkanFeature const feature : valuesOf<VulkanFeature>()) {
        if (supported_features[feature]) {
            physical_caps.setFeature(feature);
        }
    }
    return physical_caps;
}

} // namespace

CORE_ENUM_FUNCTIONS_IMPL(DeviceCreationErrorKind);

Device DeviceBuilder::build(VulkanCaps& caps) const {
    auto const unique_families = collectUniqueQueueFamilies(m_required_queue_families, m_physical_device);
    std::vector<VkDeviceQueueCreateInfo> queue_infos{ };
    queue_infos.reserve(unique_families.size());
    for (size_t i = 0; i < unique_families.size(); ++i) {
        queue_infos.push_back(VkDeviceQueueCreateInfo {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = unique_families[i].first,
            .queueCount = 1,
            .pQueuePriorities = &unique_families[i].second,
        });
    }

    std::vector<VulkanExtension> exts = caps.supportedDeviceExtensionsAsVec();
    std::vector<char const*> ext_names{ };
    ext_names.reserve(exts.size());
    for (VulkanExtension const ext : exts) {
        ext_names.push_back(getFullExtensionName(ext));
    }

    internal::PhysicalDeviceCapsStruct physical_caps = collectFeatures(caps);

    VkDeviceCreateInfo const createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = physical_caps.chained(caps.instanceVersion()),
        .queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size()),
        .pQueueCreateInfos = queue_infos.data(),
        .enabledExtensionCount = static_cast<uint32_t>(ext_names.size()),
        .ppEnabledExtensionNames = ext_names.data(),
        .pEnabledFeatures = &physical_caps.features,
    };

    VkDevice result = VK_NULL_HANDLE;
    if (!VK_CHECK(vkCreateDevice(m_physical_device.handle(), &createInfo, nullptr, &result))) {
        throw DeviceCreationError(DeviceCreationError::DeviceCreationFailed);
    }
    volkLoadDevice(result);

    Queues queues;
    for (auto const [family, _] : m_required_queue_families) {
        uint32_t const family_index = m_physical_device.queueFamily(family).value();
        VkQueue tmp = VK_NULL_HANDLE;
        vkGetDeviceQueue(result, family_index, 0, &tmp);
        queues[indexOf(family)] = Queue::make(tmp, family);
    }

    caps.commitDeviceCaps(caps.supportedExtensions(), caps.supportedFeatures());
    return Device(result, queues);
}

} // namespace core
