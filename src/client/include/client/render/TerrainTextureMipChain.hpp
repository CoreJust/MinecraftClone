#pragma once

#include <client/render/TerrainTextures.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace client::render::terrain_textures {

inline constexpr uint32_t MIP_LEVEL_COUNT = 5U;

struct MipLevel final {
    uint32_t extent = 0U;
    std::vector<uint8_t> rgba8;
};

using MipChain = std::array<MipLevel, MIP_LEVEL_COUNT>;

[[nodiscard]] MipChain makeMipChain(std::span<uint32_t const> source);

} // namespace client::render::terrain_textures
