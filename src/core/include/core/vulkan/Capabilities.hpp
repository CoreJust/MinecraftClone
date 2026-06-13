#pragma once

#include <core/common/NonCopyable.hpp>
#include <core/common/Version.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Layers.hpp>

#include <fmt/core.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core {

class VulkanCaps final : NonCopyable {
public:
    [[nodiscard]]
    static Version instanceVersion() noexcept { return instance().m_instance_version; }
    [[nodiscard]]
    static Version deviceVersion() noexcept { return instance().m_device_version; }
    [[nodiscard]]
    static bool validationEnabled() noexcept { return instance().m_validation_enabled; }
    [[nodiscard]]
    static uint64_t generation() noexcept { return instance().m_generation; }
    [[nodiscard]]
    static VulkanLayers layers() noexcept { return instance().m_layers; }
    [[nodiscard]]
    static VulkanExtensions extensions() noexcept { return instance().m_extensions; }

    [[nodiscard]]
    static bool hasLayer(VulkanLayer const layer) noexcept {
        return instance().m_layers.hasLayer(layer);
    }

    [[nodiscard]]
    static bool hasExtension(VulkanExtension const extension) noexcept {
        return instance().m_extensions.hasExtension(extension);
    }

    [[nodiscard]]
    static bool hasExtensionOrPromoted(VulkanExtension const extension) noexcept {
        if (getExtensionKind(extension) == VulkanExtensionKind::Instance) {
            if (instanceVersion() >= getExtensionPromotionVersion(extension)) {
                return true;
            }
        } else {
            if (instanceVersion() >= getExtensionPromotionVersion(extension)) {
                return true;
            }
        }
        return instance().m_extensions.hasExtension(extension);
    }

    [[nodiscard]]
    InstanceBuilder makeInstanceBuilder();

    void update(
        Version const& instance_version,
        Version const& device_version,
        bool const validation_enabled,
        VulkanLayers const& enabled_layers,
        VulkanExtensions const& enabled_extensions
    ) noexcept {
        m_instance_version = instance_version;
        m_device_version = device_version;
        m_validation_enabled = validation_enabled;
        m_layers = enabled_layers;
        m_extensions = enabled_extensions;
    }

    [[nodiscard]]
    static std::string toString() {
        return fmt::format(
            "Instance version: {}\n"
            "Device version: {}\n"
            "Validation: {}\n"
            "Layers:\n{}"
            "Extensions:\n{}",
            instanceVersion(),
            deviceVersion(),
            validationEnabled() ? "Enabled" : "Disabled",
            layers().toString("\t"),
            extensions().toString("\t")
        );
    }
private:
    VulkanCaps() = default;

    [[nodiscard]]
    static VulkanCaps& instance() noexcept {
        static VulkanCaps s_instance{ };
        return s_instance;
    }

    Version m_instance_version = core::Version{0, 1, 0, 0};
    Version m_device_version = core::Version{0, 1, 0, 0};
    bool m_validation_enabled = false;
    VulkanLayers m_layers{ };
    VulkanExtensions m_extensions{ };
    uint64_t m_generation = 0;
};

} // namespace core
