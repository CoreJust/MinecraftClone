#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <vector>
namespace generation::detail
{
struct V9Weather final
{
    std::vector<double> u_anomaly; // [180, lower/upper, 32, 32], m/s
    std::vector<double> v_anomaly;
    std::vector<double> final_pv; // [upper/lower, 32, 32], s^-1
    std::vector<double> adaptive_dt_s;
};
[[nodiscard]]
V9Weather computeV9Weather(std::span<const double> theta, std::span<const double> ground,
                           uint64_t seed, double dry_wave_speed_squared, std::stop_token stop,
                           std::function<void(uint32_t)> progress);
} // namespace generation::detail
