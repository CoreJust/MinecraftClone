#pragma once

#include <core/vulkan/Instance.hpp>
#include <core/window/Window.hpp>

#include <cstdint>
#include <utility>

namespace core {

class RawSurface : public VulkanResourceBase<VkSurfaceKHR> {
    VKC_RESOURCE_CONTEXT(RawSurface,
        RawInstance instance{ };
    )
    VKC_RESOURCE_DEFER_CONSTRUCTION_FROM(Instance const& instance, Window const& window);
};

using Surface = VulkanRaii<RawSurface>;

} // namespace core
