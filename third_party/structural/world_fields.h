#ifndef STRUCTURAL_WORLDS_WORLD_FIELDS_H
#define STRUCTURAL_WORLDS_WORLD_FIELDS_H

#include "cubic_periodic.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>

class RegisteredWorldFields final
{
public:
    explicit RegisteredWorldFields(const std::string& filename) : fields_(3 * N * N)
    {
        std::ifstream input(filename, std::ios::binary);
        input.read(reinterpret_cast<char*>(fields_.data()),
            static_cast<std::streamsize>(fields_.size() * sizeof(double)));
        if (!input || input.peek() != std::ifstream::traits_type::eof()) {
            throw std::runtime_error("expected macro-centres, lowpass-vertices, slope-centres256 layers");
        }
        for (const auto value : fields_) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("nonfinite registered world fields");
            }
        }
    }
    explicit RegisteredWorldFields(std::span<const double> fields)
        : fields_(fields.begin(), fields.end())
    {
        if (fields_.size() != 3 * N * N) {
            throw std::invalid_argument("incorrect in-memory structural layer dimensions");
        }
        for (const auto value : fields_) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("nonfinite in-memory structural layer");
            }
        }
    }
    double sample(const double x, const double y, const uint32_t layer) const
    {
        if (layer >= 3) {
            throw std::invalid_argument("world layer must be0..2");
        }
        const double offset = layer == 1 ? 0.0 : .5;
        return cubicPeriodic(x / 256.0 - offset, y / 256.0 - offset, N,
            [&](uint32_t cx, uint32_t cy) {
                return fields_[layer * N * N + cy * N + cx];
            });
    }
private:
    static constexpr uint32_t N = 256;
    std::vector<double> fields_;
};
#endif
