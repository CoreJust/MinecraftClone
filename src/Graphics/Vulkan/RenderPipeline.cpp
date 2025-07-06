#include "RenderPipeline.hpp"
#include <Core/IO/Logger.hpp>
#include "Internal/Pipeline.hpp"

namespace graphics::vulkan {
    RenderPipeline::RenderPipeline(core::memory::UniquePtr<internal::Pipeline> impl)
        : m_impl(std::move(impl))
    { }
} // namespace graphics::vulkan
