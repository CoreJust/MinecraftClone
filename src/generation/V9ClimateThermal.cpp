#include "V9ClimateThermal.hpp"

#include "V9ClimateSparseSolver.hpp"
#include "V9Numerics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <map>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace generation::detail
{
namespace
{
constexpr int32_t N = 32, C = N * N, P = 8;
constexpr double DX = 16'384'000. / N, DT = 360. * 86'400. / P, PI = std::numbers::pi_v<double>;
using Vec = std::vector<double>;
using Complex = std::complex<double>;
struct Mode
{
    std::array<double, 2> weights, chi, pressure, statics;
    double capacity, jump, speed2;
};
Mode mode()
{
    Mode m{};
    double bounds[3] = {1., std::sqrt(.30), .30};
    double k = 287. * .0065 / 9.81, mean = 0.;
    for (int32_t l = 0; l < 2; ++l)
    {
        double b = bounds[l], t = bounds[l + 1];
        m.weights[l] = (b - t) / .70;
        m.chi[l] = (std::pow(b, k + 1) - std::pow(t, k + 1)) / ((k + 1) * (b - t));
        mean += m.weights[l] * m.chi[l];
    }
    for (int32_t l = 0; l < 2; ++l)
    {
        m.pressure[l] = 287. / k * (mean - m.chi[l]);
        m.statics[l] = 1004. * m.chi[l] + 287. / k * (1 - m.chi[l]);
        m.capacity += m.weights[l] * m.statics[l];
    }
    m.jump = 280. * (m.statics[1] - m.statics[0]);
    m.speed2 = -m.pressure[0] * m.weights[0] * m.jump / m.capacity;
    return m;
}
int32_t east(int32_t c) { return c / N * N + (c % N + 1) % N; }
int32_t west(int32_t c) { return c / N * N + (c % N + N - 1) % N; }
double dot(Vec const &a, Vec const &b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.);
}
double norm(Vec const &a) { return std::sqrt(dot(a, a)); }
void check(std::stop_token cancellation)
{
    if (cancellation.stop_requested())
        throw std::runtime_error("V9 thermal cancelled");
}
struct Matrix
{
    std::vector<std::map<int32_t, double>> rows;
    explicit Matrix(int32_t n) : rows(n) {}
    void add(int32_t i, int32_t j, double v) { rows[i][j] += v; }
    Vec apply(Vec const &x) const
    {
        Vec y(rows.size());
        for (uint64_t i = 0; i < rows.size(); ++i)
            for (auto const &[j, v] : rows[i])
                y[i] += v * x[j];
        return y;
    }
};
Vec directSolve(Matrix const &a, Vec const &rhs, std::stop_token cancellation)
{
    int32_t const n = static_cast<int32_t>(rhs.size());
    V9ClimateSparseLu factorization{a.rows, cancellation};
    Vec x = factorization.solve(rhs, cancellation);
    for (int32_t iteration = 0; iteration < 3; ++iteration)
    {
        Vec residual = a.apply(x);
        for (int32_t i = 0; i < n; ++i)
            residual[i] = rhs[i] - residual[i];
        if (norm(residual) <= std::max(1e-11 * norm(rhs), 1e-15))
            return x;
        Vec correction = factorization.solve(residual, cancellation);
        for (int32_t i = 0; i < n; ++i)
            x[i] += correction[i];
    }
    throw std::runtime_error("V9 direct ocean physical residual failed");
}
// Restarted, twice-orthogonalized GMRES with explicit residual validation.
Vec solve(Matrix const &a, Vec const &b, double relative, Vec x, std::stop_token cancellation)
{
    check(cancellation);
    int32_t n = static_cast<int32_t>(b.size()), restart = 100;
    if (x.empty())
        x.resize(n);
    bool saddle = false;
    for (int32_t i = 0; i < n; ++i)
    {
        auto diagonal = a.rows[i].find(i);
        if (diagonal == a.rows[i].end() || std::abs(diagonal->second) < 1e-30)
        {
            saddle = true;
            break;
        }
    }
    if (saddle)
        return directSolve(a, b, cancellation);
    auto factor = a.rows;
    for (int32_t i = 0; i < n; ++i)
        factor[i].try_emplace(i, 0.);
    for (int32_t i = 0; i < n; ++i)
    {
        check(cancellation);
        for (auto entry = factor[i].begin(); entry != factor[i].end() && entry->first < i; ++entry)
        {
            int32_t j = entry->first;
            double pivot = factor[j].at(j);
            if (std::abs(pivot) < 1e-30)
                throw std::runtime_error("V9 ILU zero pivot");
            entry->second /= pivot;
            double multiplier = entry->second;
            for (auto upper = factor[j].upper_bound(j); upper != factor[j].end(); ++upper)
            {
                auto target = factor[i].find(upper->first);
                if (target != factor[i].end())
                    target->second -= multiplier * upper->second;
            }
        }
    }
    auto precondition = [&](Vec z)
    {
        for (int32_t i = 0; i < n; ++i)
            for (auto const &[j, v] : factor[i])
            {
                if (j >= i)
                    break;
                z[i] -= v * z[j];
            }
        for (int32_t i = n - 1; i >= 0; --i)
        {
            for (auto it = factor[i].upper_bound(i); it != factor[i].end(); ++it)
                z[i] -= it->second * z[it->first];
            z[i] /= factor[i].at(i);
        }
        return z;
    };
    auto action = [&](Vec const &v) { return precondition(a.apply(v)); };
    Vec rhs = precondition(b);
    double tolerance = relative * std::max(norm(rhs), 1e-20);
    for (int32_t cycle = 0; cycle < 100; ++cycle)
    {
        check(cancellation);
        Vec r = action(x);
        for (int32_t i = 0; i < n; ++i)
            r[i] = rhs[i] - r[i];
        double beta = norm(r);
        if (beta <= tolerance)
        {
            Vec actual = a.apply(x);
            for (int32_t i = 0; i < n; ++i)
                actual[i] -= b[i];
            double physical = norm(actual);
            if (!std::isfinite(physical))
                throw std::runtime_error("V9 linear solve nonfinite physical residual");
            if (physical > std::max(100 * relative * norm(b), 1e-13))
            {
                tolerance *= .1;
                if (tolerance < 1e-20)
                    throw std::runtime_error("V9 linear solve physical residual stagnation");
                continue;
            }
            return x;
        }
        std::vector<Vec> v(restart + 1, Vec(n));
        for (int32_t i = 0; i < n; ++i)
            v[0][i] = r[i] / beta;
        std::vector<Vec> h(restart + 1, Vec(restart));
        Vec cs(restart), sn(restart), g(restart + 1);
        g[0] = beta;
        int32_t count = 0;
        for (int32_t j = 0; j < restart; ++j)
        {
            check(cancellation);
            Vec w = action(v[j]);
            for (int32_t pass = 0; pass < 2; ++pass)
                for (int32_t k = 0; k <= j; ++k)
                {
                    double q = dot(w, v[k]);
                    h[k][j] += q;
                    for (int32_t i = 0; i < n; ++i)
                        w[i] -= q * v[k][i];
                }
            h[j + 1][j] = norm(w);
            if (h[j + 1][j] > 1e-30)
                for (int32_t i = 0; i < n; ++i)
                    v[j + 1][i] = w[i] / h[j + 1][j];
            for (int32_t k = 0; k < j; ++k)
            {
                double q = cs[k] * h[k][j] + sn[k] * h[k + 1][j];
                h[k + 1][j] = -sn[k] * h[k][j] + cs[k] * h[k + 1][j];
                h[k][j] = q;
            }
            double q = std::hypot(h[j][j], h[j + 1][j]);
            if (q == 0.)
                throw std::runtime_error("V9 GMRES singular Krylov system");
            cs[j] = h[j][j] / q;
            sn[j] = h[j + 1][j] / q;
            h[j][j] = q;
            h[j + 1][j] = 0.;
            g[j + 1] = -sn[j] * g[j];
            g[j] *= cs[j];
            count = j + 1;
            if (std::abs(g[j + 1]) <= tolerance)
                break;
        }
        Vec y(count);
        for (int32_t j = count - 1; j >= 0; --j)
        {
            double q = g[j];
            for (int32_t k = j + 1; k < count; ++k)
                q -= h[j][k] * y[k];
            y[j] = q / h[j][j];
        }
        for (int32_t j = 0; j < count; ++j)
            for (int32_t i = 0; i < n; ++i)
                x[i] += v[j][i] * y[j];
    }
    throw std::runtime_error("V9 portable GMRES did not converge: dimension " + std::to_string(n));
}
Vec thermalReduced(Vec const &rhs, Vec const &capacity)
{
    Vec answer(P * C);
    double k = 2.4e13 / (DX * DX);
    for (int32_t phase = 0; phase < P; ++phase)
    {
        Complex lam = (1. - std::exp(Complex(0., -2 * PI * phase / P))) / DT;
        std::vector<Complex> f(C), d(C), z(N), lower(N), diag(N), upper(N);
        for (int32_t c = 0; c < C; ++c)
        {
            for (int32_t t = 0; t < P; ++t)
                f[c] += rhs[t * C + c] * std::exp(Complex(0., -2 * PI * phase * t / P));
            d[c] = 2.09 + lam * capacity[c];
        }
        for (int32_t j = 0; j < N; ++j)
        {
            Complex inv = 0.;
            for (int32_t i = 0; i < N; ++i)
            {
                z[j] += f[j * N + i] / d[j * N + i] / double(N);
                inv += 1. / d[j * N + i] / double(N);
            }
            double degree = (j == 0 || j == N - 1) ? 1. : 2.;
            diag[j] = 1. + inv * k * degree;
            lower[j] = -inv * k;
            upper[j] = -inv * k;
        }
        for (int32_t j = 1; j < N; ++j)
        {
            Complex q = lower[j] / diag[j - 1];
            diag[j] -= q * upper[j - 1];
            z[j] -= q * z[j - 1];
        }
        z[N - 1] /= diag[N - 1];
        for (int32_t j = N - 2; j >= 0; --j)
            z[j] = (z[j] - upper[j] * z[j + 1]) / diag[j];
        for (int32_t j = 0; j < N; ++j)
        {
            Complex force = k * ((j == 0 || j == N - 1 ? 1. : 2.) * z[j] -
                            (j ? z[j - 1] : Complex{}) - (j + 1 < N ? z[j + 1] : Complex{}));
            for (int32_t i = 0; i < N; ++i)
            {
                int32_t c = j * N + i;
                Complex value = (f[c] - force) / d[c];
                for (int32_t t = 0; t < P; ++t)
                    answer[t * C + c] +=
                        (value * std::exp(Complex(0., 2 * PI * phase * t / P))).real() / P;
            }
        }
    }
    return answer;
}
std::pair<Vec, Vec> gradient(Vec const &a)
{
    Vec x(C), y(C);
    for (int32_t c = 0; c < C; ++c)
    {
        int32_t j = c / N;
        x[c] = (a[east(c)] - a[west(c)]) / (2 * DX);
        y[c] = j == 0       ? (a[c + N] - a[c]) / DX
               : j == N - 1 ? (a[c] - a[c - N]) / DX
                            : (a[c + N] - a[c - N]) / (2 * DX);
    }
    return {x, y};
}
struct Wind
{
    Vec u, v, base, terrain_u, terrain_v, forcing, response;
};
Wind wind(Vec const &theta, Vec const &z, std::stop_token cancellation)
{
    Mode m = mode();
    double r = 1. / 86'400.;
    Vec f(N), base(N), aa(N), bb(N);
    for (int32_t j = 0; j < N; ++j)
    {
        double y = (j + .5) / N;
        f[j] = -1e-4 * std::cos(PI * y);
        base[j] = -8 * std::cos(4 * PI * y);
        aa[j] = r / (r * r + f[j] * f[j]);
        bb[j] = f[j] / (r * r + f[j] * f[j]);
    }
    Matrix gx(C), gy(C), op(C);
    for (int32_t c = 0; c < C; ++c)
    {
        gx.add(c, east(c), 1 / (2 * DX));
        gx.add(c, west(c), -1 / (2 * DX));
        int32_t j = c / N;
        if (j == 0)
        {
            gy.add(c, c, -1 / (2 * DX));
            gy.add(c, c + N, 1 / (2 * DX));
        }
        else if (j == N - 1)
        {
            gy.add(c, c - N, -1 / (2 * DX));
            gy.add(c, c, 1 / (2 * DX));
        }
        else
        {
            gy.add(c, c - N, -1 / (2 * DX));
            gy.add(c, c + N, 1 / (2 * DX));
        }
        op.add(c, c, r);
    }
    for (int32_t c = 0; c < C; ++c)
    {
        double a = aa[c / N] * m.speed2, b = bb[c / N] * m.speed2;
        for (auto const &[i, vi] : gx.rows[c])
        {
            for (auto const &[j, vj] : gx.rows[c])
                op.add(i, j, a * vi * vj);
            for (auto const &[j, vj] : gy.rows[c])
                op.add(i, j, b * vi * vj);
        }
        for (auto const &[i, vi] : gy.rows[c])
        {
            for (auto const &[j, vj] : gx.rows[c])
                op.add(i, j, -b * vi * vj);
            for (auto const &[j, vj] : gy.rows[c])
                op.add(i, j, a * vi * vj);
        }
    }
    std::vector<Complex> spectral(N * (N / 2 + 1));
    for (int32_t k = 0; k <= N / 2; ++k)
    {
        double kval = 2 * PI * k / (N * DX);
        std::vector<Complex> a(N), b(N), c(N), rhs(N);
        for (int32_t j = 0; j < N; ++j)
        {
            Complex topo = 0.;
            for (int32_t i = 0; i < N; ++i)
                topo += f[j] * z[j * N + i] / 8000. * std::exp(Complex(0., -2 * PI * k * i / N));
            Complex d = r + Complex(0., kval * base[j]);
            double beta = j == 0       ? (f[1] - f[0]) / DX
                          : j == N - 1 ? (f[N - 1] - f[N - 2]) / DX
                            : (f[j + 1] - f[j - 1]) / (2 * DX);
            a[j] = d / (DX * DX);
            c[j] = a[j];
            b[j] = d * (-2 / (DX * DX) - kval * kval) + Complex(0., kval * beta);
            if (j == 0 || j == N - 1)
                b[j] -= d / (DX * DX);
            rhs[j] = -base[j] * Complex(0., kval) * topo;
        }
        for (int32_t j = 1; j < N; ++j)
        {
            Complex q = a[j] / b[j - 1];
            b[j] -= q * c[j - 1];
            rhs[j] -= q * rhs[j - 1];
        }
        rhs[N - 1] /= b[N - 1];
        for (int32_t j = N - 2; j >= 0; --j)
            rhs[j] = (rhs[j] - c[j] * rhs[j + 1]) / b[j];
        for (int32_t j = 0; j < N; ++j)
            spectral[j * (N / 2 + 1) + k] = rhs[j];
    }
    Vec psi(C);
    for (int32_t j = 0; j < N; ++j)
        for (int32_t i = 0; i < N; ++i)
            for (int32_t k = 0; k <= N / 2; ++k)
            {
                double weight = (k == 0 || k == N / 2) ? 1. : 2.;
                psi[j * N + i] +=
                    weight *
                    (spectral[j * (N / 2 + 1) + k] * std::exp(Complex(0., 2 * PI * k * i / N)))
                        .real() /
                    N;
            }
    auto [px, py] = gradient(psi);
    Wind out;
    out.base.resize(C);
    out.terrain_u.resize(C);
    out.terrain_v = px;
    out.u.resize(P * C);
    out.v.resize(P * C);
    out.forcing.resize(P * C);
    out.response.resize(P * C);
    for (int32_t c = 0; c < C; ++c)
    {
        out.base[c] = base[c / N];
        out.terrain_u[c] = -py[c];
    }
    for (int32_t t = 0; t < P; ++t)
    {
        Vec rhs(C);
        for (int32_t j = 0; j < N; ++j)
        {
            double mean = 0.;
            for (int32_t i = 0; i < N; ++i)
                mean += theta[t * C + j * N + i] / N;
            for (int32_t i = 0; i < N; ++i)
            {
                int32_t c = j * N + i;
                out.forcing[t * C + c] = m.pressure[0] * (theta[t * C + c] - mean);
                rhs[c] = r * out.forcing[t * C + c];
            }
        }
        Vec p = solve(op, rhs, 1e-11, {}, cancellation), x = gx.apply(p), y = gy.apply(p);
        for (int32_t c = 0; c < C; ++c)
        {
            int32_t j = c / N;
            out.response[t * C + c] = p[c];
            out.u[t * C + c] = -aa[j] * x[c] - bb[j] * y[c] + base[j] - py[c];
            out.v[t * C + c] = (j == 0 || j == N - 1) ? 0. : bb[j] * x[c] - aa[j] * y[c] + px[c];
        }
    }
    return out;
}
struct Ocean
{
    Vec u, v, east_volume, north_volume;
};
Ocean ocean(Wind const &w, Vec const &wet, std::stop_token cancellation,
            std::function<void(char const *)> const &progress)
{
    struct Face
    {
        int32_t p, q;
        double h, fraction;
        bool north;
    };
    std::vector<Face> faces;
    std::array<int32_t, C> ei, ni;
    ei.fill(-1);
    ni.fill(-1);
    for (int32_t c = 0; c < C; ++c)
    {
        int32_t q = east(c);
        if (wet[c] > 0 && wet[q] > 0)
        {
            ei[c] = static_cast<int32_t>(faces.size());
            double fraction = std::min(wet[c], wet[q]);
            faces.push_back({c, q, 20 * fraction, fraction, false});
        }
    }
    for (int32_t c = 0; c < C - N; ++c)
    {
        int32_t q = c + N;
        if (wet[c] > 0 && wet[q] > 0)
        {
            ni[c] = static_cast<int32_t>(faces.size());
            double fraction = std::min(wet[c], wet[q]);
            faces.push_back({c, q, 20 * fraction, fraction, true});
        }
    }
    int32_t count = static_cast<int32_t>(faces.size());
    Ocean out{Vec(P * C), Vec(P * C), Vec(P * C), Vec(P * C)};
    if (!count)
        return out;
    std::array<int32_t, C> component;
    component.fill(-1);
    int32_t components = 0;
    std::vector<std::vector<int32_t>> adjacent(C);
    for (auto const &f : faces)
    {
        adjacent[f.p].push_back(f.q);
        adjacent[f.q].push_back(f.p);
    }
    std::array<int32_t, C> pressure;
    pressure.fill(-1);
    int32_t pc = 0;
    for (int32_t c = 0; c < C; ++c)
        if (wet[c] > 0 && component[c] < 0)
        {
            std::vector<int32_t> queue{c};
            component[c] = components;
            for (uint64_t k = 0; k < queue.size(); ++k)
                for (int32_t q : adjacent[queue[k]])
                    if (component[q] < 0)
                    {
                        component[q] = components;
                        queue.push_back(q);
                    }
            ++components;
            for (int32_t q : queue)
                if (q != c)
                    pressure[q] = pc++;
        }
    Matrix linear(count + pc);
    constexpr double PSCALE = .0002 / 20., CSCALE = .0002 / 20.;
    for (int32_t k = 0; k < count; ++k)
    {
        auto const &f = faces[k];
        linear.add(k, k, .0002 * f.fraction);
        for (auto [cell, sign] : {std::pair{f.p, 1.}, std::pair{f.q, -1.}})
            if (pressure[cell] >= 0)
            {
                int32_t p = count + pressure[cell];
                linear.add(k, p, -sign * f.h * PSCALE);
                linear.add(p, k, sign * f.h * CSCALE);
            }
    }
    for (int32_t c = 0; c < C; ++c)
        if (ei[c] >= 0)
        {
            int32_t j = c / N, i = c % N;
            for (auto [dj, di] :
                 {std::pair{0, 0}, std::pair{0, 1}, std::pair{-1, 0}, std::pair{-1, 1}})
            {
                int32_t q = (j + dj + N) % N * N + (i + di) % N;
                if (ni[q] >= 0)
                {
                    double f1 = -1e-4 * std::cos(PI * (j + .5) / N),
                           f2 = -1e-4 * std::cos(PI * (q / N + .5) / N);
                    double b = .25 * std::min(faces[ei[c]].h, faces[ni[q]].h) * .5 * (f1 + f2);
                    linear.add(ei[c], ni[q], -b);
                    linear.add(ni[q], ei[c], b);
                }
            }
        }
    std::vector<std::vector<std::pair<int32_t, double>>> rx(C), ry(C);
    for (int32_t k = 0; k < count; ++k)
    {
        auto const &f = faces[k];
        auto &interp = f.north ? ry : rx;
        interp[f.p].push_back({k, .5});
        interp[f.q].push_back({k, .5});
    }
    for (int32_t t = 0; t < P; ++t)
    {
        Vec force(count + pc), tx(C), ty(C);
        for (int32_t c = 0; c < C; ++c)
        {
            double speed = std::hypot(w.u[t * C + c], w.v[t * C + c]);
            tx[c] = 1.225 * .0013 * speed * w.u[t * C + c];
            ty[c] = 1.225 * .0013 * speed * w.v[t * C + c];
        }
        for (int32_t k = 0; k < count; ++k)
        {
            auto const &f = faces[k];
            auto const &stress = f.north ? ty : tx;
            force[k] = .5 * (stress[f.p] + stress[f.q]) * f.fraction / 1000.;
        }
        if (progress)
            progress("ocean-current-phase");
        check(cancellation);
        Vec state = solve(linear, force, 1e-10, {}, cancellation);
        double force_norm = norm(force);
        bool converged = false;
        for (int32_t iteration = 0; iteration <= 12; ++iteration)
        {
            Matrix tangent = linear;
            Vec stress(count + pc);
            for (int32_t c = 0; c < C; ++c)
            {
                double u = 0., v = 0.;
                for (auto [k, a] : rx[c])
                    u += a * state[k];
                for (auto [k, a] : ry[c])
                    v += a * state[k];
                double speed = std::hypot(u, v), inv = speed > 0 ? 1 / speed : 0.,
                       weight = .002 * wet[c];
                double uu = weight * (speed + u * u * inv), vv = weight * (speed + v * v * inv),
                       uv = weight * u * v * inv;
                for (auto [k, a] : rx[c])
                {
                    stress[k] += a * weight * speed * u;
                    for (auto [l, b] : rx[c])
                        tangent.add(k, l, a * b * uu);
                    for (auto [l, b] : ry[c])
                        tangent.add(k, l, a * b * uv);
                }
                for (auto [k, a] : ry[c])
                {
                    stress[k] += a * weight * speed * v;
                    for (auto [l, b] : ry[c])
                        tangent.add(k, l, a * b * vv);
                    for (auto [l, b] : rx[c])
                        tangent.add(k, l, a * b * uv);
                }
            }
            Vec residual = linear.apply(state);
            double momentum = 0., continuity = 0., fluxnorm = 0.;
            for (int32_t k = 0; k < count; ++k)
            {
                residual[k] += stress[k] - force[k];
                momentum += residual[k] * residual[k];
                double q = faces[k].h * state[k];
                fluxnorm += q * q;
            }
            for (int32_t k = count; k < count + pc; ++k)
            {
                double q = residual[k] / CSCALE;
                continuity += q * q;
            }
            if (std::sqrt(momentum) / std::max(force_norm, 1e-30) < 1e-9 &&
                std::sqrt(continuity) / std::max(std::sqrt(fluxnorm), 1e-30) < 1e-10)
            {
                converged = true;
                break;
            }
            if (iteration == 12)
                break;
            for (double &q : residual)
                q = -q;
            Vec delta = solve(tangent, residual, 1e-11, {}, cancellation);
            for (uint64_t k = 0; k < state.size(); ++k)
                state[k] += delta[k];
        }
        if (!converged)
            throw std::runtime_error("V9 nonlinear ocean current did not converge");
        Vec east(C), north(C);
        for (int32_t k = 0; k < count; ++k)
        {
            auto const &f = faces[k];
            if (f.north)
            {
                north[f.p] = state[k];
                out.north_volume[t * C + f.p] = state[k] * f.h * DX;
            }
            else
            {
                east[f.p] = state[k];
                out.east_volume[t * C + f.p] = state[k] * f.h * DX;
            }
        }
        for (int32_t c = 0; c < C; ++c)
        {
            out.u[t * C + c] = .5 * (east[c] + east[west(c)]);
            out.v[t * C + c] = .5 * (north[c] + north[(c + C - N) % C]);
        }
    }
    return out;
}
Matrix thermalMatrix(Vec const &capacity, bool local)
{
    Matrix a(P * C);
    double k = 2.4e13 / (DX * DX);
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
        {
            int32_t p = t * C + c;
            a.add(p, p, 2.09 + capacity[c] / DT);
            a.add(p, (t + P - 1) % P * C + c, -capacity[c] / DT);
            if (local)
            {
                for (int32_t q : {east(c), c + N})
                {
                    if (q >= C)
                        continue;
                    int32_t z = t * C + q;
                    a.add(p, p, k);
                    a.add(p, z, -k);
                    a.add(z, p, -k);
                    a.add(z, z, k);
                }
            }
            else
            {
                int32_t j = c / N;
                double degree = (j == 0 || j == N - 1) ? 1. : 2.;
                for (int32_t i = 0; i < N; ++i)
                {
                    a.add(p, t * C + j * N + i, k * degree / N);
                    if (j)
                        a.add(p, t * C + (j - 1) * N + i, -k / N);
                    if (j + 1 < N)
                        a.add(p, t * C + (j + 1) * N + i, -k / N);
                }
            }
        }
    return a;
}
Matrix heatAdvection(Ocean const &o)
{
    Matrix l(P * C);
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
            for (int32_t axis = 0; axis < 2; ++axis)
            {
                double f =
                    (axis ? o.north_volume : o.east_volume)[t * C + c] * 1000 * 3990 / (DX * DX);
                if (f == 0)
                    continue;
                int32_t q = axis ? c + N : east(c);
                if (q >= C)
                    throw std::runtime_error("V9 ocean crossed closed pole");
                int32_t p = t * C + c, z = t * C + q, donor = f >= 0 ? p : z;
                l.add(p, donor, f);
                l.add(z, donor, -f);
            }
    return l;
}
void addMatrix(Matrix &a, Matrix const &b)
{
    for (uint64_t i = 0; i < b.rows.size(); ++i)
        for (auto [j, v] : b.rows[i])
            a.add(static_cast<int32_t>(i), j, v);
}
void columnFields(V9ClimateThermal &result)
{
    auto &f = result.fields;
    auto const &theta = f.at("theta_C");
    auto const &ground = f.at("ground_m");
    auto const &ps = f.at("reference_ps");
    Vec mass(2 * C), sat(P * 2 * C), nodes(48), weights(48);
    double bounds[3] = {1., std::sqrt(.30), .30};
    for (int32_t i = 0; i < 24; ++i)
    {
        double z = std::cos(PI * (i + .75) / (48 + .5)), derivative = 0.;
        for (int32_t iteration = 0; iteration < 30; ++iteration)
        {
            double p0 = 1., p1 = z;
            for (int32_t l = 2; l <= 48; ++l)
            {
                double p = ((2 * l - 1) * z * p1 - (l - 1) * p0) / l;
                p0 = p1;
                p1 = p;
            }
            derivative = 48 * (z * p1 - p0) / (z * z - 1);
            double next = z - p1 / derivative;
            if (std::abs(next - z) < 1e-15)
            {
                z = next;
                break;
            }
            z = next;
        }
        double weight = 2 / ((1 - z * z) * derivative * derivative);
        nodes[i] = -z;
        nodes[47 - i] = z;
        weights[i] = weights[47 - i] = weight;
    }
    for (int32_t l = 0; l < 2; ++l)
        for (int32_t c = 0; c < C; ++c)
        {
            mass[l * C + c] = (bounds[l] - bounds[l + 1]) * ps[c] / 9.81;
            for (int32_t t = 0; t < P; ++t)
            {
                double tg = theta[t * C + c] + 273.15 - .0065 * ground[c], qmean = 0.;
                for (int32_t i = 0; i < 48; ++i)
                {
                    double sigma = .5 * (bounds[l] + bounds[l + 1]) +
                            .5 * (bounds[l] - bounds[l + 1]) * nodes[i],
                           temp = tg * std::pow(sigma, 287. * .0065 / 9.81), tc = temp - 273.15,
                           pressure = ps[c] * sigma,
                           es = 611.2 * std::exp(17.67 * tc / (tc + 243.5));
                    if (temp <= 29.65 || !std::isfinite(es) || es >= pressure)
                        throw std::runtime_error("V9 saturation domain");
                    qmean += weights[i] * .5 * .622 * es / (pressure - .378 * es);
                }
                sat[(t * 2 + l) * C + c] = mass[l * C + c] * qmean;
            }
        }
    f["layer_mass"] = mass;
    f["layer_saturation_kg_m2"] = sat;
    Vec fx(P * 2 * C), fy(P * 2 * (N + 1) * N);
    Mode m = mode();
    Vec mx(C), my((N + 1) * N), rawx(C), rawy((N + 1) * N);
    for (int32_t c = 0; c < C; ++c)
    {
        double total = mass[c] + mass[C + c], next = mass[east(c)] + mass[C + east(c)];
        mx[c] = .5 * (total + next);
        rawx[c] = mx[c] * .5 * (f.at("base_u")[c] + f.at("base_u")[east(c)]);
        if (c / N < N - 1)
        {
            my[c + N] = .5 * (total + mass[c + N] + mass[C + c + N]);
        }
    }
    Vec basefx, basefy;
    projectV9Flux(rawx, rawy, DX, basefx, basefy);
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
        {
            int32_t q = east(c);
            double ax = .5 * ((f.at("u")[t * C + c] - f.at("base_u")[c]) +
                              (f.at("u")[t * C + q] - f.at("base_u")[q])),
                   pair = .5 * (mass[c] + mass[q]) * ax;
            for (int32_t l = 0; l < 2; ++l)
                fx[(t * 2 + l) * C + c] = m.weights[l] * basefx[c] + (l == 0 ? pair : -pair);
            if (c / N < N - 1)
            {
                q = c + N;
                double ay = .5 * (f.at("v")[t * C + c] + f.at("v")[t * C + q]),
                       py = .5 * (mass[c] + mass[q]) * ay;
                for (int32_t l = 0; l < 2; ++l)
                    fy[(t * 2 + l) * (N + 1) * N + c + N] =
                        m.weights[l] * basefy[c + N] + (l == 0 ? py : -py);
            }
        }
    Vec lowerx(P * C), lowery(P * (N + 1) * N), loweru(P * C), lowerv(P * C), direct(N);
    for (int32_t j = 0; j < N; ++j)
    {
        double degrees = std::abs(180 * ((j + .5) / N - .5)),
               r = std::clamp((degrees - 25) / 10, 0., 1.);
        direct[j] = 1 - r * r * (3 - 2 * r);
    }
    double gain = -2.4e13 / DX / m.jump;
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
        {
            int32_t p = t * C + c;
            lowerx[p] = gain * (theta[p] - theta[t * C + east(c)]) * direct[c / N];
            if (c / N < N - 1)
                lowery[t * (N + 1) * N + c + N] =
                    gain * (theta[p] - theta[p + N]) * .5 * (direct[c / N] + direct[c / N + 1]);
        }
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
        {
            int32_t p = t * C + c;
            loweru[p] = .5 * (lowerx[p] + lowerx[t * C + west(c)]) / mass[c];
            lowerv[p] =
                .5 * (lowery[t * (N + 1) * N + c] + lowery[t * (N + 1) * N + c + N]) / mass[c];
            f.at("u")[p] += loweru[p];
            f.at("v")[p] += lowerv[p];
            for (int32_t l = 0; l < 2; ++l)
            {
                fx[(t * 2 + l) * C + c] += (l == 0 ? 1. : -1.) * lowerx[p];
                fy[(t * 2 + l) * (N + 1) * N + c] +=
                    (l == 0 ? 1. : -1.) * lowery[t * (N + 1) * N + c];
            }
        }
    f["overturning_lower_x_mass_flux"] = std::move(lowerx);
    f["overturning_lower_y_mass_flux"] = std::move(lowery);
    f["overturning_lower_u_m_s"] = std::move(loweru);
    f["overturning_lower_v_m_s"] = std::move(lowerv);
    f["layer_vapor_kg_m2"] = sat;
    for (double &v : f.at("layer_vapor_kg_m2"))
        v *= .5;
    f["layer_cloud_kg_m2"] = Vec(P * 2 * C);
    f["bucket_kg_m2"] = Vec(P * C);
    f["carrier_flux_x"] = std::move(fx);
    f["carrier_flux_y"] = std::move(fy);
}
} // namespace
double v9DryWaveSpeedSquared() { return mode().speed2; }
double v9DryStaticJump() { return mode().jump; }
V9ClimateThermal computeV9ClimateThermal(std::span<const double> bed, std::stop_token cancellation,
                            std::function<void(char const *)> const &progress)
{
    if (bed.size() != 256 * 256 ||
        !std::all_of(bed.begin(), bed.end(), [](double v) { return std::isfinite(v); }))
        throw std::invalid_argument("V9 requires finite 256 square bed");
    check(cancellation);
    if (progress)
        progress("reference-capacity");
    V9ClimateThermal result;
    auto &f = result.fields;
    Vec ground(C), wet(C), meanbed(C);
    for (int32_t j = 0; j < 256; ++j)
        for (int32_t i = 0; i < 256; ++i)
        {
            double a = bed[j * 256 + i], b = bed[(j + 1) % 256 * 256 + i],
                   c = bed[j * 256 + (i + 1) % 256], d = bed[(j + 1) % 256 * 256 + (i + 1) % 256];
            int32_t cell = j / 8 * N + i / 8;
            double value = .25 * (a + b + c + d);
            ground[cell] += std::max(value, 0.) / 64.;
            meanbed[cell] += value / 64.;
            wet[cell] += .25 * ((a < 0) + (b < 0) + (c < 0) + (d < 0)) / 64.;
        }
    Vec toa(P * C), rhs(P * C), ones(C, 1.);
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
        {
            double y = (c / N + .5) / N, q = std::cos(2 * PI * y),
                   amplitude = 30 + 150 * (1 + q) / 2;
            toa[t * C + c] = 340 - 140 * q + amplitude * std::cos(2 * PI * t / P);
            rhs[t * C + c] = .70 * toa[t * C + c] - 203.3;
        }
    Vec reference = thermalReduced(rhs, ones), ps(C), atmos(C), surface(C), capacity(C);
    Mode m = mode();
    for (int32_t c = 0; c < C; ++c)
    {
        double temp = 0.;
        for (int32_t t = 0; t < P; ++t)
            temp += reference[t * C + c] / P;
        double tk = temp + 273.15, local = tk - .0065 * ground[c];
        if (local <= 0)
            throw std::runtime_error("V9 hydrostatic domain");
        ps[c] = 101'325 * std::pow(local / tk, 9.81 / (287. * .0065));
        atmos[c] = .70 * ps[c] / 9.81 * m.capacity;
        surface[c] = (1 - wet[c]) * 2e6 + wet[c] * 1000 * 3990 * 20;
        capacity[c] = atmos[c] + surface[c];
    }
    Matrix a = thermalMatrix(capacity, true);
    if (progress)
        progress("local-thermal-control");
    Vec local_theta = solve(a, rhs, 1e-11, {}, cancellation);
    Wind control_wind = wind(local_theta, ground, cancellation);
    Ocean currents = ocean(control_wind, wet, cancellation, progress);
    Matrix l = heatAdvection(currents), combined = a;
    addMatrix(combined, l);
    Vec offset(P * C);
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
            offset[t * C + c] = .0065 * ground[c];
    Vec adv_offset = l.apply(offset), forcing = rhs;
    for (int32_t p = 0; p < P * C; ++p)
        forcing[p] += adv_offset[p];
    Vec theta = solve(combined, forcing, 1e-11, {}, cancellation), albedo(P * C), ice(P * C),
        snow(P * C), residual(P * C);
    bool converged = false;
    auto closure = [&]()
    {
        for (int32_t t = 0; t < P; ++t)
            for (int32_t c = 0; c < C; ++c)
            {
                int32_t p = t * C + c;
                double local = theta[p] - offset[p];
                ice[p] = .5 * (1 - std::tanh((local + 1.8) / 2.));
                snow[p] = .5 * (1 - std::tanh(local / 2.));
                albedo[p] = .30 + .15 * (wet[c] * ice[p] + (1 - wet[c]) * snow[p]);
            }
    };
    for (int32_t iteration = 0; iteration < 400; ++iteration)
    {
        check(cancellation);
        if (progress)
            progress("warm-albedo-iteration");
        closure();
        for (int32_t p = 0; p < P * C; ++p)
            forcing[p] = (1 - albedo[p]) * toa[p] - 203.3 + adv_offset[p];
        Vec updated = solve(combined, forcing, 1e-12, theta, cancellation);
        double delta = 0.;
        for (int32_t p = 0; p < P * C; ++p)
            delta = std::max(delta, std::abs(updated[p] - theta[p]));
        theta = std::move(updated);
        closure();
        residual = combined.apply(theta);
        double maximum = 0.;
        for (int32_t p = 0; p < P * C; ++p)
        {
            residual[p] -= (1 - albedo[p]) * toa[p] - 203.3 + adv_offset[p];
            maximum = std::max(maximum, std::abs(residual[p]));
        }
        if (maximum < 1e-5 && delta < 1e-5)
        {
            converged = true;
            break;
        }
    }
    if (!converged)
        throw std::runtime_error("V9 warm thermal albedo did not converge");
    Wind final_wind = wind(theta, ground, cancellation);
    Vec local(P * C), absorbed(P * C), outgoing(P * C), storage(P * C), dry(P * C);
    for (int32_t t = 0; t < P; ++t)
        for (int32_t c = 0; c < C; ++c)
        {
            int32_t p = t * C + c;
            local[p] = theta[p] - offset[p];
            absorbed[p] = (1 - albedo[p]) * toa[p];
            outgoing[p] = 203.3 + 2.09 * theta[p];
            storage[p] = capacity[c] * (theta[p] - theta[(t + P - 1) % P * C + c]) / DT;
            double k = 2.4e13 / (DX * DX);
            dry[p] = k * (2 * theta[p] - theta[t * C + east(c)] - theta[t * C + west(c)]);
            if (c / N)
                dry[p] += k * (theta[p] - theta[p - N]);
            if (c / N + 1 < N)
                dry[p] += k * (theta[p] - theta[p + N]);
        }
    f["ground_m"] = ground;
    f["bed_m"] = meanbed;
    f["wet_fraction"] = wet;
    f["reference_ps"] = ps;
    f["atmosphere_capacity"] = atmos;
    f["surface_capacity"] = surface;
    f["total_capacity"] = capacity;
    f["theta_C"] = theta;
    f["local_effective_C"] = local;
    f["thermal_residual"] = residual;
    f["combined_energy_residual"] = residual;
    f["ocean_heat_residual"] = residual;
    f["u"] = final_wind.u;
    f["v"] = final_wind.v;
    f["base_u"] = final_wind.base;
    f["terrain_u"] = final_wind.terrain_u;
    f["terrain_v"] = final_wind.terrain_v;
    f["thermal_forcing"] = final_wind.forcing;
    f["pressure_response"] = final_wind.response;
    f["ocean_u"] = currents.u;
    f["ocean_v"] = currents.v;
    f["ocean_east_volume"] = currents.east_volume;
    f["ocean_north_volume"] = currents.north_volume;
    f["ocean_heat_export_W_m2"] = l.apply(local);
    f["planetary_albedo"] = albedo;
    f["ocean_freezing_potential"] = ice;
    f["land_snow_potential"] = snow;
    f["absorbed_sw_W_m2"] = absorbed;
    f["outgoing_longwave_W_m2"] = outgoing;
    f["thermal_storage_W_m2"] = storage;
    f["dry_heat_export_W_m2"] = dry;
    f["TOA"] = toa;
    check(cancellation);
    if (progress)
        progress("ground-column-carrier");
    columnFields(result);
    return result;
}
} // namespace generation::detail
