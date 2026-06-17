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

namespace core {

CORE_VK_ERROR_WITH_KINDS(DeviceCreationError,
    PhysicalDeviceHasNoSuchFamily,
    DeviceCreationFailed)

// Extensions and features are taken from Capabilities
class DeviceBuilder final {
public:
    explicit DeviceBuilder(PhysicalDevice const& physical_device)
        : m_physical_device(physical_device)
    { }

    template<typename Self>
    [[nodiscard]] auto&& requireQueueFamilies(
        this Self&& self,
        InputSpan<TrivialPair<QueueFamily, float>> const families
    ) {
        appendRange(self.m_required_queue_families, families);
        return std::forward<Self>(self);
    }

    // Throws DeviceCreationError
    [[nodiscard]]
    Device build(VulkanCaps& caps) const;
private:
    PhysicalDevice const& m_physical_device;

    std::vector<TrivialPair<QueueFamily, float>> m_required_queue_families;
};

} // namespace core
