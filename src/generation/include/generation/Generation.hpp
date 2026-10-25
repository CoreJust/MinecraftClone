#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace generation
{

namespace detail
{
struct V9ProviderState;
} // namespace detail

inline constexpr uint32_t WORLD_SIZE_BLOCKS = 65'536U;
inline constexpr uint32_t WORLD_DEPTH_BLOCKS = 1'024U;
// Bump MODEL_REVISION when fixed kernels, numerical steps, local refinement,
// or canonical surface-material classification changes.
inline constexpr uint32_t TERRAIN_VERSION = 2U;
inline constexpr uint32_t MODEL_REVISION = 6U;
inline constexpr std::string_view NUMERICAL_KERNEL_IDENTITY = "shelf-exp=exp64-to-f32-v1";
// SHA-256 of UTF-8 Python default json.dumps(sources, sort_keys=True) for the
// seven frozen v3 source hashes recorded in tests/fixtures/terrain_v2/manifest.json.
inline constexpr std::string_view MODEL_SOURCE_MAP_SHA256 =
    "5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704";
inline constexpr size_t CONFIGURATION_IDENTITY_MAX_BYTES = 320U;
inline constexpr uint32_t BLOCK_SEA_LEVEL = 384U;
inline constexpr uint32_t MAX_HEIGHT = 1'020U;
inline constexpr uint32_t V9_BLOCK_SEA_LEVEL = 384U;
inline constexpr uint32_t V9_MAX_HEIGHT = WORLD_DEPTH_BLOCKS - 1U;

enum class TerrainModelTag : uint8_t
{
    R6,
    V9,
};

[[nodiscard]]
constexpr uint32_t terrainVersion(TerrainModelTag const model) noexcept
{
    return model == TerrainModelTag::V9 ? 9U : TERRAIN_VERSION;
}

struct GenerationConfiguration
{
    uint64_t seed = 42U;
    uint32_t resolution = 256U;
    uint32_t epochs = 12U;
    uint32_t erosion_steps = 3U;
    uint32_t initial_climate_iterations = 24U;
    uint32_t climate_iterations = 80U;
    bool currents = true;
    int8_t wind_sign = 1;
    bool coastal_elaboration = true;
    TerrainModelTag model_tag = TerrainModelTag::R6;

    [[nodiscard]] bool operator==(GenerationConfiguration const&) const = default;
};

enum class GenerationStage : uint8_t
{
    Tectonics,
    CoastalElaboration,
    InitialDrainage,
    ProvisionalClimate,
    TerrainErosion,
    FinalClimate,
    FinalDrainage,
    V9Platec,
    V9Terrain,
    V9Climate,
    V9Water,
    Complete,
};

struct GenerationPlateDirection final
{
    uint32_t plate_id = 0U;
    float x = 0.0F;
    float y = 0.0F;

    bool operator==(GenerationPlateDirection const&) const = default;
};

enum class V9GenerationIceSurfaceKind : uint8_t
{
    None,
    LandSnow,
    WaterIce,
};

struct V9GenerationSnapshotFields final
{
    std::vector<float> source_elevation_m;
    std::vector<float> temperature_celsius;
    // Actual eight-phase native climate means, sampled on the progress snapshot grid.
    std::vector<float> wind_u;
    std::vector<float> wind_v;
    std::vector<float> current_u;
    std::vector<float> current_v;
    std::vector<float> precipitation_mm_year;
    std::vector<float> water_level_m;
    std::vector<uint8_t> ocean_mask;
    std::vector<uint8_t> lake_mask;
    std::vector<uint8_t> river_mask;
    std::vector<uint8_t> ice_surface_kind;
    std::vector<uint32_t> plate_ids;
    std::vector<GenerationPlateDirection> plate_directions;
    uint32_t plate_directions_observed_after_native_step = 0U;

    bool operator==(V9GenerationSnapshotFields const&) const = default;
};

struct GenerationSnapshot
{
    GenerationStage stage = GenerationStage::Tectonics;
    uint32_t resolution = 0U;
    // A missing vector means that stage has not produced that field yet.
    std::vector<float> elevation;
    std::vector<float> temperature;
    std::vector<float> precipitation;
    std::vector<float> water_level;
    std::vector<uint8_t> ocean_mask;
    std::vector<uint8_t> lake_mask;
    std::vector<uint8_t> river_mask;
    std::optional<V9GenerationSnapshotFields> v9;
};

[[nodiscard]]
bool isValidGenerationSnapshot(GenerationSnapshot const& snapshot) noexcept;

struct GenerationProgress
{
    GenerationStage stage = GenerationStage::Tectonics;
    uint32_t completed = 0U;
    uint32_t total = 0U;
    std::shared_ptr<GenerationSnapshot const> snapshot;
};

using ProgressCallback = std::function<void(GenerationProgress const&)>;

class GenerationCancelled final : public std::runtime_error
{
public:
    GenerationCancelled();
};

struct GlobalAtlas
{
    GenerationConfiguration configuration;
    // V9 retains its typed source-stage registry behind this immutable payload.
    // R6 fields below remain empty for V9 atlases.
    std::shared_ptr<detail::V9ProviderState const> v9_provider;
    float sea_level = 0.0F;
    uint32_t block_sea_level = BLOCK_SEA_LEVEL;
    uint32_t max_height = MAX_HEIGHT;
    double vertical_scale = 0.0;

    // Fields are flattened in NumPy-compatible row-major [y,x] order.
    std::vector<float> elevation;
    std::vector<float> tectonic_elevation;
    std::vector<float> filled;
    std::vector<float> water_level;
    std::vector<float> flow_accumulation;
    std::vector<int32_t> downstream;
    std::vector<int32_t> lake_id;
    std::vector<int32_t> outlet_id;
    std::vector<uint8_t> lake_mask;
    std::vector<uint8_t> river_mask;
    std::vector<uint8_t> ocean_mask;
    std::vector<uint8_t> fjord_candidate_mask;
    std::vector<uint8_t> harbour_candidate_mask;

    std::vector<float> temperature;
    std::vector<float> precipitation;
    std::vector<float> moisture;
    std::vector<float> wind_u;
    std::vector<float> wind_v;
    std::vector<float> current_u;
    std::vector<float> current_v;
    std::vector<uint8_t> climate_zone;
    std::vector<uint8_t> snow_mask;

    std::vector<float> uplift;
    std::vector<float> island_arc_uplift;
    std::vector<float> hotspot;
    std::vector<float> subduction;
    std::vector<float> divergence;
    std::vector<float> continental;
    std::vector<float> plate_vx;
    std::vector<float> plate_vy;
    std::vector<int16_t> plate_id;
};

// The identity is canonical for configurations accepted by prepareAtlas and
// stays within CONFIGURATION_IDENTITY_MAX_BYTES.
[[nodiscard]]
std::string configurationIdentity(GenerationConfiguration const& configuration);

[[nodiscard]]
std::shared_ptr<GlobalAtlas const> prepareAtlas(
    GenerationConfiguration const& configuration,
    std::stop_token cancellation = {},
    ProgressCallback progress = {}
);

[[nodiscard]]
bool isValidAtlas(GlobalAtlas const& atlas) noexcept;

} // namespace generation
