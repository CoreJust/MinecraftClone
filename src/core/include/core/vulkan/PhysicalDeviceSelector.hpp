#pragma once

#include <core/common/InputSpan.hpp>
#include <core/common/VectorUtils.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Features.hpp>
#include <core/vulkan/Instance.hpp>
#include <core/vulkan/PhysicalDevice.hpp>
#include <core/vulkan/PhysicalDeviceProperties.hpp>
#include <core/vulkan/Surface.hpp>
#include <core/vulkan/enum/MemoryProperty.hpp>
#include <core/vulkan/enum/PhysicalDeviceType.hpp>

namespace core {

CORE_VK_ERROR_WITH_KINDS(PhysicalDeviceSelectionError,
    NoPhysicalDevices,
    NoSuitableDevice)

class PhysicalDeviceSelector final {
public:
    explicit PhysicalDeviceSelector(Instance const& instance)
        : m_instance(instance)
    { }

    template<typename Self>
    [[nodiscard]] auto&& requireApiVersion(this Self&& self, Version const version) noexcept {
        self.m_required_api_version = version;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireExtensions(
        this Self&& self,
        InputSpan<VulkanExtension> const exts
    ) {
        appendRange(self.m_required_extensions, exts);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferExtensions(
        this Self&& self,
        InputSpan<VulkanExtension> const exts
    ) {
        appendRange(self.m_preferred_extensions, exts);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireFeatures(
        this Self&& self,
        InputSpan<VulkanFeature> const features
    ) {
        appendRange(self.m_required_features, features);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& preferFeatures(
        this Self&& self,
        InputSpan<VulkanFeature> const features
    ) {
        appendRange(self.m_preferred_features, features);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireMemoryHeaps(
        this Self&& self,
        InputSpan<MemoryPropertyBits> const heaps
    ) {
        appendRange(self.m_required_heaps, heaps);
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireQueueFamilies(
        this Self&& self,
        InputSpan<QueueFamily> const families
    ) {
        for (QueueFamily const family : families) {
            ASSERT(family != QueueFamily::Present, "Use requirePresentQueueFamily to request present queue family");
        }
        appendRange(self.m_required_queue_families, families);
        return std::forward<Self>(self);
    }
    
    template<typename Self>
    [[nodiscard]] auto&& requirePresentQueueFamily(
        this Self&& self,
        Surface const& surface
    ) {
        self.m_required_queue_families.push_back(QueueFamily::Present);
        self.m_surface = &surface;
        return std::forward<Self>(self);
    }

    template<typename Self>
    [[nodiscard]] auto&& requireDeviceType(
        this Self&& self,
        InputSpan<PhysicalDeviceType> const types
    ) {
        appendRange(self.m_required_device_types, types);
        return std::forward<Self>(self);
    }

    // By default discrete GPUs are preferred with weight 1024
    template<typename Self>
    [[nodiscard]] auto&& preferDeviceType(
        this Self&& self,
        InputSpan<TrivialPair<PhysicalDeviceType, int32_t>> const types
    ) {
        appendRange(self.m_preferred_device_types, types);
        return std::forward<Self>(self);
    }

    // Throws PhysicalDeviceSelectionError
    [[nodiscard]]
    PhysicalDevice select(VulkanCaps& out_caps) const;
private:
    [[nodiscard]]
    int32_t scoreDevice(PhysicalDevice const& device, Version const instance_version) const;
private:
    Instance const& m_instance;
    Surface const* m_surface = nullptr;
    
    std::vector<VulkanExtension> m_required_extensions;
    std::vector<VulkanExtension> m_preferred_extensions;
    std::vector<VulkanFeature> m_required_features;
    std::vector<VulkanFeature> m_preferred_features;

    std::vector<MemoryPropertyBits> m_required_heaps;
    std::vector<QueueFamily> m_required_queue_families;
    std::vector<TrivialPair<PhysicalDeviceType, int32_t>> m_preferred_device_types;
    std::vector<PhysicalDeviceType> m_required_device_types;

    Version m_required_api_version;
};

} // namespace core
