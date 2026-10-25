#pragma once

#include <cstdint>
#include <vector>

namespace generation::detail
{

// NumPy default_rng(seed).normal samples in flattened C array order.
[[nodiscard]] std::vector<double> v9NormalSamples(uint64_t seed, uint64_t count);

} // namespace generation::detail
