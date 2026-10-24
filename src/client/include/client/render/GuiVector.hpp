#pragma once

#include <array>
#include <cstdint>

namespace client {

// Framebuffer pixels from the top-left; straight-alpha RGBA8, interpolated per vertex.
struct GuiVectorVertex final {
    std::array<float, 2> position{};
    std::array<uint8_t, 4> rgba{};
};
static_assert(sizeof(GuiVectorVertex) == 12U);

inline constexpr uint32_t MAXIMUM_GUI_VECTOR_VERTICES = 65'536U;

} // namespace client
