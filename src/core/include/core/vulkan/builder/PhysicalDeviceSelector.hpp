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
    NoSurfaceProvidedDespitePresentRequested,
    NoPhysicalDevices,
    NoSuitableDevice)

class PhysicalDeviceSelector final {
public:
    template<typename Self>
    auto&& requireApiVersion(this Self&& self, Version const version) noexcept {
        self.m_required_api_version = version;
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& requireExtensions(
        this Self&& self,
        InputSpan<VulkanExtension> const exts
    ) {
        appendRange(self.m_required_extensions, exts);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& preferExtensions(
        this Self&& self,
        InputSpan<VulkanExtension> const exts
    ) {
        appendRange(self.m_preferred_extensions, exts);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& requireFeatures(
        this Self&& self,
        InputSpan<VulkanFeature> const features
    ) {
        appendRange(self.m_required_features, features);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& preferFeatures(
        this Self&& self,
        InputSpan<VulkanFeature> const features
    ) {
        appendRange(self.m_preferred_features, features);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& requireMemoryHeaps(
        this Self&& self,
        InputSpan<MemoryPropertyBits> const heaps
    ) {
        appendRange(self.m_required_heaps, heaps);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& requireQueueFamilies(
        this Self&& self,
        InputSpan<QueueFamily> const families
    ) {
        appendRange(self.m_required_queue_families, families);
        return std::forward<Self>(self);
    }

    template<typename Self>
    auto&& requireDeviceType(
        this Self&& self,
        InputSpan<PhysicalDeviceType> const types
    ) {
        appendRange(self.m_required_device_types, types);
        return std::forward<Self>(self);
    }

    // By default discrete GPUs are preferred with weight 1024
    template<typename Self>
    auto&& preferDeviceType(
        this Self&& self,
        InputSpan<TrivialPair<PhysicalDeviceType, int32_t>> const types
    ) {
        appendRange(self.m_preferred_device_types, types);
        return std::forward<Self>(self);
    }

    // Throws PhysicalDeviceSelectionError
    [[nodiscard]]
    PhysicalDevice select(VulkanCaps& out_caps, Instance const& instance, Surface const* surface = nullptr) const;
private:
    [[nodiscard]]
    int32_t scoreDevice(PhysicalDevice const& device, Version const instance_version) const;
private:
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
