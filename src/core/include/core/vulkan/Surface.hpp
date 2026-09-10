#pragma once

#include <core/vulkan/Instance.hpp>
#include <core/vulkan/SurfaceProvider.hpp>

namespace core::vk {

class RawSurface : public VulkanResourceBase<VkSurfaceKHR> {
    CORE_VK_RESOURCE_CONTEXT(RawSurface,
        RawInstance instance{ };
    )
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(Instance const& instance, SurfaceProvider const& provider);
};

using Surface = VulkanRaii<RawSurface>;

} // namespace core::vk
