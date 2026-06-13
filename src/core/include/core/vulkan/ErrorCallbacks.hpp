#pragma once

#include <functional>

namespace core {

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

} // namespace core
