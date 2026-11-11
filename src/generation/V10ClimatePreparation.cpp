#include "V10ClimatePreparation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace generation::detail
{
namespace
{

V9ClimateField const& field(V9ClimateResult const& climate, char const* name,
    std::vector<uint32_t> const& shape)
{
    auto const found = climate.fields.find(name);
    if (found == climate.fields.end() || found->second.shape != shape)
    {
        throw std::runtime_error("V10 fresh climate field has an unexpected shape");
    }
    return found->second;
}

std::vector<double> precipitation(V9ClimateResult const& climate)
{
    auto const& rain = field(climate, "rain_kg_m2_s", {8U, 32U, 32U}).values;
    std::vector<double> result(8U * 256U * 256U);
    for (size_t p = 0U; p < 8U; ++p)
    {
        for (int32_t y = 0; y < 256; ++y)
        {
            double const sy = static_cast<double>(y) / 8.0 - .5;
            auto const iy = static_cast<int32_t>(std::floor(sy));
            double const v = sy - iy;
            for (int32_t x = 0; x < 256; ++x)
            {
                double const sx = static_cast<double>(x) / 8.0 - .5;
                auto const ix = static_cast<int32_t>(std::floor(sx));
                double const u = sx - ix;
                auto const at = [&](int32_t xx, int32_t yy) {
                    return rain[(p * 32U + static_cast<size_t>((yy + 32) % 32)) * 32U
                        + static_cast<size_t>((xx + 32) % 32)];
                };
                // scipy.ndimage order=1 register, then the source's explicit
                // water-field float32 storage before fine climate consumes it.
                double const registered = (1.0 - v) * ((1.0 - u) * at(ix, iy) + u * at(ix + 1, iy))
                    + v * ((1.0 - u) * at(ix, iy + 1) + u * at(ix + 1, iy + 1));
                result[(p * 256U + static_cast<size_t>(y)) * 256U + static_cast<size_t>(x)]
                    = static_cast<float>(registered * (360.0 * 86'400.0));
            }
        }
    }
    return result;
}

} // namespace

std::expected<V10ClimatePreparation, V9ClimateFailure> prepareV10Climate(
    std::span<double const> canonical_l0_m, uint64_t seed,
    std::stop_token cancellation, V9ClimateProgressCallback progress
)
{
    if (seed != 42U || canonical_l0_m.size() != 256U * 256U
        || std::ranges::any_of(canonical_l0_m, [](double value) {
            return !std::isfinite(value) || !std::isfinite(10.0 * value);
        }))
    {
        return std::unexpected(V9ClimateFailure::InvalidInput);
    }
    if (cancellation.stop_requested())
    {
        return std::unexpected(V9ClimateFailure::Cancelled);
    }
    std::vector<double> equivalent(canonical_l0_m.size());
    std::ranges::transform(canonical_l0_m, equivalent.begin(), [](double value) { return 10.0 * value; });
    auto climate = generateV9Climate(equivalent, seed, cancellation, std::move(progress));
    if (!climate)
    {
        return std::unexpected(climate.error());
    }
    if (cancellation.stop_requested())
    {
        return std::unexpected(V9ClimateFailure::Cancelled);
    }
    try
    {
        V10ClimateFields fields{
            .local_effective_celsius = field(*climate, "local_effective_C", {8U, 32U, 32U}).values,
            .ground_m = field(*climate, "ground_m", {32U, 32U}).values,
            .theta_celsius = field(*climate, "theta_C", {8U, 32U, 32U}).values,
            .precipitation_mm_year = precipitation(*climate),
        };
        return V10ClimatePreparation{std::move(*climate), V10ClimateForcing(std::move(fields))};
    }
    catch (std::exception const&)
    {
        return std::unexpected(V9ClimateFailure::NumericalFailure);
    }
}

} // namespace generation::detail
