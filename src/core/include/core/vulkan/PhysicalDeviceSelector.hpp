#pragma once

#include <core/meta/Enum.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Features.hpp>
#include <core/vulkan/Instance.hpp>
#include <core/vulkan/MemoryProperty.hpp>
#include <core/vulkan/PhysicalDevice.hpp>
#include <core/vulkan/PhysicalDeviceProperties.hpp>
#include <core/vulkan/PhysicalDeviceType.hpp>
#include <core/vulkan/Surface.hpp>

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

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireExtensions(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_required_extensions.insert(
            self.m_required_extensions.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& preferExtensions(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_preferred_extensions.insert(
            self.m_preferred_extensions.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireFeatures(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_required_features.insert(
            self.m_required_features.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& preferFeatures(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_preferred_features.insert(
            self.m_preferred_features.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireMemoryHeaps(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_required_heaps.insert(
            self.m_required_heaps.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireQueueFamilies(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        for (QueueFamily family : span_over_data.asSpan()) {
            ASSERT(family != QueueFamily::Present, "Use requirePresentQueueFamily to request present queue family");
        }
        self.m_required_queue_families.insert(
            self.m_required_queue_families.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
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

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& preferDeviceType(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_preferred_device_types.insert(
            self.m_preferred_device_types.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    template<typename Self, typename First, typename... Tail>
    [[nodiscard]] auto&& requireDeviceType(
        this Self&& self,
        First const first,
        Tail const... rest
    ) {
        SpanOver span_over_data{ first, rest... };
        self.m_required_device_types.insert(
            self.m_required_device_types.end(),
            span_over_data.asSpan().begin(),
            span_over_data.asSpan().end()
        );
        return std::forward<Self>(self);
    }

    // Throws PhysicalDeviceSelectionError
    [[nodiscard]]
    PhysicalDevice select(VulkanCaps* const out_caps = nullptr) const;
private:
    [[nodiscard]]
    int32_t scoreDevice(PhysicalDevice const& device) const;
private:
    Instance const& m_instance;
    Surface const* m_surface;
    
    std::vector<VulkanExtension> m_required_extensions;
    std::vector<VulkanExtension> m_preferred_extensions;
    std::vector<VulkanFeature> m_required_features;
    std::vector<VulkanFeature> m_preferred_features;

    std::vector<MemoryPropertyBits> m_required_heaps;
    std::vector<QueueFamily> m_required_queue_families;
    std::vector<PhysicalDeviceType> m_preferred_device_types;
    std::vector<PhysicalDeviceType> m_required_device_types;

    Version m_required_api_version;
};

} // namespace core
