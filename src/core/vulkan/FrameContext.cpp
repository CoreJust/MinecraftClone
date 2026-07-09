#include <core/vulkan/FrameContext.hpp>

#include <core/vulkan/Context.hpp>

namespace core::vk {

FrameContext::~FrameContext() {
    m_p_ctx.endFrame();
}

} // namespace core::vk
