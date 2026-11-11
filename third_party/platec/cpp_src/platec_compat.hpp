#pragma once

#include <bit>
#include <cstdint>
#include <cmath>

namespace platec_compat
{

template <typename Float>
[[nodiscard]]
inline Float fusedMultiplyAdd(Float left, Float right, Float addend) noexcept
{
    return std::fma(left, right, addend);
}

[[nodiscard]]
constexpr std::int64_t wrappedSignedSquareRemainder256(std::uint32_t value) noexcept
{
    std::uint64_t const product_bits = static_cast<std::uint64_t>(value) * value;
    return std::bit_cast<std::int64_t>(product_bits) % 256;
}

} // namespace platec_compat
