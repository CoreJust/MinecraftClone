#include "Numerics.hpp"
#include "NumpyRandom.hpp"

#include <generation/GenerationStages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace generation::detail
{

namespace
{

static constexpr double WORLD_SIZE = 65'536.0;
static constexpr double PI = 3.141'592'653'589'793'238'462'643'383'279'502'88;

struct Point
{
    double x = 0.0;
    double y = 0.0;
};

struct PlateMap
{
    Int16Field first;
    Int16Field second;
    DoubleField edge_distance;
};

struct MotionField
{
    Int16Field plate_id;
    Int16Field second_plate_id;
    DoubleField vx;
    DoubleField vy;
    DoubleField edge_distance;
};

struct CrustFields
{
    FloatField continental;
    FloatField initial_elevation;
};

[[nodiscard]]
double wrapValue(double value, double period)
{
    double wrapped = std::fmod(value, period);
    if (wrapped < 0.0) {
        wrapped += period;
    }
    return wrapped;
}

[[nodiscard]]
double wrappedDelta(double a, double b)
{
    double wrapped = std::fmod(a - b + WORLD_SIZE / 2.0, WORLD_SIZE);
    if (wrapped < 0.0) {
        wrapped += WORLD_SIZE;
    }
    return wrapped - WORLD_SIZE / 2.0;
}

[[nodiscard]]
std::pair<DoubleField, DoubleField> makeCoordinates(uint32_t resolution)
{
    uint64_t const field_size = static_cast<uint64_t>(resolution) * resolution;
    DoubleField x(field_size);
    DoubleField y(field_size);
    for (uint32_t row = 0U; row < resolution; ++row) {
        for (uint32_t column = 0U; column < resolution; ++column) {
            uint64_t const index = static_cast<uint64_t>(row) * resolution + column;
            x[index] = static_cast<double>(column) * WORLD_SIZE / resolution;
            y[index] = static_cast<double>(row) * WORLD_SIZE / resolution;
        }
    }
    return {std::move(x), std::move(y)};
}

[[nodiscard]]
PlateMap mapPlates(
    std::span<double const> x,
    std::span<double const> y,
    std::vector<Point> const& centres,
    std::stop_token cancellation
)
{
    uint64_t const field_size = x.size();
    double const infinity = std::numeric_limits<double>::infinity();
    PlateMap result{
        .first = Int16Field(field_size, 0),
        .second = Int16Field(field_size, 0),
        .edge_distance = DoubleField(field_size, 0.0),
    };
    DoubleField best(field_size, infinity);
    DoubleField second(field_size, infinity);

    uint32_t const plate_count = static_cast<uint32_t>(centres.size());
    for (uint32_t plate = 0U; plate < plate_count; ++plate) {
        checkCancellation(cancellation);
        for (uint64_t index = 0U; index < field_size; ++index) {
            double const dx = wrappedDelta(x[index], centres[plate].x);
            double const dy = wrappedDelta(y[index], centres[plate].y);
            double const distance = dx * dx + dy * dy;
            if (distance < best[index]) {
                second[index] = best[index];
                result.second[index] = result.first[index];
                best[index] = distance;
                result.first[index] = static_cast<int16_t>(plate);
            } else if (distance < second[index]) {
                second[index] = distance;
                result.second[index] = static_cast<int16_t>(plate);
            }
        }
    }

    for (uint64_t index = 0U; index < field_size; ++index) {
        uint32_t const first = static_cast<uint32_t>(result.first[index]);
        uint32_t const second_id = static_cast<uint32_t>(result.second[index]);
        double const dx = wrappedDelta(centres[second_id].x, x[index])
            - wrappedDelta(centres[first].x, x[index]);
        double const dy = wrappedDelta(centres[second_id].y, y[index])
            - wrappedDelta(centres[first].y, y[index]);
        double const separation = std::max(std::hypot(dx, dy), 1.0);
        result.edge_distance[index] = std::max((second[index] - best[index]) / (2.0 * separation), 0.0);
    }
    return result;
}

[[nodiscard]]
MotionField calculateMotion(
    std::span<double const> x,
    std::span<double const> y,
    std::vector<Point> const& centres,
    std::vector<Point> const& velocities,
    double blend_width,
    std::stop_token cancellation
)
{
    PlateMap plate_map = mapPlates(x, y, centres, cancellation);
    uint64_t const field_size = x.size();
    uint32_t const plate_count = static_cast<uint32_t>(centres.size());
    MotionField motion{
        .plate_id = std::move(plate_map.first),
        .second_plate_id = std::move(plate_map.second),
        .vx = DoubleField(field_size, 0.0),
        .vy = DoubleField(field_size, 0.0),
        .edge_distance = std::move(plate_map.edge_distance),
    };
    for (uint64_t index = 0U; index < field_size; ++index) {
        if (index % 4'096U == 0U) {
            checkCancellation(cancellation);
        }
        uint32_t const nearest = static_cast<uint32_t>(motion.plate_id[index]);
        double const nearest_dx = wrappedDelta(x[index], centres[nearest].x);
        double const nearest_dy = wrappedDelta(y[index], centres[nearest].y);
        double const nearest_distance = nearest_dx * nearest_dx + nearest_dy * nearest_dy;
        double sum_weights = 0.0;
        double vx = 0.0;
        double vy = 0.0;
        for (uint32_t plate = 0U; plate < plate_count; ++plate) {
            double const dx = wrappedDelta(x[index], centres[plate].x);
            double const dy = wrappedDelta(y[index], centres[plate].y);
            double const distance = dx * dx + dy * dy;
            double const weight = std::exp(
                -0.5 * (distance - nearest_distance) / (blend_width * blend_width)
            );
            sum_weights += weight;
            vx += weight * velocities[plate].x;
            vy += weight * velocities[plate].y;
        }
        motion.vx[index] = vx / sum_weights;
        motion.vy[index] = vy / sum_weights;
    }
    return motion;
}

[[nodiscard]]
CrustFields makeInitialCrust(
    uint64_t seed,
    uint32_t resolution,
    std::span<double const> x,
    std::span<double const> y,
    std::span<float const> substrate,
    NumpyRandom& random,
    std::stop_token cancellation
)
{
    uint64_t const field_size = x.size();
    uint32_t const large_count = static_cast<uint32_t>(random.integers(3, 6));
    uint32_t const middle_count = static_cast<uint32_t>(random.integers(6, 11));
    uint32_t const island_count = static_cast<uint32_t>(random.integers(10, 19));
    FloatField warp_x_noise = periodicFbm(seed + 701U, resolution, 3U, 2U, cancellation);
    FloatField warp_y_noise = periodicFbm(seed + 709U, resolution, 3U, 2U, cancellation);
    DoubleField warped_x(field_size);
    DoubleField warped_y(field_size);
    float const warp_scale = static_cast<float>(0.040 * WORLD_SIZE);
    for (uint64_t index = 0U; index < field_size; ++index) {
        float const warp_x = warp_scale * warp_x_noise[index];
        float const warp_y = warp_scale * warp_y_noise[index];
        warped_x[index] = x[index] + warp_x;
        warped_y[index] = y[index] + warp_y;
    }

    uint32_t const concentration_axis = static_cast<uint32_t>(random.integers(0, 2));
    double const concentration_phase = random.uniform(0.0, 2.0 * PI);
    double const concentration = random.uniform(0.35, 0.85);
    NumpyRandom cluster_random(seed + 8'321U);
    bool const clustered_large_cores = cluster_random.random() < 0.3;

    DoubleField strongest(field_size, 0.0);
    DoubleField runner_up(field_size, 0.0);
    std::vector<Point> large_centres;
    large_centres.reserve(large_count);

    auto place_core = [&]() {
        Point centre{
            .x = random.uniform(0.0, WORLD_SIZE),
            .y = random.uniform(0.0, WORLD_SIZE),
        };
        if (random.random() < concentration) {
            double const coordinate = random.vonmises(concentration_phase, 1.8) * WORLD_SIZE
                / (2.0 * PI);
            if (concentration_axis == 0U) {
                centre.x = wrapValue(coordinate, WORLD_SIZE);
            } else {
                centre.y = wrapValue(coordinate, WORLD_SIZE);
            }
        }
        return centre;
    };

    auto add_core = [&](Point centre, double major, double minor, double amplitude, double angle) {
        checkCancellation(cancellation);
        double const cosine = std::cos(angle);
        double const sine = std::sin(angle);
        for (uint64_t index = 0U; index < field_size; ++index) {
            double const dx = wrappedDelta(warped_x[index], centre.x);
            double const dy = wrappedDelta(warped_y[index], centre.y);
            double const u = (cosine * dx + sine * dy) / major;
            double const v = (-sine * dx + cosine * dy) / minor;
            double const influence = amplitude * std::exp(-0.5 * (u * u + v * v));
            if (influence > strongest[index]) {
                runner_up[index] = strongest[index];
                strongest[index] = influence;
            } else {
                runner_up[index] = std::max(runner_up[index], influence);
            }
        }
    };

    uint32_t const core_count = large_count + middle_count + island_count;
    for (uint32_t index = 0U; index < core_count; ++index) {
        checkCancellation(cancellation);
        Point centre;
        double radius = 0.0;
        double aspect = 0.0;
        double amplitude = 0.0;
        if (index < large_count) {
            if (clustered_large_cores) {
                for (uint32_t attempt = 0U; attempt < 40U; ++attempt) {
                    centre = place_core();
                    bool separated = true;
                    for (Point const& previous : large_centres) {
                        double const dx = wrappedDelta(centre.x, previous.x);
                        double const dy = wrappedDelta(centre.y, previous.y);
                        if (std::hypot(dx, dy) <= 0.17 * WORLD_SIZE) {
                            separated = false;
                            break;
                        }
                    }
                    if (separated) {
                        break;
                    }
                }
            } else {
                std::array<Point, 80U> choices{};
                for (Point& choice : choices) {
                    choice.x = random.uniform(0.0, WORLD_SIZE);
                    choice.y = random.uniform(0.0, WORLD_SIZE);
                }
                if (large_centres.empty()) {
                    centre = choices[0];
                } else {
                    double farthest_separation = -1.0;
                    for (Point const& choice : choices) {
                        double nearest_separation = std::numeric_limits<double>::infinity();
                        for (Point const& previous : large_centres) {
                            double const dx = wrappedDelta(choice.x, previous.x);
                            double const dy = wrappedDelta(choice.y, previous.y);
                            nearest_separation = std::min(nearest_separation, dx * dx + dy * dy);
                        }
                        if (nearest_separation > farthest_separation) {
                            farthest_separation = nearest_separation;
                            centre = choice;
                        }
                    }
                }
            }
            large_centres.push_back(centre);
            double const scale = std::sqrt(5.0 / large_count);
            radius = random.uniform(0.17, 0.225) * scale * WORLD_SIZE;
            aspect = random.uniform(1.15, 2.4);
            amplitude = random.uniform(0.9, 1.1);
        } else {
            centre = place_core();
            bool const middle_core = index < large_count + middle_count;
            if (middle_core) {
                if (random.random() < 0.35) {
                    uint32_t const anchor = static_cast<uint32_t>(random.integers(0, large_count));
                    double const bearing = random.uniform(0.0, 2.0 * PI);
                    double const offset = random.uniform(0.13, 0.25) * WORLD_SIZE;
                    centre.x = wrapValue(
                        large_centres[anchor].x + offset * std::cos(bearing),
                        WORLD_SIZE
                    );
                    centre.y = wrapValue(
                        large_centres[anchor].y + offset * std::sin(bearing),
                        WORLD_SIZE
                    );
                }
                radius = random.uniform(0.065, 0.11) * WORLD_SIZE;
                aspect = random.uniform(1.2, 2.7);
                amplitude = random.uniform(0.75, 1.1);
            } else {
                if (random.random() < 0.55) {
                    uint32_t const anchor = static_cast<uint32_t>(random.integers(0, large_count));
                    double const bearing = random.uniform(0.0, 2.0 * PI);
                    double const offset = random.uniform(0.14, 0.29) * WORLD_SIZE;
                    centre.x = wrapValue(
                        large_centres[anchor].x + offset * std::cos(bearing),
                        WORLD_SIZE
                    );
                    centre.y = wrapValue(
                        large_centres[anchor].y + offset * std::sin(bearing),
                        WORLD_SIZE
                    );
                }
                radius = random.uniform(0.025, 0.052) * WORLD_SIZE;
                aspect = random.uniform(1.1, 3.2);
                amplitude = random.uniform(0.65, 1.15);
            }
        }
        double const angle = random.uniform(0.0, 2.0 * PI);
        double const major = radius * std::sqrt(aspect);
        double const minor = radius / std::sqrt(aspect);
        add_core(centre, major, minor, amplitude, angle);
    }

    DoubleField overlap(field_size);
    DoubleField crust(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        overlap[index] = std::clamp((runner_up[index] - 0.35) / 0.25, 0.0, 1.0);
        double const gap = (strongest[index] - runner_up[index]) / 0.25;
        crust[index] = strongest[index] - 1.2 * overlap[index] * std::exp(-0.5 * gap * gap);
    }

    NumpyRandom lobe_random(seed + 9'421U);
    bool const lobe_regime = clustered_large_cores && lobe_random.random() < 0.35;
    double lobe_strength = 0.0;
    if (lobe_regime) {
        lobe_strength = lobe_random.uniform(0.55, 0.70);
    }
    FloatField roughness_noise = periodicFbm(seed + 719U, resolution, 5U, 2U, cancellation);
    for (uint64_t index = 0U; index < field_size; ++index) {
        double const cold_offset = 2.0 * std::abs(y[index] / WORLD_SIZE - 0.5);
        double const cold_proxy = cold_offset * cold_offset;
        double const roughness = 0.075 + 0.16 * cold_proxy + 0.12 * overlap[index];
        if (lobe_strength > 0.0) {
            double const coordinate = concentration_axis == 0U ? x[index] : y[index];
            crust[index] += lobe_strength * std::cos(
                2.0 * PI * coordinate / WORLD_SIZE - concentration_phase
            );
        }
        crust[index] += roughness * roughness_noise[index];
    }

    DoubleField median_values = crust;
    std::sort(median_values.begin(), median_values.end());
    uint64_t const median_index = field_size / 2U;
    double const median = 0.5 * (median_values[median_index - 1U] + median_values[median_index]);
    CrustFields result{
        .continental = FloatField(field_size),
        .initial_elevation = FloatField(field_size),
    };
    for (uint64_t index = 0U; index < field_size; ++index) {
        double const centred = crust[index] - median;
        result.continental[index] = static_cast<float>(1.0 / (1.0 + std::exp(-8.0 * centred)));
        result.initial_elevation[index] = -4.2F + 5.0F * result.continental[index]
            + 0.18F * substrate[index];
    }
    return result;
}

} // namespace

TectonicsFields generateTectonics(
    GenerationConfiguration const& configuration,
    std::stop_token cancellation,
    ProgressCallback const& progress
)
{
    uint32_t const resolution = configuration.resolution;
    uint64_t const field_size = static_cast<uint64_t>(resolution) * resolution;
    if (resolution < 8U || field_size > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("tectonics resolution must be at least 8 and fit a 32-bit field size");
    }
    checkCancellation(cancellation);

    NumpyRandom random(configuration.seed);
    uint32_t const plate_count = static_cast<uint32_t>(random.integers(16, 23));
    std::vector<Point> centres(plate_count);
    std::vector<Point> velocities(plate_count);
    for (uint32_t plate = 0U; plate < plate_count; ++plate) {
        centres[plate] = Point{
            .x = random.uniform(0.0, WORLD_SIZE),
            .y = random.uniform(0.0, WORLD_SIZE),
        };
    }
    std::vector<double> directions(plate_count);
    std::vector<double> speeds(plate_count);
    for (double& direction : directions) {
        direction = random.uniform(0.0, 2.0 * PI);
    }
    for (double& speed : speeds) {
        speed = random.uniform(250.0, 850.0);
    }
    for (uint32_t plate = 0U; plate < plate_count; ++plate) {
        velocities[plate] = Point{
            .x = std::cos(directions[plate]) * speeds[plate],
            .y = std::sin(directions[plate]) * speeds[plate],
        };
    }

    auto [x, y] = makeCoordinates(resolution);
    FloatField warp_x_noise = periodicFbm(
        configuration.seed + 401U,
        resolution,
        3U,
        2U,
        cancellation
    );
    FloatField warp_y_noise = periodicFbm(
        configuration.seed + 409U,
        resolution,
        3U,
        2U,
        cancellation
    );
    DoubleField warped_x(field_size);
    DoubleField warped_y(field_size);
    float const warp_scale = static_cast<float>(0.030 * WORLD_SIZE);
    for (uint64_t index = 0U; index < field_size; ++index) {
        float const warp_x = warp_scale * warp_x_noise[index];
        float const warp_y = warp_scale * warp_y_noise[index];
        warped_x[index] = x[index] + warp_x;
        warped_y[index] = y[index] + warp_y;
    }

    DoubleField polarity_rank(plate_count);
    for (double& rank : polarity_rank) {
        rank = random.uniform(0.0, 1.0);
    }
    FloatField substrate = periodicFbm(
        configuration.seed + 211U,
        resolution,
        4U,
        2U,
        cancellation
    );
    CrustFields crust = makeInitialCrust(
        configuration.seed,
        resolution,
        x,
        y,
        substrate,
        random,
        cancellation
    );

    FloatField uplift(field_size, 0.0F);
    FloatField island_arc_uplift(field_size, 0.0F);
    FloatField subduction(field_size, 0.0F);
    FloatField divergence(field_size, 0.0F);
    FloatField arc_segments = periodicFbm(
        configuration.seed + 521U,
        resolution,
        3U,
        5U,
        cancellation
    );
    for (float& value : arc_segments) {
        value = std::clamp(0.85F + 1.2F * value, 0.25F, 1.5F);
    }

    double const cell_size = WORLD_SIZE / resolution;
    double const blend_width = 0.055 * WORLD_SIZE;
    double const broad_cells = std::max(0.016 * WORLD_SIZE / cell_size, 0.5);
    double const narrow_cells = std::max(0.004 * WORLD_SIZE / cell_size, 0.5);
    double const blend_denominator = 800.0 * cell_size;

    for (uint32_t epoch = 0U; epoch < configuration.epochs; ++epoch) {
        checkCancellation(cancellation);
        MotionField const motion = calculateMotion(
            warped_x,
            warped_y,
            centres,
            velocities,
            blend_width,
            cancellation
        );
        DoubleField sampled_continental(field_size);
        FloatField next_uplift(field_size);
        FloatField next_island_arc_uplift(field_size);
        FloatField next_subduction(field_size);
        FloatField next_divergence(field_size);
        for (uint64_t index = 0U; index < field_size; ++index) {
            double const source_x = x[index] - motion.vx[index];
            double const source_y = y[index] - motion.vy[index];
            sampled_continental[index] = bilinearPeriodic(
                crust.continental,
                resolution,
                source_x,
                source_y
            );
            next_uplift[index] = static_cast<float>(
                0.88 * bilinearPeriodic(uplift, resolution, source_x, source_y)
            );
            next_island_arc_uplift[index] = static_cast<float>(
                0.88 * bilinearPeriodic(island_arc_uplift, resolution, source_x, source_y)
            );
            next_subduction[index] = static_cast<float>(
                0.88 * bilinearPeriodic(subduction, resolution, source_x, source_y)
            );
            next_divergence[index] = static_cast<float>(
                0.88 * bilinearPeriodic(divergence, resolution, source_x, source_y)
            );
        }
        DoubleField smoothed_continental = gaussianFilterWrap(
            sampled_continental,
            resolution,
            0.45,
            cancellation
        );
        for (uint64_t index = 0U; index < field_size; ++index) {
            crust.continental[index] = static_cast<float>(smoothed_continental[index]);
        }
        uplift = std::move(next_uplift);
        island_arc_uplift = std::move(next_island_arc_uplift);
        subduction = std::move(next_subduction);
        divergence = std::move(next_divergence);

        std::pair<DoubleField, DoubleField> const vx_gradient = gradientPeriodic(motion.vx, resolution);
        std::pair<DoubleField, DoubleField> const vy_gradient = gradientPeriodic(motion.vy, resolution);
        DoubleField convergent(field_size);
        DoubleField divergent(field_size);
        for (uint64_t index = 0U; index < field_size; ++index) {
            double const strain = (vx_gradient.first[index] + vy_gradient.second[index])
                * blend_width / blend_denominator;
            convergent[index] = std::clamp(-strain, 0.0, 1.2);
            divergent[index] = std::clamp(strain, 0.0, 1.2);
        }
        DoubleField broad_convergence = gaussianFilterWrap(
            convergent,
            resolution,
            broad_cells,
            cancellation
        );
        DoubleField narrow_convergence = gaussianFilterWrap(
            convergent,
            resolution,
            narrow_cells,
            cancellation
        );
        DoubleField broad_divergence = gaussianFilterWrap(
            divergent,
            resolution,
            broad_cells,
            cancellation
        );
        DoubleField narrow_divergence = gaussianFilterWrap(
            divergent,
            resolution,
            narrow_cells,
            cancellation
        );

        FloatField continent(field_size);
        for (uint64_t index = 0U; index < field_size; ++index) {
            float const raw = (crust.continental[index] - 0.38F) / 0.24F;
            float const value = std::clamp(raw, 0.0F, 1.0F);
            continent[index] = value * value * (3.0F - 2.0F * value);
        }
        FloatField local_continent = gaussianFilterWrap(
            continent,
            resolution,
            broad_cells,
            cancellation
        );
        DoubleField polarity(field_size);
        for (uint64_t index = 0U; index < field_size; ++index) {
            uint32_t const first = static_cast<uint32_t>(motion.plate_id[index]);
            double const rank_delta = polarity_rank[first]
                - polarity_rank[static_cast<uint32_t>(motion.second_plate_id[index])];
            double const rank_sign = (rank_delta > 0.0) - (rank_delta < 0.0);
            polarity[index] = 0.5 + 0.5 * numpyTanh(
                motion.edge_distance[index] / (0.006 * WORLD_SIZE)
            ) * rank_sign;
        }
        polarity = gaussianFilterWrap(polarity, resolution, narrow_cells, cancellation);

        for (uint64_t index = 0U; index < field_size; ++index) {
            float const continent_weight = continent[index];
            float const local_weight = local_continent[index];
            float const cc = continent_weight * local_weight;
            float const oo = (1.0F - continent_weight) * (1.0F - local_weight);
            float const mixed_continent = continent_weight * (1.0F - local_weight);
            float const mixed_ocean = (1.0F - continent_weight) * local_weight;
            double const collision = 2.5 * broad_convergence[index] * cc;
            double const continental_arc = 2.2 * broad_convergence[index] * mixed_continent;
            double const island_arc = 2.0 * narrow_convergence[index] * oo
                * polarity[index] * arc_segments[index];
            double const trench = -1.5 * narrow_convergence[index]
                * (mixed_ocean + oo * (1.0 - polarity[index]));
            double const sea_ridge = 0.9 * narrow_divergence[index] * oo;
            double const rift = -0.55 * narrow_divergence[index] * continent_weight;
            double const shoulders = 0.55
                * std::max(broad_divergence[index] - narrow_divergence[index], 0.0)
                * continent_weight;
            uplift[index] = static_cast<float>(std::clamp(
                uplift[index] + 0.24 * (collision + continental_arc + island_arc + shoulders),
                0.0,
                6.0
            ));
            island_arc_uplift[index] = static_cast<float>(
                island_arc_uplift[index] + 0.24 * island_arc
            );
            subduction[index] = static_cast<float>(std::clamp(
                subduction[index] + 0.24 * trench,
                -3.0,
                0.0
            ));
            divergence[index] = static_cast<float>(std::clamp(
                divergence[index] + 0.24 * (sea_ridge + rift),
                -1.5,
                2.0
            ));
        }
        for (uint32_t plate = 0U; plate < plate_count; ++plate) {
            centres[plate].x = wrapValue(centres[plate].x + velocities[plate].x, WORLD_SIZE);
            centres[plate].y = wrapValue(centres[plate].y + velocities[plate].y, WORLD_SIZE);
        }
        reportProgress(progress, GenerationStage::Tectonics, epoch + 1U, configuration.epochs);
    }

    MotionField final_motion = calculateMotion(
        warped_x,
        warped_y,
        centres,
        velocities,
        blend_width,
        cancellation
    );
    FloatField hotspot(field_size, 0.0F);
    NumpyRandom hotspot_random(configuration.seed + 9'991U);
    uint32_t const hotspot_count = configuration.epochs == 0U
        ? 0U
        : static_cast<uint32_t>(hotspot_random.integers(0, 4));
    double const sigma = std::max(0.004 * WORLD_SIZE, cell_size);
    for (uint32_t source = 0U; source < hotspot_count; ++source) {
        checkCancellation(cancellation);
        Point location;
        bool accepted = false;
        for (uint32_t attempt = 0U; attempt < 20U; ++attempt) {
            location = Point{
                .x = hotspot_random.uniform(0.0, WORLD_SIZE),
                .y = hotspot_random.uniform(0.0, WORLD_SIZE),
            };
            if (bilinearPeriodic(crust.continental, resolution, location.x, location.y) < 0.35) {
                accepted = true;
                break;
            }
        }
        if (!accepted) {
            continue;
        }
        double const motion_x = bilinearPeriodic(final_motion.vx, resolution, location.x, location.y);
        double const motion_y = bilinearPeriodic(final_motion.vy, resolution, location.x, location.y);
        double const speed = std::max(std::hypot(motion_x, motion_y), 1.0);
        double const ux = motion_x / speed;
        double const uy = motion_y / speed;
        for (uint32_t age = 0U; age < 8U; ++age) {
            checkCancellation(cancellation);
            double const trail_x = location.x + age * motion_x;
            double const trail_y = location.y + age * motion_y;
            double const age_weight = 2.5 * std::exp(-static_cast<double>(age) / 3.5);
            for (uint64_t index = 0U; index < field_size; ++index) {
                double const dx = wrappedDelta(x[index], trail_x);
                double const dy = wrappedDelta(y[index], trail_y);
                double const along = ux * dx + uy * dy;
                double const across = -uy * dx + ux * dy;
                double const along_scaled = along / (1.4 * sigma);
                double const across_scaled = across / sigma;
                float const increment = static_cast<float>(age_weight * std::exp(
                    -0.5 * (along_scaled * along_scaled + across_scaled * across_scaled)
                ));
                hotspot[index] += increment;
            }
        }
    }

    FloatField elevation(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        elevation[index] = -4.2F + 5.0F * crust.continental[index]
            + 0.18F * substrate[index] + uplift[index]
            + subduction[index] + divergence[index] + hotspot[index];
    }
    if (configuration.epochs > 0U) {
        FloatField ruggedness = periodicFbm(
            configuration.seed + 307U,
            resolution,
            5U,
            2U,
            cancellation
        );
        for (uint64_t index = 0U; index < field_size; ++index) {
            float const amplitude = 0.30F * ruggedness[index]
                * std::min(uplift[index] / 2.0F, 1.0F);
            elevation[index] += amplitude;
        }
        FloatField shelf_noise = periodicFbm(
            configuration.seed + 727U,
            resolution,
            6U,
            2U,
            cancellation
        );
        for (uint64_t index = 0U; index < field_size; ++index) {
            double const cold_offset = 2.0 * std::abs(y[index] / WORLD_SIZE - 0.5);
            double const cold_proxy = cold_offset * cold_offset;
            float const activity = std::clamp(
                (uplift[index] - subduction[index] + std::abs(divergence[index])) / 2.5F,
                0.0F,
                1.0F
            );
            float const continental_offset = (crust.continental[index] - 0.5F) / 0.18F;
            float const shelf_exponent = -0.5F * continental_offset * continental_offset;
            float const shelf = static_cast<float>(
                std::exp(static_cast<double>(shelf_exponent))
            );
            float const shelf_noise_scale = 0.18F * shelf_noise[index];
            double const roughness_factor = 0.2 + 0.5 * cold_proxy + 0.5 * activity;
            float const amplitude = static_cast<float>(shelf_noise_scale * roughness_factor * shelf);
            elevation[index] += amplitude;
        }
    }

    FloatField plate_vx(field_size);
    FloatField plate_vy(field_size);
    for (uint64_t index = 0U; index < field_size; ++index) {
        plate_vx[index] = static_cast<float>(final_motion.vx[index]);
        plate_vy[index] = static_cast<float>(final_motion.vy[index]);
    }
    TectonicsFields result{
        .elevation = std::move(elevation),
        .initial_elevation = std::move(crust.initial_elevation),
        .continental = std::move(crust.continental),
        .uplift = std::move(uplift),
        .island_arc_uplift = std::move(island_arc_uplift),
        .hotspot = std::move(hotspot),
        .subduction = std::move(subduction),
        .divergence = std::move(divergence),
        .plate_vx = std::move(plate_vx),
        .plate_vy = std::move(plate_vy),
        .plate_id = std::move(final_motion.plate_id),
    };
    return result;
}

} // namespace generation::detail
