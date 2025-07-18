#include "ErrorCallbacks.hpp"
#include <Core/Memory/Move.hpp>

namespace graphics::vulkan::internal {
namespace {
    Callback g_onOutOfDateKHR  = nullptr;
    Callback g_onSuboptimalKHR = nullptr;
    Callback g_onDeviceLost    = nullptr;
} // namespace

    void setOutOfDateKHRCallback (Callback callback) noexcept { g_onOutOfDateKHR  = core::move(callback); }
    void setSuboptimalKHRCallback(Callback callback) noexcept { g_onSuboptimalKHR = core::move(callback); }
    void setDeviceLostCallback   (Callback callback) noexcept { g_onDeviceLost    = core::move(callback); }

    bool onOutOfDateKHR () { return g_onOutOfDateKHR  ? g_onOutOfDateKHR () : false; }
    bool onSuboptimalKHR() { return g_onSuboptimalKHR ? g_onSuboptimalKHR() : false; }
    bool onDeviceLost   () { return g_onDeviceLost    ? g_onDeviceLost   () : false; }
} // namespace graphics::vulkan::internal
