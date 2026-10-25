#ifndef STRUCTURAL_WORLDS_CENTERED_CONTROL_H
#define STRUCTURAL_WORLDS_CENTERED_CONTROL_H

#include "controlfunction.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>

class CenteredControl final : public ControlFunction<CenteredControl>
{
    friend class ControlFunction<CenteredControl>;
public:
    explicit CenteredControl(const std::string& filename) : fields_(2 * N * N)
    {
        std::ifstream input(filename, std::ios::binary);
        input.read(reinterpret_cast<char*>(fields_.data()),
            static_cast<std::streamsize>(fields_.size() * sizeof(double)));
        if (!input || input.peek() != std::ifstream::traits_type::eof()) {
            throw std::runtime_error("expected two canonical cell-centred256-square control layers");
        }
        for (const auto value : fields_) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("nonfinite centred control");
            }
        }
    }
    explicit CenteredControl(std::span<const double> fields)
        : fields_(fields.begin(), fields.end())
    {
        if (fields_.size() != 2 * N * N) {
            throw std::invalid_argument("incorrect in-memory structural layer dimensions");
        }
        for (const auto value : fields_) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("nonfinite in-memory structural layer");
            }
        }
    }
    double reliefSlope(const double x, const double y) const
    {
        return sample(x, y, 1);
    }
private:
    static constexpr uint32_t N = 256;
    std::vector<double> fields_;

    double sample(const double x, const double y, const uint32_t layer) const
    {
        const double gx = x / 256.0 - .5;
        const double gy = y / 256.0 - .5;
        const auto ix = static_cast<int64_t>(std::floor(gx));
        const auto iy = static_cast<int64_t>(std::floor(gy));
        const auto at = [&](const int64_t sx, const int64_t sy) {
            const auto cx = static_cast<uint32_t>((sx % N + N) % N);
            const auto cy = static_cast<uint32_t>((sy % N + N) % N);
            return fields_[layer * N * N + cy * N + cx];
        };
        const double fx = gx - ix;
        const double fy = gy - iy;
        const double a = (1 - fx) * at(ix, iy) + fx * at(ix + 1, iy);
        const double b = (1 - fx) * at(ix, iy + 1) + fx * at(ix + 1, iy + 1);
        return (1 - fy) * a + fy * b;
    }
    double EvaluateImpl(const double x, const double y) const { return sample(x, y, 0); }
    bool InsideDomainImpl(double, double) const { return true; }
    double DistToDomainImpl(double, double) const { return 0.0; }
    double MinimumImpl() const { return 0.0; }
    double MaximumImpl() const { return 1.0; }
};
#endif
