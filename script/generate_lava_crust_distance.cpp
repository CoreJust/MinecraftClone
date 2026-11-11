// Offline generator for the periodic creation-history crust distance table.
// Build with c++ -std=c++23 -O3, then redirect stdout to the .inc file.
// The v1 field is fixed at 256x256. Keep the checked-in table authoritative
// across toolchains, where trigonometric library rounding can differ.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <random>
#include <vector>

namespace
{
constexpr uint32_t GENERATION_MAP_ANIMATION_RESOLUTION = 256U;
constexpr double DISTANCE_CAP = 3.07;
constexpr uint32_t MAX_DISTANCE_CODE = 65'535U;

// Versioned copy of the periodic grain used to bake the v1 crust geometry.
float nativeEase(float value)
{
    float const amount = std::clamp(value, 0.0F, 1.0F);
    return amount * amount * (3.0F - 2.0F * amount);
}
float nativeRockGrain(float x, float y, bool fine = false, float warp = 1.0F)
{
    auto hash = [](uint32_t x, uint32_t y) {
        uint32_t h = x * 0x9e37'79b9U ^ y * 0x85eb'ca6bU ^ 0x68bc'21ebU;
        h = (h ^ (h >> 16U)) * 0x7feb'352dU;
        h = (h ^ (h >> 15U)) * 0x846c'a68bU;
        h ^= h >> 16U;
        return static_cast<float>(h & 65'535U) / 65'535.0F;
    };
    constexpr float SIDE = static_cast<float>(GENERATION_MAP_ANIMATION_RESOLUTION);
    x = std::fmod(x, SIDE); y = std::fmod(y, SIDE);
    if (x < 0.0F)
    {
        x += SIDE;
    }
    if (y < 0.0F)
    {
        y += SIDE;
    }
    // Integer wave directions keep the domain periodic while breaking the lattice axes.
    float const angle = 2.0F * std::numbers::pi_v<float> / SIDE;
    float warped_x = x + 7.0F * std::sin((2.0F * x + 3.0F * y) * angle)
        + 5.0F * std::sin((5.0F * x - 2.0F * y) * angle);
    float warped_y = y + 7.0F * std::sin((3.0F * x - 2.0F * y) * angle)
        + 5.0F * std::cos((2.0F * x + 5.0F * y) * angle);
    if (warp != 1.0F)
    {
        warped_x = x + warp * (warped_x - x);
        warped_y = y + warp * (warped_y - y);
    }
    x = std::fmod(warped_x + SIDE, SIDE);
    y = std::fmod(warped_y + SIDE, SIDE);
    auto noise = [&](uint32_t period) {
        float const xx = x * static_cast<float>(period) / SIDE;
        float const yy = y * static_cast<float>(period) / SIDE;
        uint32_t const ix = static_cast<uint32_t>(xx), iy = static_cast<uint32_t>(yy);
        float const fx = nativeEase(xx - ix), fy = nativeEase(yy - iy);
        auto tile = [&](uint32_t xx, uint32_t yy) { return hash(xx % period, yy % period); };
        return std::lerp(std::lerp(tile(ix, iy), tile(ix + 1U, iy), fx),
            std::lerp(tile(ix, iy + 1U), tile(ix + 1U, iy + 1U), fx), fy);
    };
    return noise(fine ? 43U : 11U) * .55F + noise(fine ? 79U : 29U) * .30F
        + noise(fine ? 113U : 61U) * .15F;
}
struct CrustNucleus
{
    float x;
    float y;
    float direction_cosine;
    float direction_sine;
    float radius;
    float stretch;
};

std::vector<float> makeNativeCrustDistance()
{
    constexpr uint32_t SIDE = GENERATION_MAP_ANIMATION_RESOLUTION;
    constexpr std::size_t CELL_COUNT = static_cast<std::size_t>(SIDE) * SIDE;
    constexpr float side = static_cast<float>(SIDE);
    std::mt19937 random(0x363C11U);
    auto unit = [&] { return static_cast<float>(random() >> 8U) / 16'777'216.0F; };
    std::vector<CrustNucleus> nuclei;
    nuclei.reserve(149U);
    for (std::size_t attempt = 0U; attempt < 4'096U && nuclei.size() < 149U; ++attempt)
    {
        float const x = unit() * side;
        float const y = unit() * side;
        if (std::any_of(nuclei.begin(), nuclei.end(), [&](CrustNucleus const& other) {
                return std::hypot(std::remainder(x - other.x, side),
                    std::remainder(y - other.y, side)) < 10.5F;
            }))
        {
            continue;
        }
        float const angle = unit() * 2.0F * std::numbers::pi_v<float>;
        nuclei.push_back({x, y, std::cos(angle), std::sin(angle),
            8.0F + 7.0F * unit(), 0.48F + 0.42F * unit()});
    }

    std::vector<float> distance(CELL_COUNT, 30.0F);
    for (uint32_t y = 0; y < SIDE; ++y)
    {
        for (uint32_t x = 0; x < SIDE; ++x)
        {
            float const fx = static_cast<float>(x);
            float const fy = static_cast<float>(y);
            // Continuous toroidal warp bends crust contacts without exposing a lattice.
            float const wx = fx + (nativeRockGrain(fx, fy, false, 0.4F) - 0.5F) * 19.0F;
            float const wy = fy + (nativeRockGrain(fx + 83.0F, fy + 47.0F, false, 0.4F)
                - 0.5F) * 19.0F;
            auto& nearest = distance[static_cast<std::size_t>(y) * SIDE + x];
            for (CrustNucleus const& nucleus : nuclei)
            {
                float const dx = std::remainder(wx - nucleus.x, side);
                float const dy = std::remainder(wy - nucleus.y, side);
                if (std::abs(dx) > 54.0F || std::abs(dy) > 54.0F)
                {
                    continue;
                }
                float const parallel = dx * nucleus.direction_cosine + dy * nucleus.direction_sine;
                float const across = (dy * nucleus.direction_cosine - dx * nucleus.direction_sine) / nucleus.stretch;
                nearest = std::min(nearest,
                    std::hypot(parallel, across) / nucleus.radius);
            }
        }
    }
    return distance;
}

}

int main()
{
    auto const distances = makeNativeCrustDistance();
    for (std::size_t i = 0U; i < distances.size(); ++i)
    {
        double const value = std::min(static_cast<double>(distances[i]), DISTANCE_CAP);
        std::cout << std::lround(value / DISTANCE_CAP * MAX_DISTANCE_CODE) << ',';
        std::cout << ((i + 1U) % 16U == 0U ? '\n' : ' ');
    }
    return std::cout ? 0 : 1;
}
