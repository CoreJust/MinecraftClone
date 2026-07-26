#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/Version.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Extent.hpp>
#include <core/vulkan/Features.hpp>
#include <core/vulkan/Layers.hpp>
#include <core/vulkan/PhysicalDeviceProperties.hpp>
#include <core/vulkan/enum/ColorSpace.hpp>
#include <core/vulkan/enum/Format.hpp>
#include <core/vulkan/enum/MemoryProperty.hpp>
#include <core/vulkan/enum/PhysicalDeviceType.hpp>
#include <core/vulkan/enum/PresentMode.hpp>
#include <core/vulkan/enum/SurfaceTransform.hpp>

#include <vector>

namespace core::vk {

class VulkanCaps : NonCopyable {
public:
    constexpr VulkanCaps() noexcept = default;

    void commitInstanceCaps(
        Version const& instance_version,
        bool const validation_enabled,
        VulkanLayers const& supported_layers,
        VulkanLayers const& enabled_layers,
        VulkanExtensions const& enabled_extensions,
        VulkanExtensions const& supported_extensions
    ) noexcept;

    void commitPhysicalDeviceCaps(
        std::string device_name,
        Version const& device_version,
        PhysicalDeviceProperties const& properties,
        std::vector<MemoryHeap> heaps,
        PhysicalDeviceType const type,
        VulkanExtensions const& supported_extensions,
        VulkanFeatures const& supported_features
    ) noexcept;

    void commitDeviceCaps(
        VulkanExtensions const& enabled_extensions,
        VulkanFeatures const& enabled_features
    ) noexcept;

    void commitSwapchainCaps(
        Format const format,
        ColorSpace const color_space,
        PresentMode const present_mode,
        SurfaceTransformBits const surface_transform,
        Extent2d const extent,
        uint32_t const image_count
    ) noexcept;

    [[nodiscard]]
    constexpr Version instanceVersion() const noexcept { return m_instance_version; }
    [[nodiscard]]
    constexpr Version deviceVersion() const noexcept { return m_device_version; }
    [[nodiscard]]
    constexpr std::string const& deviceName() const noexcept { return m_device_name; }
    [[nodiscard]]
    constexpr PhysicalDeviceProperties const& deviceProperties() const noexcept { return m_device_properties; }
    [[nodiscard]]
    constexpr bool validationEnabled() const noexcept { return m_validation_enabled; }
    [[nodiscard]]
    constexpr VulkanLayers const& enabledLayers() const noexcept { return m_enabled_layers; }
    [[nodiscard]]
    constexpr VulkanExtensions const& enabledExtensions() const noexcept { return m_enabled_extensions; }
    [[nodiscard]]
    constexpr VulkanFeatures const& enabledFeatures() const noexcept { return m_enabled_features; }
    [[nodiscard]]
    constexpr VulkanLayers const& supportedLayers() const noexcept { return m_supported_layers; }
    [[nodiscard]]
    constexpr VulkanExtensions const& supportedExtensions() const noexcept { return m_supported_extensions; }
    [[nodiscard]]
    constexpr VulkanFeatures const& supportedFeatures() const noexcept { return m_supported_features; }
    [[nodiscard]]
    constexpr Format surfaceFormat() const noexcept { return m_format; }
    [[nodiscard]]
    constexpr ColorSpace colorSpace() const noexcept { return m_color_space; }
    [[nodiscard]]
    constexpr PresentMode presentMode() const noexcept { return m_present_mode; }
    [[nodiscard]]
    constexpr SurfaceTransformBits surfaceTransform() const noexcept { return m_surface_transform; }
    [[nodiscard]]
    constexpr Extent2d extent() const noexcept { return m_extent; }
    [[nodiscard]]
    constexpr uint32_t swapchainImageCount() const noexcept { return m_image_count; }

    [[nodiscard]]
    std::vector<VulkanExtension> supportedDeviceExtensionsAsVec() const;

    [[nodiscard]]
    bool has(VulkanLayer const layer) const noexcept {
        return m_enabled_layers.hasLayer(layer);
    }

    [[nodiscard]]
    bool has(VulkanExtension const extension) const noexcept {
        return m_enabled_extensions.hasExtension(extension);
    }

    [[nodiscard]]
    bool has(VulkanFeature const feature) const noexcept {
        return m_enabled_features[feature];
    }

    [[nodiscard]]
    bool hasExtensionOrPromoted(VulkanExtension const extension) const noexcept {
        if (getExtensionKind(extension) == VulkanExtensionKind::Instance) {
            if (instanceVersion() >= getExtensionPromotionVersion(extension)) {
                return true;
            }
        } else {
            return false;
        }
        return m_enabled_extensions.hasExtension(extension);
    }

    [[nodiscard]]
    std::string toString() const;
private:
    VulkanLayers m_supported_layers{ };
    VulkanLayers m_enabled_layers{ };
    VulkanExtensions m_supported_extensions{ };
    VulkanExtensions m_enabled_extensions{ };
    VulkanFeatures m_supported_features{ };
    VulkanFeatures m_enabled_features{ };
    PhysicalDeviceProperties m_device_properties{ };
    std::vector<MemoryHeap> m_heaps{ };
    std::string m_device_name{ };
    Version m_instance_version = core::Version{0, 1, 0, 0};
    Version m_device_version = core::Version{0, 1, 0, 0};
    PhysicalDeviceType m_device_type{ PhysicalDeviceType::Other };
    Format m_format{ Format::None };
    ColorSpace m_color_space{ ColorSpace::Count };
    PresentMode m_present_mode{ PresentMode::Count };
    SurfaceTransformBits m_surface_transform{ };
    Extent2d m_extent{ 0, 0 };
    uint32_t m_image_count{ 0 };
    bool m_validation_enabled{ false };
};

} // namespace core::vk
