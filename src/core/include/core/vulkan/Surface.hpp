#pragma once

#include <core/vulkan/Instance.hpp>
#include <core/window/Window.hpp>

namespace core::vk {

class RawSurface : public VulkanResourceBase<VkSurfaceKHR> {
    CORE_VK_RESOURCE_CONTEXT(RawSurface,
        RawInstance instance{ };
    )
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(Instance const& instance, Window const& window);
};

using Surface = VulkanRaii<RawSurface>;

} // namespace core::vk
