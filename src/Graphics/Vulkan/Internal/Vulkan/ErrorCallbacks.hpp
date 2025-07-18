#pragma once
#include <Core/Memory/Function.hpp>

namespace graphics::vulkan::internal {
    using Callback = core::Function<bool>;

    void setOutOfDateKHRCallback (Callback callback) noexcept;
    void setSuboptimalKHRCallback(Callback callback) noexcept;
    void setDeviceLostCallback   (Callback callback) noexcept;
    bool onOutOfDateKHR();
    bool onSuboptimalKHR();
    bool onDeviceLost();
} // namespace graphics::vulkan::internal
