#pragma once
#include <Core/Memory/Function.hpp>

namespace graphics::vulkan::internal {
    using Callback = core::Function<bool>;

    void setOutOfDateKHRCallback(Callback callback) noexcept;
    bool onOutOfDateKHR();
} // namespace graphics::vulkan::internal
