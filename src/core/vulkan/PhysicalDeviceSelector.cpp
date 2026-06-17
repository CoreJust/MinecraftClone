#include <core/vulkan/PhysicalDeviceSelector.hpp>

#include <core/meta/EnumImpl.hpp>
#include <core/vulkan/Check.hpp>
#include <core/vulkan/PhysicalDeviceCapsStruct.hpp>

namespace core {

CORE_ENUM_FUNCTIONS_IMPL(PhysicalDeviceSelectionErrorKind);

PhysicalDevice PhysicalDeviceSelector::select(VulkanCaps* const out_caps) const {
    uint32_t physical_device_count = 0;
    if (!VK_CHECK(vkEnumeratePhysicalDevices(m_instance.handle(), &physical_device_count, nullptr))
        || physical_device_count == 0
    ) {
        throw PhysicalDeviceSelectionError(PhysicalDeviceSelectionError::NoPhysicalDevices);
    }

    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (!VK_CHECK(vkEnumeratePhysicalDevices(m_instance.handle(), &physical_device_count, physical_devices.data()))) {
        throw PhysicalDeviceSelectionError(PhysicalDeviceSelectionError::NoPhysicalDevices);
    }

    PhysicalDevice best_device{ };
    int32_t best_score = std::numeric_limits<int32_t>::min();
    VkSurfaceKHR const surface_handle = m_surface
        ? m_surface->handle()
        : VK_NULL_HANDLE;

    for (VkPhysicalDevice const physical_device : physical_devices) {
        PhysicalDevice const device = PhysicalDevice::make(
            physical_device,
            QueueFamilies::query(physical_device, surface_handle));
        int32_t const score = scoreDevice(device);
        if (score >= 0 && score > best_score) {
            best_score = score;
            best_device = device;
        }
    }

    if (best_device.isNull()) {
        throw PhysicalDeviceSelectionError(PhysicalDeviceSelectionError::NoSuitableDevice);
    }

    if (out_caps) {
        auto caps = internal::PhysicalDeviceCapsStruct::query(best_device);
        out_caps->commitPhysicalDeviceCaps(
            caps.apiVersion(),
            caps.memoryHeaps(),
            caps.deviceType()
        );
    }

    return best_device;
}

int32_t PhysicalDeviceSelector::scoreDevice(PhysicalDevice const& device) const {
    // Eligibility check
    for (QueueFamily const family : m_required_queue_families) {
        if (!device.queueFamily(family).has_value()) {
            CORE_DEBUG("PhysicalDevice rejected: queue family {} not found", family);
            return -1;
        }
    }
    
    auto caps = internal::PhysicalDeviceCapsStruct::query(device);
    if (caps.apiVersion() < m_required_api_version) {
        CORE_DEBUG(
            "PhysicalDevice {} rejected: required version {} < found {}",
            caps.deviceName(), m_required_api_version, caps.apiVersion());
        return -1;
    }

    if (!m_required_device_types.empty()
        && std::ranges::find(m_required_device_types, caps.deviceType()) == m_required_device_types.end()
    ) {
        CORE_DEBUG(
            "PhysicalDevice {} rejected: it's type {} is not in required",
            caps.deviceName(), caps.deviceType());
        return -1;
    }

    for (MemoryPropertyBits const bits : m_required_heaps) {
        if (!caps.hasHeapWith(bits)) {
            CORE_DEBUG("PhysicalDevice {} rejected: required heap not found", caps.deviceName());
            return -1;
        }
    }

    for (VulkanFeature const feature : m_required_features) {
        if (!caps.hasFeature(feature)) {
            CORE_DEBUG("PhysicalDevice {} rejected: required feature {} not found", caps.deviceName(), feature);
            return -1;
        }
    }

    auto extensions = VulkanExtensions::loadSupportedDeviceExtensions(device);
    for (VulkanExtension const ext : m_required_extensions) {
        if (!extensions.hasExtension(ext)) {
            CORE_DEBUG("PhysicalDevice {} rejected: required extension {} not found", caps.deviceName(), getFullExtensionName(ext));
            return -1;
        }
    }

    // Evaluation of eligible device
    int32_t score = 0;
    if (std::ranges::find(m_preferred_device_types, caps.deviceType()) == m_preferred_device_types.end()) {
        score += 1024;
    }

    for (VulkanFeature const feature : m_preferred_features) {
        if (caps.hasFeature(feature)) {
            score += 1;
        }
    }

    for (VulkanExtension const ext : m_preferred_extensions) {
        if (extensions.hasExtension(ext)) {
            score += 1;
        }
    }
    CORE_DEBUG("Eligible physical device {} gained score {}", caps.deviceName(), score);
    return score;
}

} // namespace core
