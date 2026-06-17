#include <core/vulkan/Capabilities.hpp>

#include <core/IO/EnumFmt.hpp>

namespace core {

void VulkanCaps::commitInstanceCaps(
    Version const& instance_version,
    bool const validation_enabled,
    VulkanLayers const& supported_layers,
    VulkanLayers const& enabled_layers,
    VulkanExtensions const& supported_extensions,
    VulkanExtensions const& enabled_extensions
) noexcept {
    m_instance_version = instance_version;
    m_validation_enabled = validation_enabled;
    m_supported_layers = supported_layers;
    m_enabled_layers = enabled_layers;
    for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
        if (getExtensionKind(ext) == VulkanExtensionKind::Instance) {
            m_supported_extensions.versionAt(ext) = supported_extensions.getExtensionVersion(ext);
            m_enabled_extensions.versionAt(ext) = enabled_extensions.getExtensionVersion(ext);
        }
    }
}

void VulkanCaps::commitPhysicalDeviceCaps(
    std::string device_name,
    Version const& device_version,
    PhysicalDeviceProperties const& properties,
    std::vector<MemoryHeap> heaps,
    PhysicalDeviceType const type,
    VulkanExtensions const& supported_extensions,
    VulkanFeatures const& supported_features
) noexcept {
    m_device_name = device_name;
    m_device_version = device_version;
    m_device_properties = properties;
    m_heaps = heaps;
    m_device_type = type;
    m_supported_features = supported_features;
    for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
        if (getExtensionKind(ext) == VulkanExtensionKind::Device) {
            m_supported_extensions.versionAt(ext) = supported_extensions.getExtensionVersion(ext);
        }
    }
}

void VulkanCaps::commitDeviceCaps(
    VulkanExtensions const& enabled_extensions,
    VulkanFeatures const& enabled_features
) noexcept {
    m_enabled_features = enabled_features;
    for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
        if (getExtensionKind(ext) == VulkanExtensionKind::Device) {
            m_enabled_extensions.versionAt(ext) = enabled_extensions.getExtensionVersion(ext);
        }
    }
}

std::vector<VulkanExtension> VulkanCaps::supportedDeviceExtensionsAsVec() const {
    std::vector<VulkanExtension> result{ };
    for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
        if (getExtensionKind(ext) == VulkanExtensionKind::Device && m_supported_extensions.hasExtension(ext)) {
            result.push_back(ext);
        }
    }
    return result;
}

std::string VulkanCaps::toString() const {
    std::string heaps_message, tmp;
    uint32_t index = 0;
    for (MemoryHeap const heap : m_heaps) {
        tmp = "\t" + std::to_string(heap.heap_index) + ": ";
        for (MemoryProperty const property : valuesOf<MemoryProperty>()) {
            if (MemoryPropertyBits::of(property).value & heap.properties.value) {
                tmp += toStringView(property);
                tmp += ", ";
            }
        }
        if (tmp.ends_with(", ")) {
            tmp.pop_back();
            tmp.pop_back();
        }
        heaps_message += tmp;
        heaps_message += '\n';
        ++index;
    }
    return fmt::format(
        "Instance version: {}\n"
        "Device name: {}\n"
        "Device version: {}\n"
        "Device type: {}\n"
        "Validation: {}\n"
        "Layers:\n{}"
        "Extensions:\n{}"
        "Features:\n{}"
        "Memory heaps:\n{}",
        instanceVersion(),
        deviceName(),
        deviceVersion(),
        m_device_type,
        validationEnabled() ? "Enabled" : "Disabled",
        m_enabled_layers.toString("\t"),
        m_enabled_extensions.toString("\t"),
        m_enabled_features.toString("\t"),
        heaps_message
    );
}

} // namespace core
