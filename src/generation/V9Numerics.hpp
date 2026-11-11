#pragma once
#include <complex>
#include <cstdint>
#include <span>
#include <vector>
namespace generation::detail
{
double v9PairwiseSum(std::span<const double> values);
// Orthonormal periodic-X, cell-centred closed-Y transforms at the native 32² grid.
void v9Transform(std::span<const double> values, std::vector<std::complex<double>>& output,
                 bool sine);
void v9Inverse(std::vector<std::complex<double>> values, std::vector<double>& output, bool sine);
void projectV9Flux(std::span<const double> fx, std::span<const double> fy, double dx,
                   std::vector<double>& out_x, std::vector<double>& out_y);
} // namespace generation::detail
