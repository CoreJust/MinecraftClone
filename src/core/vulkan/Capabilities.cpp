#include <core/vulkan/Capabilities.hpp>

#include <core/IO/EnumBitsFmt.hpp>
#include <core/IO/EnumFmt.hpp>
#include <core/IO/JoinContainerFmt.hpp>

namespace core::vk {

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

void VulkanCaps::commitSwapchainCaps(
    Format const format,
    ColorSpace const color_space,
    PresentMode const present_mode,
    SurfaceTransformBits const surface_transform,
    Extent2d const extent,
    uint32_t const image_count
) noexcept {
    m_format = format;
    m_color_space = color_space;
    m_present_mode = present_mode;
    m_surface_transform = surface_transform;
    m_extent = extent;
    m_image_count = image_count;
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
    return fmt::format(
        "\tInstance version: {}\n"
        "\tDevice name: {}\n"
        "\tDevice version: {}\n"
        "\tDevice type: {}\n"
        "\tValidation: {}\n"
        "\tSurface format: {}\n"
        "\tColor space: {}\n"
        "\tPresent mode: {}\n"
        "\tSurface transform: {}\n"
        "\tExtent: {}x{}\n"
        "\tSwapchain image count: {}\n"
        "\tLayers:\n{}"
        "\tExtensions:\n{}"
        "\tFeatures:\n{}"
        "\tMemory heaps:\n{}",
        instanceVersion(),
        deviceName(),
        deviceVersion(),
        m_device_type,
        validationEnabled() ? "Enabled" : "Disabled",
        m_format,
        m_color_space,
        m_present_mode,
        joinFmt(m_surface_transform),
        m_extent.x, m_extent.y,
        m_image_count,
        m_enabled_layers.toString("\t\t"),
        m_enabled_extensions.toString("\t\t"),
        m_enabled_features.toString("\t\t"),
        joinFmt<"", "", "">(m_heaps, [](fmt::context::iterator out, MemoryHeap const& heap) {
            return fmt::format_to(out, "\t\t{}: {}\n", heap.heap_index, joinFmt(heap.properties));
        })
    );
}

} // namespace core::vk
