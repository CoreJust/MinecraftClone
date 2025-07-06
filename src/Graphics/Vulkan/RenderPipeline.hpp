#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Memory/UniquePtr.hpp>

namespace graphics::vulkan {
    namespace internal {
        class Pipeline;
    }

    // Public interface for a vulkan pipeline.
    class RenderPipeline final {
        core::memory::UniquePtr<internal::Pipeline> m_impl;

    public:
        RenderPipeline(core::memory::UniquePtr<internal::Pipeline> impl);

        PURE internal::Pipeline      & getImpl()       noexcept { return *m_impl; }
        PURE internal::Pipeline const& getImpl() const noexcept { return *m_impl; }
    };
} // namespace graphics::vulkan
