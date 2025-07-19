#pragma once
#include <Core/Macro/Attributes.hpp>

namespace graphics::vulkan::pipeline {
    // Public interface for a vulkan pipeline.
    class RenderPipeline {
        usize m_index;

    public:
        RenderPipeline(usize index) noexcept : m_index(index) { }

        PURE usize& getIndex() noexcept { return m_index; }
    };

    template<typename T>
    concept PipelineConcept = requires (T& pipeline) {
        typename T::Vertex;
        T::Options;
        pipeline.getIndex();
    };
} // namespace graphics::vulkan::pipeline
