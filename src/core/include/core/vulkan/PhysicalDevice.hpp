#pragma once

#include <core/vulkan/QueueFamily.hpp>
#include <core/vulkan/Resource.hpp>

namespace core {

// Note that RAII makes no sense for physical device since it is not created, but selected
class PhysicalDevice final : public VulkanResourceBase<VkPhysicalDevice> {
    CORE_VK_RESOURCE_CONTEXT(PhysicalDevice);
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(
        VkPhysicalDevice const physical_device,
        QueueFamilies const& families
    ) {
        self.m_handle = physical_device;
        self.m_families = families;
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT(){ };
    }
public:
    [[nodiscard]]
    constexpr QueueFamilies const& queueFamilies() const noexcept { return m_families; }
    [[nodiscard]]
    constexpr std::optional<uint32_t> queueFamily(QueueFamily const family) const noexcept {
        uint32_t const result = m_families[family];
        return result == QueueFamilies::NO_INDEX
            ? std::nullopt
            : std::optional(result);
    }
private:
    QueueFamilies m_families{ };
};

} // namespace core
