#pragma once
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan {
    // Public interface for a vulkan pipeline.
    class RenderPipeline final {
        size_t m_index;

    public:
        RenderPipeline(size_t index) noexcept : m_index(index) { }

        PURE size_t& getIndex() noexcept { return m_index; }
    };
} // namespace graphics::vulkan
