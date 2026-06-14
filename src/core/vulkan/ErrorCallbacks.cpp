#include <core/vulkan/ErrorCallbacks.hpp>

namespace core {
namespace {

Callback g_on_out_of_date_khr                = nullptr;
Callback g_on_suboptimal_khr                 = nullptr;
Callback g_on_device_lost                    = nullptr;
Callback g_on_surface_lost                   = nullptr;
Callback g_on_fullscreen_exclusive_mode_lost = nullptr;
Callback g_on_out_of_host_memory             = nullptr;
Callback g_on_out_of_device_memory           = nullptr;

} // namespace

void setOutOfDateKHRCallback     (Callback callback) noexcept { g_on_out_of_date_khr      = std::move(callback); }
void setSuboptimalKHRCallback    (Callback callback) noexcept { g_on_suboptimal_khr       = std::move(callback); }
void setDeviceLostCallback       (Callback callback) noexcept { g_on_device_lost          = std::move(callback); }
void setSurfaceLostCallback      (Callback callback) noexcept { g_on_surface_lost         = std::move(callback); }
void setFullscreenExclusiveModeLostCallback(Callback callback) noexcept {
    g_on_fullscreen_exclusive_mode_lost = std::move(callback);
}
void setOutOfHostMemoryCallback  (Callback callback) noexcept { g_on_out_of_host_memory   = std::move(callback); }
void setOutOfDeviceMemoryCallback(Callback callback) noexcept { g_on_out_of_device_memory = std::move(callback); }

bool onOutOfDateKHR     () { return g_on_out_of_date_khr      ? g_on_out_of_date_khr ()     : false; }
bool onSuboptimalKHR    () { return g_on_suboptimal_khr       ? g_on_suboptimal_khr()       : false; }
bool onDeviceLost       () { return g_on_device_lost          ? g_on_device_lost   ()       : false; }
bool onSurfaceLost      () { return g_on_surface_lost         ? g_on_surface_lost  ()       : false; }
bool onFullscreenExclusiveModeLost() { return g_on_fullscreen_exclusive_mode_lost ? g_on_fullscreen_exclusive_mode_lost() : false; }
bool onOutOfHostMemory  () { return g_on_out_of_host_memory   ? g_on_out_of_host_memory()   : false; }
bool onOutOfDeviceMemory() { return g_on_out_of_device_memory ? g_on_out_of_device_memory() : false; }

} // namespace core
