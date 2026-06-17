#include <core/vulkan/Capabilities.hpp>

#include <core/IO/EnumFmt.hpp>

namespace core {

void VulkanCaps::commitInstanceCaps(
    Version const& instance_version,
    bool const validation_enabled,
    VulkanLayers const& enabled_layers,
    VulkanExtensions const& enabled_extensions
) noexcept {
    m_instance_version = instance_version;
    m_validation_enabled = validation_enabled;
    m_layers = enabled_layers;
    for (VulkanExtension const ext : valuesOf<VulkanExtension>()) {
        if (getExtensionKind(ext) == VulkanExtensionKind::Instance) {
            m_extensions.versionAt(ext) = enabled_extensions.getExtensionVersion(ext);
        }
    }
}

void VulkanCaps::commitPhysicalDeviceCaps(
    Version const& device_version,
    std::vector<MemoryHeap> heaps,
    PhysicalDeviceType const type
) noexcept {
    m_device_version = device_version;
    m_heaps = heaps;
    m_device_type = type;
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
        "Device version: {}\n"
        "Device type: {}\n"
        "Validation: {}\n"
        "Layers:\n{}"
        "Extensions:\n{}"
        "Features:\n{}"
        "Memory heaps:\n{}",
        instanceVersion(),
        deviceVersion(),
        m_device_type,
        validationEnabled() ? "Enabled" : "Disabled",
        m_layers.toString("\t"),
        m_extensions.toString("\t"),
        m_features.toString("\t"),
        heaps_message
    );
}

} // namespace core
