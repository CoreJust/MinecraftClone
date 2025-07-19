#pragma once
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::pipeline {
    // Public interface for a vulkan pipeline.
    class RenderPipeline final {
        usize m_index;

    public:
        RenderPipeline(usize index) noexcept : m_index(index) { }

        PURE usize& getIndex() noexcept { return m_index; }
    };
} // namespace graphics::vulkan::pipeline
