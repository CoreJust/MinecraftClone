#pragma once

#include <core/common/InputSpan.hpp>
#include <core/common/TrivialPair.hpp>
#include <core/common/VectorUtils.hpp>
#include <core/vulkan/Capabilities.hpp>
#include <core/vulkan/Device.hpp>
#include <core/vulkan/Error.hpp>
#include <core/vulkan/Extensions.hpp>
#include <core/vulkan/Features.hpp>
#include <core/vulkan/PhysicalDevice.hpp>

CORE_VK_ERROR_WITH_KINDS(DeviceCreationError, VulkanInitializationError,
    PhysicalDeviceHasNoSuchFamily,
    DeviceCreationFailed)

namespace core::vk {

// Extensions and features are taken from Capabilities
class DeviceBuilder final {
public:
    [[nodiscard]]
    std::span<TrivialPair<QueueFamily, float> const> requiredQueueFamilies() const noexcept {
        return m_required_queue_families;
    }
public:
    template<typename Self>
    auto&& requireQueueFamilies(
        this Self&& self,
        InputSpan<TrivialPair<QueueFamily, float>> const families
    ) {
        self.m_required_queue_families.assign(families.begin(), families.end());
        return std::forward<Self>(self);
    }

    // Throws DeviceCreationError
    [[nodiscard]]
    Device build(VulkanCaps& caps, PhysicalDevice const& physical_device) const;
private:
    std::vector<TrivialPair<QueueFamily, float>> m_required_queue_families;
};

} // namespace core::vk
