#pragma once
#include <Core/Macro/Attributes.hpp>
#include "../Wrapper/Handles.hpp"

namespace graphics::vulkan::internal {
    class Vulkan;

    class RenderPass final {
        VkRenderPass m_pass = VK_NULL_HANDLE;
        Vulkan&      m_vulkan;

    public:
        RenderPass(Vulkan& vulkan);
        ~RenderPass();

        PURE VkRenderPass get() const noexcept { return m_pass; }
    };
} // namespace graphics::vulkan::internal
