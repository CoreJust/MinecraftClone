#include "V9Numerics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
namespace generation::detail
{
namespace
{
constexpr int32_t N = 32;
using Complex = std::complex<double>;
void fft(Complex* a, bool inverse)
{
    for (int32_t i = 1, j = 0; i < N; ++i)
    {
        int32_t bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (int32_t len = 2; len <= N; len <<= 1)
    {
        Complex root = std::polar(1., (inverse ? 2. : -2.) * std::numbers::pi / len);
        for (int32_t i = 0; i < N; i += len)
        {
            Complex w = 1.;
            for (int32_t j = 0; j < len / 2; ++j)
            {
                Complex u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= root;
            }
        }
    }
    for (int32_t i = 0; i < N; ++i)
        a[i] /= std::sqrt(double(N));
}
double basis(int32_t k, int32_t j, bool sine)
{
    static auto const matrices = []
    {
        std::array<std::array<double, N * N>, 2> result{};
        for (int32_t k = 0; k < N; ++k)
            for (int32_t j = 0; j < N; ++j)
            {
                result[1][k * N + j] = std::sqrt(2. / N) *
                            std::sin(std::numbers::pi * (j + .5) * (k + 1) / N) *
                            (k == N - 1 ? 1 / std::sqrt(2.) : 1.);
                result[0][k * N + j] = std::sqrt(2. / N) *
                            std::cos(std::numbers::pi * (j + .5) * k / N) *
                            (k == 0 ? 1 / std::sqrt(2.) : 1.);
            }
        return result;
    }();
    return matrices[sine ? 1 : 0][k * N + j];
}
} // namespace
double v9PairwiseSum(std::span<const double> values)
{
    if (values.size() < 8)
    {
        double sum = -0.;
        for (double a : values)
            sum += a;
        return sum;
    }
    if (values.size() <= 128)
    {
        std::array<double, 8> sums;
        std::copy_n(values.begin(), 8, sums.begin());
        uint64_t i = 8;
        for (; i < values.size() - values.size() % 8; i += 8)
            for (uint64_t k = 0; k < 8; ++k)
                sums[k] += values[i + k];
        double sum = ((sums[0] + sums[1]) + (sums[2] + sums[3])) +
                     ((sums[4] + sums[5]) + (sums[6] + sums[7]));
        for (; i < values.size(); ++i)
            sum += values[i];
        return sum;
    }
    uint64_t split = values.size() / 2 - (values.size() / 2) % 8;
    return v9PairwiseSum(values.first(split)) + v9PairwiseSum(values.subspan(split));
}
void v9Transform(std::span<const double> a, std::vector<Complex>& h, bool sine)
{
    if (a.size() % (N * N) != 0)
        throw std::invalid_argument("v9 transform shape");
    h.assign(a.size(), {});
    for (uint64_t l = 0; l < a.size(); l += N * N)
    {
        for (int32_t k = 0; k < N; ++k)
        {
            std::array<double, N> sums{};
            for (int32_t j = 0; j < N; ++j)
            {
                double const weight = basis(k, j, sine);
                for (int32_t x = 0; x < N; ++x)
                    sums[static_cast<size_t>(x)] += weight * a[l + j * N + x];
            }
            for (int32_t x = 0; x < N; ++x)
                h[l + k * N + x] = sums[static_cast<size_t>(x)];
        }
        for (int32_t y = 0; y < N; ++y)
            fft(h.data() + l + y * N, false);
    }
}
void v9Inverse(std::vector<Complex> h, std::vector<double>& a, bool sine)
{
    if (h.size() % (N * N) != 0)
        throw std::invalid_argument("v9 inverse shape");
    a.resize(h.size());
    for (uint64_t l = 0; l < h.size(); l += N * N)
    {
        for (int32_t y = 0; y < N; ++y)
            fft(h.data() + l + y * N, true);
        for (int32_t j = 0; j < N; ++j)
        {
            std::array<double, N> sums{};
            for (int32_t k = 0; k < N; ++k)
            {
                double const weight = basis(k, j, sine);
                for (int32_t x = 0; x < N; ++x)
                    sums[static_cast<size_t>(x)] += weight * h[l + k * N + x].real();
            }
            for (int32_t x = 0; x < N; ++x)
                a[l + j * N + x] = sums[static_cast<size_t>(x)];
        }
    }
}
void projectV9Flux(std::span<const double> fx, std::span<const double> fy, double dx,
                   std::vector<double>& ox, std::vector<double>& oy)
{
    if (fx.size() != N * N || fy.size() != (N + 1) * N || !std::isfinite(dx) || dx <= 0)
        throw std::invalid_argument("v9 face shape/spacing");
    std::vector<double> div(N * N), p;
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            int32_t i = y * N + x;
            double upper = y == N - 1 ? 0 : fy[i + N], lower = y == 0 ? 0 : fy[i];
            div[i] = (fx[i] - fx[y * N + (x + N - 1) % N] + upper - lower) / dx;
        }
    std::vector<Complex> h;
    v9Transform(div, h, false);
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            double lap = -4 *
                         (std::pow(std::sin(std::numbers::pi * x / N), 2) +
                          std::pow(std::sin(std::numbers::pi * y / (2 * N)), 2)) /
                         (dx * dx);
            h[y * N + x] /= (x == 0 && y == 0 ? 1. : lap);
        }
    v9Inverse(std::move(h), p, false);
    ox.assign(fx.begin(), fx.end());
    oy.assign(fy.begin(), fy.end());
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            int32_t i = y * N + x;
            ox[i] -= (p[y * N + (x + 1) % N] - p[i]) / dx;
            if (y > 0)
                oy[i] -= (p[i] - p[i - N]) / dx;
            else
                oy[i] = 0.;
            oy[N * N + x] = 0.;
        }
}
} // namespace generation::detail
