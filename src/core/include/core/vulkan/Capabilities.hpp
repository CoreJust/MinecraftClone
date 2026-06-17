#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/Version.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Features.hpp>
#include <core/vulkan/Layers.hpp>
#include <core/vulkan/MemoryProperty.hpp>
#include <core/vulkan/PhysicalDeviceProperties.hpp>
#include <core/vulkan/PhysicalDeviceType.hpp>

namespace core {

class VulkanCaps final : NonCopyable {
public:
    constexpr VulkanCaps() noexcept = default;

    void commitInstanceCaps(
        Version const& instance_version,
        bool const validation_enabled,
        VulkanLayers const& enabled_layers,
        VulkanExtensions const& enabled_extensions
    ) noexcept;

    void commitPhysicalDeviceCaps(
        Version const& device_version,
        std::vector<MemoryHeap> heaps,
        PhysicalDeviceType const type
    ) noexcept;

    [[nodiscard]]
    constexpr Version instanceVersion() const noexcept { return m_instance_version; }
    [[nodiscard]]
    constexpr Version deviceVersion() const noexcept { return m_device_version; }
    [[nodiscard]]
    constexpr bool validationEnabled() const noexcept { return m_validation_enabled; }
    [[nodiscard]]
    constexpr VulkanLayers const& layers() const noexcept { return m_layers; }
    [[nodiscard]]
    constexpr VulkanExtensions const& extensions() const noexcept { return m_extensions; }
    [[nodiscard]]
    constexpr VulkanFeatures const& features() const noexcept { return m_features; }

    [[nodiscard]]
    bool has(VulkanLayer const layer) const noexcept {
        return m_layers.hasLayer(layer);
    }

    [[nodiscard]]
    bool has(VulkanExtension const extension) const noexcept {
        return m_extensions.hasExtension(extension);
    }

    [[nodiscard]]
    bool has(VulkanFeature const feature) const noexcept {
        return m_features[feature];
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
        return m_extensions.hasExtension(extension);
    }

    [[nodiscard]]
    std::string toString() const;
private:
    VulkanLayers m_layers{ };
    VulkanExtensions m_extensions{ };
    VulkanFeatures m_features{ };
    PhysicalDeviceProperties m_device_properties{ };
    std::vector<MemoryHeap> m_heaps{ };
    Version m_instance_version = core::Version{0, 1, 0, 0};
    Version m_device_version = core::Version{0, 1, 0, 0};
    PhysicalDeviceType m_device_type{ PhysicalDeviceType::Other };
    bool m_validation_enabled{ false };
};

} // namespace core
