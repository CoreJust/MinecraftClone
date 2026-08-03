#pragma once

#include <functional>

namespace core::vk {

/*
 * Error-recovery callbacks invoked by VK_CHECK when a Vulkan function returns
 * a recoverable result code (OUT_OF_DATE_KHR, SURFACE_LOST_KHR, etc.).
 *
 * These are global singletons because VK_CHECK is used throughout the codebase
 * without access to a VulkanContext instance, so there is no way to route the
 * callback to a specific context. Only one VulkanContext should coexist at a
 * time; a second context's constructor would overwrite the first's callbacks.
 */
using Callback = std::function<bool()>;

void setOutOfDateKHRCallback     (Callback callback) noexcept;
void setSuboptimalKHRCallback    (Callback callback) noexcept;
void setDeviceLostCallback       (Callback callback) noexcept;
void setSurfaceLostCallback      (Callback callback) noexcept;
void setFullscreenExclusiveModeLostCallback(Callback callback) noexcept;
void setOutOfHostMemoryCallback  (Callback callback) noexcept;
void setOutOfDeviceMemoryCallback(Callback callback) noexcept;

bool onOutOfDateKHR();
bool onSuboptimalKHR();
bool onDeviceLost();
bool onSurfaceLost();
bool onFullscreenExclusiveModeLost();
bool onOutOfHostMemory();
bool onOutOfDeviceMemory();

} // namespace core::vk
