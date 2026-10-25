#pragma once
#include <functional>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
namespace generation::detail
{
struct V9ClimateThermal final
{
    std::map<std::string, std::vector<double>> fields;
};
double v9DryWaveSpeedSquared();
double v9DryStaticJump();
V9ClimateThermal computeV9ClimateThermal(std::span<const double> bed,
                            std::stop_token cancellation = {},
                            std::function<void(char const *)> const &progress = {});
} // namespace generation::detail
