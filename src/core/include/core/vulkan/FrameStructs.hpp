#pragma once

namespace core::vk {

struct RelativeViewport final { float x = 0.f, y = 0.f, w = 1.f, h = 1.f, depth_min = 0.f, depth_max = 1.f; };
struct RelativeScissor final { float x = 0.f, y = 0.f, w = 1.f, h = 1.f; };

} // namespace core::vk
