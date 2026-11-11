#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace generation::detail
{

[[nodiscard]]
std::array<uint8_t, 32U> sha256Digest(std::span<uint8_t const> bytes);

} // namespace generation::detail
