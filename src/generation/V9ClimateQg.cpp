#include "V9ClimateQg.hpp"

#include "V9Numerics.hpp"
#include "V9Random.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <stdexcept>
namespace generation::detail
{
namespace
{
using C = std::complex<double>;
struct Solver
{
    int32_t n = 32, N = 2048;
    double dx = 512'000., F;
    explicit Solver(double coupling) : F(coupling)
    {
    }
    void transform(const std::vector<double>& a, std::vector<C>& h)
    {
        v9Transform(a, h, true);
    }
    void inverse(std::vector<C> h, std::vector<double>& a)
    {
        v9Inverse(std::move(h), a, true);
    }
    void invert(const std::vector<double>& q, const double* lap, std::vector<double>& p)
    {
        std::vector<C> h;
        transform(q, h);
        for (int32_t i = 0; i < n * n; ++i)
        {
            C bar = .5 * (h[i] + h[n * n + i]) / lap[i],
              d = .5 * (h[i] - h[n * n + i]) / (lap[i] - 2 * F);
            h[i] = bar + d;
            h[n * n + i] = bar - d;
        }
        inverse(h, p);
    }
    double at(const std::vector<double>& a, int32_t l, int32_t y, int32_t x, bool odd)
    {
        x = (x + n) % n;
        if (y < 0)
            return odd ? -a[l * n * n + x] : 2 * a[l * n * n + x] - a[l * n * n + n + x];
        if (y >= n)
            return odd ? -a[l * n * n + (n - 1) * n + x]
                       : 2 * a[l * n * n + (n - 1) * n + x] - a[l * n * n + (n - 2) * n + x];
        return a[l * n * n + y * n + x];
    }
    void jac(const std::vector<double>& p, const std::vector<double>& q, std::vector<double>& out)
    {
        out.resize(N);
        int32_t stride = n + 2;
        std::vector<double> ph(2 * stride * stride), qh(ph.size());
        for (int32_t l = 0; l < 2; l++)
            for (int32_t y = -1; y <= n; y++)
                for (int32_t x = -1; x <= n; x++)
                {
                    int32_t i = l * stride * stride + (y + 1) * stride + x + 1;
                    ph[i] = at(p, l, y, x, true);
                    qh[i] = at(q, l, y, x, false);
                }
        for (int32_t l = 0; l < 2; l++)
            for (int32_t y = 0; y < n; y++)
                for (int32_t x = 0; x < n; x++)
                {
                    auto P = [&](int32_t yy, int32_t xx)
                    { return ph[l * stride * stride + (yy + 1) * stride + xx + 1]; };
                    auto Q = [&](int32_t yy, int32_t xx)
                    { return qh[l * stride * stride + (yy + 1) * stride + xx + 1]; };
                    double j1 = (P(y, x + 1) - P(y, x - 1)) * (Q(y + 1, x) - Q(y - 1, x)) -
                            (P(y + 1, x) - P(y - 1, x)) * (Q(y, x + 1) - Q(y, x - 1));
                    double j2 = P(y, x + 1) * (Q(y + 1, x + 1) - Q(y - 1, x + 1)) -
                            P(y, x - 1) * (Q(y + 1, x - 1) - Q(y - 1, x - 1)) -
                            P(y + 1, x) * (Q(y + 1, x + 1) - Q(y + 1, x - 1)) +
                            P(y - 1, x) * (Q(y - 1, x + 1) - Q(y - 1, x - 1));
                    double j3 = Q(y + 1, x) * (P(y + 1, x + 1) - P(y + 1, x - 1)) -
                            Q(y - 1, x) * (P(y - 1, x + 1) - P(y - 1, x - 1)) -
                            Q(y, x + 1) * (P(y + 1, x + 1) - P(y - 1, x + 1)) +
                            Q(y, x - 1) * (P(y + 1, x - 1) - P(y - 1, x - 1));
                    out[l * n * n + y * n + x] = (j1 + j2 + j3) / (12 * dx * dx);
                }
    }
    void velocity(const std::vector<double>& p, std::vector<double>& u, std::vector<double>& v)
    {
        u.resize(N);
        v.resize(N);
        for (int32_t l = 0; l < 2; l++)
            for (int32_t y = 0; y < n; y++)
                for (int32_t x = 0; x < n; x++)
                {
                    int32_t i = l * n * n + y * n + x;
                    u[i] = -(at(p, l, y + 1, x, true) - at(p, l, y - 1, x, true)) / (2 * dx);
                    v[i] = (at(p, l, y, x + 1, true) - at(p, l, y, x - 1, true)) / (2 * dx);
                }
    }
    void tendency(const std::vector<double>& q, const double* peq, const double* bg,
                  const double* lap, std::vector<double>& rhs, std::vector<double>& p)
    {
        invert(q, lap, p);
        std::vector<double> total(N), qt(N), damp;
        for (int32_t i = 0; i < N; i++)
        {
            total[i] = peq[i] + p[i];
            qt[i] = q[i] + bg[i];
        }
        jac(total, qt,
            rhs); // The discrete inversion identity gives Laplacian(p) = q - F*(p_other-p).
        damp.resize(N);
        for (int32_t i = 0; i < N; i++)
            damp[i] = q[i] - F * (p[(i + n * n) % N] - p[i]);
        for (int32_t i = 0; i < N; i++)
            rhs[i] = -rhs[i] - q[i] / (40 * 86'400.) - (i >= n * n ? damp[i] / (10 * 86'400.) : 0);
    }
};
constexpr int32_t N = 32, S = N * N;
constexpr double DX = 512'000.;
void check(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error("v9 climate cancelled");
}
// scipy.ndimage gaussian_filter: axis order Y(reflect), then X(wrap), radius round(4 sigma).
std::vector<double> gaussian(std::span<const double> a)
{
    std::vector<double> tmp(a.size()), out(a.size());
    double weights[17], sum = 0.;
    for (int32_t k = -8; k <= 8; ++k)
    {
        weights[k + 8] = std::exp(-.5 * k * k / 4.);
        sum += weights[k + 8];
    }
    sum = v9PairwiseSum(weights);
    for (double& w : weights)
        w /= sum;
    for (int32_t l = 0; l < 2; ++l)
        for (int32_t y = 0; y < N; ++y)
            for (int32_t x = 0; x < N; ++x)
            {
                double t = 0.;
                for (int32_t k = -8; k <= 8; ++k)
                {
                    int32_t yy = y + k;
                    if (yy < 0)
                        yy = -yy - 1;
                    if (yy >= N)
                        yy = 2 * N - yy - 1;
                    t += weights[k + 8] * a[l * S + yy * N + x];
                }
                tmp[l * S + y * N + x] = t;
            }
    for (int32_t l = 0; l < 2; ++l)
        for (int32_t y = 0; y < N; ++y)
            for (int32_t x = 0; x < N; ++x)
            {
                double t = 0.;
                for (int32_t k = -8; k <= 8; ++k)
                    t += weights[k + 8] * tmp[l * S + y * N + (x + k + N) % N];
                out[l * S + y * N + x] = t;
            }
    return out;
}
} // namespace
V9Weather computeV9Weather(std::span<const double> theta, std::span<const double> ground,
                           uint64_t seed, double c2, std::stop_token stop,
                           std::function<void(uint32_t)> progress)
{
    if (theta.size() != 8 * S || ground.size() != S || !(c2 > 0))
        throw std::invalid_argument("v9 weather input shape/mode");
    check(stop);
    double F = .5 * std::pow(7e-5, 2) / c2;
    Solver solver(F);
    std::vector<double> annual(N), f(N), U(2 * N), peq(2 * S), lap(S), background, q;
    for (int32_t y = 0; y < N; ++y)
    {
        std::vector<double> row(N);
        for (int32_t x = 0; x < N; ++x)
        {
            double a = 0.;
            for (int32_t p = 0; p < 8; ++p)
                a += theta[p * S + y * N + x];
            row[x] = a / 8;
        }
        annual[y] = v9PairwiseSum(row) / N;
        f[y] = -1e-4 * std::cos(std::numbers::pi * (y + .5) / N);
    }
    for (int32_t y = 0; y < N; ++y)
    {
        double gradient = (y == 0       ? (annual[1] - annual[0])
                           : y == N - 1 ? (annual[N - 1] - annual[N - 2])
                            : (annual[y + 1] - annual[y - 1]) / 2) /
                          DX;
        double base = -8 * std::cos(4 * std::numbers::pi * (y + .5) / N);
        U[y] = base -
               287 * std::log(1 / .30) * f[y] / (f[y] * f[y] + std::pow(1 / 86'400., 2)) * gradient;
        U[N + y] = base;
    }
    for (int32_t l = 0; l < 2; ++l)
    {
        double mean = v9PairwiseSum(std::span(U).subspan(l * N, N)) / N;
        double total = 0.;
        for (int32_t y = 0; y < N; ++y)
        {
            total -= (U[l * N + y] - mean) * DX;
            peq[l * S + y * N] = total;
        }
        double first = peq[l * S], last = peq[l * S + (N - 1) * N] - first;
        for (int32_t y = 0; y < N; ++y)
        {
            double value = peq[l * S + y * N] - first - last * y / (N - 1);
            for (int32_t x = 0; x < N; ++x)
                peq[l * S + y * N + x] = value;
        }
    }
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
            lap[y * N + x] = -4 *
                             (std::pow(std::sin(std::numbers::pi * (y + 1) / (2 * N)), 2) +
                              std::pow(std::sin(std::numbers::pi * x / N), 2)) /
                             (DX * DX);
    auto qfrom = [&](std::vector<double> const& p)
    {
        std::vector<C> h;
        solver.transform(p, h);
        auto original = h;
        for (int32_t i = 0; i < 2 * S; ++i)
            h[i] = original[i] * lap[i % S] + F * (original[(i + S) % (2 * S)] - original[i]);
        std::vector<double> result;
        solver.inverse(h, result);
        return result;
    };
    background = qfrom(peq);
    for (int32_t y = 0; y < N; ++y)
        for (int32_t x = 0; x < N; ++x)
        {
            background[y * N + x] += f[y];
            background[S + y * N + x] += f[y] + f[y] * ground[y * N + x] / 8000.;
        }
    auto noise = gaussian(v9NormalSamples(seed, 2 * S));
    for (int32_t l = 0; l < 2; ++l)
    {
        double mean = v9PairwiseSum(std::span(noise).subspan(l * S, S)) / S;
        for (int32_t i = 0; i < S; ++i)
            noise[l * S + i] -= mean;
    }
    double mean = v9PairwiseSum(noise) / (2 * S);
    std::vector<double> squares(noise.size());
    for (int32_t i = 0; i < 2 * S; ++i)
        squares[i] = (noise[i] - mean) * (noise[i] - mean);
    double variance = v9PairwiseSum(squares) / (2 * S);
    double scale = .01 * DX / std::max(std::sqrt(variance), 1e-20);
    for (double& x : noise)
        x *= scale;
    q = qfrom(noise);
    V9Weather result;
    result.u_anomaly.reserve(180 * 2 * S);
    result.v_anomaly.reserve(180 * 2 * S);
    std::vector<double> p, u, v, rhs, p2, rhs2, mid(2 * S), total(2 * S);
    double lmin = *std::min_element(lap.begin(), lap.end());
    std::vector<double> damping(2 * S);
    double damping_dt = -1.;
    for (int32_t day = 0; day < 180; ++day)
    {
        check(stop);
        solver.invert(q, lap.data(), p);
        solver.velocity(p, u, v);
        for (int32_t l = 1; l >= 0; --l)
            for (int32_t y = 0; y < N; ++y)
            {
                double mu = v9PairwiseSum(std::span(u).subspan(l * S + y * N, N)) / N,
                       mv = v9PairwiseSum(std::span(v).subspan(l * S + y * N, N)) / N;
                for (int32_t x = 0; x < N; ++x)
                {
                    result.u_anomaly.push_back(u[l * S + y * N + x] - mu);
                    result.v_anomaly.push_back(v[l * S + y * N + x] - mv);
                }
            }
        double t = 0.;
        int32_t steps = 0;
        while (t < 86'400.)
        {
            check(stop);
            if (++steps > 1000)
                throw std::runtime_error("v9 QG daily step bound");
            solver.tendency(q, peq.data(), background.data(), lap.data(), rhs, p);
            for (int32_t i = 0; i < 2 * S; ++i)
                total[i] = peq[i] + p[i];
            solver.velocity(total, u, v);
            double speed = 0.;
            for (int32_t i = 0; i < 2 * S; ++i)
                speed = std::max(speed, std::abs(u[i]) + std::abs(v[i]));
            double dt = std::min({1800., .35 * DX / std::max(speed, 1.), 86'400. - t});
            result.adaptive_dt_s.push_back(dt);
            for (int32_t i = 0; i < 2 * S; ++i)
                mid[i] = q[i] + dt * rhs[i];
            solver.tendency(mid, peq.data(), background.data(), lap.data(), rhs2, p2);
            for (int32_t i = 0; i < 2 * S; ++i)
                q[i] = .5 * q[i] + .5 * (mid[i] + dt * rhs2[i]);
            std::vector<C> h;
            solver.transform(q, h);
            if (dt != damping_dt)
            {
                for (int32_t i = 0; i < 2 * S; ++i)
                {
                    damping[i] = std::exp(-dt * std::pow(lap[i % S] / lmin, 2) / (.5 * 86'400.));
                }
                damping_dt = dt;
            }
            for (int32_t i = 0; i < 2 * S; ++i)
            {
                h[i] *= damping[i];
            }
            solver.inverse(h, q);
            for (double a : q)
                if (!std::isfinite(a) || std::abs(a) > .1)
                    throw std::runtime_error("v9 QG bounded invalid PV");
            t += dt;
        }
        if (progress)
            progress(day + 1);
    }
    result.final_pv = std::move(q);
    return result;
}
} // namespace generation::detail
