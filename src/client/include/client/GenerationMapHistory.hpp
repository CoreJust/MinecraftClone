#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace client
{

// Read-only final fields in metres on a square periodic grid. Preparation owns a copy.
struct GenerationHistorySource final
{
    uint32_t resolution = 0U;
    std::span<double const> elevation_m;
    std::span<double const> water_level_m;
    std::span<uint32_t const> plate_ids;
    double sea_level_m = 0.0;
};

struct GenerationHistoryCore final
{
    std::array<double, 2U> source_center{};
    std::array<double, 2U> initial_displacement_cells{};
    // Exactly resolution squared binary cells; preparation copies this footprint.
    std::span<uint8_t const> source_footprint;
    double support_collar_cells = 3.0;
    double initial_rotation_radians = 0.0;
};

// A source-space scalar uplift/submergence event, never an RGB image.
struct GenerationHistoryEvent final
{
    std::span<double const> height_offset_m;
    double begin = 0.25;
    double end = 0.75;
};

struct GenerationHistoryFrame final
{
    uint32_t resolution = 0U;
    std::vector<double> elevation_m;
    std::vector<double> water_level_m;
    std::vector<float> liquid_fraction;
    std::vector<float> boundary_heat;
    std::vector<float> water_depth_m;
    std::vector<float> overlap_fraction;
    // Diagnostic ID only; do not threshold rendered color on discrete ownership.
    // Stable footprint-derived ID for a unique body; zero denotes background/ambiguous overlap.
    std::vector<uint64_t> source_body_id;
    // Continuous canonical-color blend weight, zero at full overlap or periodic mapping ambiguity.
    std::vector<float> source_mapping_weight;
    // Exact rigid samples in body interiors; weight ambiguous coordinates by source_mapping_weight.
    std::vector<std::array<float, 2U>> final_coordinates;

    // Reused evaluator workspace; these are not additional terrain source fields.
    std::vector<double> next_elevation_m;
    std::vector<float> warm_weight;
    std::vector<float> core_weight;
};

class GenerationMapHistory final
{
public:
    // Footprints must be nonempty, binary, mutually disjoint and leave some background.
    // Collars are positive and no larger than one quarter of the source side.
    // At most 64 bodies and 16 events. All floating inputs must have magnitude <= max(float)/24:
    // the 16-event limit plus 8 reserves headroom for scalar sums and water depth.
    // Unsupported values are rejected, never rescaled or saturated.
    [[nodiscard]]
    static std::optional<GenerationMapHistory> prepare(
        GenerationHistorySource const& source,
        std::span<GenerationHistoryCore const> cores = {},
        std::span<GenerationHistoryEvent const> events = {}
    );

    // Normalized illustration time, independent of actual model/readiness clocks.
    // Invalid time leaves frame unchanged. The endpoint and its derivatives settle
    // exactly before the final weather interval; there is no final image switch.
    [[nodiscard]]
    bool evaluate(double normalized_time, GenerationHistoryFrame& frame) const;

private:
    struct Data;
    explicit GenerationMapHistory(std::shared_ptr<Data const> data);
    std::shared_ptr<Data const> data_;
};

} // namespace client
