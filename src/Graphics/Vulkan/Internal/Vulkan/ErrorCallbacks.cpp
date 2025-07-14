#include "ErrorCallbacks.hpp"
#include <Core/Memory/Move.hpp>

namespace graphics::vulkan::internal {
namespace {
    Callback g_onOutOfDateKHR = nullptr;
} // namespace

    void setOutOfDateKHRCallback(Callback callback) noexcept {
        g_onOutOfDateKHR = core::memory::move(callback);
    }

    bool onOutOfDateKHR() {
        if (g_onOutOfDateKHR)
            return g_onOutOfDateKHR();
        return false;
    }
} // namespace graphics::vulkan::internal
