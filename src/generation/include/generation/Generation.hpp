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
struct V21ProviderState;
struct V10ProviderState;
} // namespace detail

inline constexpr uint32_t WORLD_SIZE_BLOCKS = 65'536U;
inline constexpr uint32_t WORLD_DEPTH_BLOCKS = 1'024U;
// V21 preserves its approved 0.34 source-metre scale and uses a separate
// vertical datum/domain. Legacy model output remains in its 1024-block range.
inline constexpr uint32_t V21_WORLD_DEPTH_BLOCKS = 2'048U;
// Bump MODEL_REVISION when fixed kernels, numerical steps, local refinement,
// or canonical surface-material classification changes.
inline constexpr uint32_t TERRAIN_VERSION = 2U;
inline constexpr uint32_t MODEL_REVISION = 6U;
inline constexpr std::string_view NUMERICAL_KERNEL_IDENTITY = "shelf-exp=exp64-to-f32-v1";
// SHA-256 of UTF-8 Python default json.dumps(sources, sort_keys=True) for the
// seven frozen v3 source hashes recorded in tests/fixtures/terrain_v2/manifest.json.
inline constexpr std::string_view MODEL_SOURCE_MAP_SHA256 =
    "5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704";
inline constexpr size_t CONFIGURATION_IDENTITY_MAX_BYTES = 384U;
inline constexpr uint32_t BLOCK_SEA_LEVEL = 384U;
inline constexpr uint32_t MAX_HEIGHT = 1'020U;
inline constexpr uint32_t V9_BLOCK_SEA_LEVEL = 384U;
inline constexpr uint32_t V9_MAX_HEIGHT = WORLD_DEPTH_BLOCKS - 1U;
inline constexpr uint32_t V21_BLOCK_SEA_LEVEL = 384U;
inline constexpr uint32_t V10_BLOCK_SEA_LEVEL = 512U;
inline constexpr uint32_t V10_WORLD_DEPTH_BLOCKS = 2'048U;
inline constexpr uint32_t V10_MAX_HEIGHT = V10_WORLD_DEPTH_BLOCKS - 1U;
inline constexpr std::string_view V10_MODEL_IDENTITY =
    "terrain-v10;source=f775213330028b165328885c6899ac0bf9c3c0c9a93ff6e0aca591554dec6e59";
inline constexpr std::string_view V10_FINE_TERRAIN_IDENTITY_SUFFIX =
    ";fine=e52e2e82b3e9873e14e543e0a7583fb978a182f1555997bda6febcd9ff25061a";
inline constexpr uint32_t V21_MAX_HEIGHT = V21_WORLD_DEPTH_BLOCKS - 1U;
inline constexpr std::string_view V21_MODEL_IDENTITY =
    "terrain-v21;model=a3f58ba8feb417556f6ae90e5e9b96b4ca318c169908dc8f30ce87bce61010cc";

enum class TerrainModelTag : uint8_t
{
    R6,
    V9,
    V21,
    V10,
};

[[nodiscard]]
constexpr uint32_t terrainVersion(TerrainModelTag const model) noexcept
{
    switch (model)
    {
    case TerrainModelTag::V10:
        return 10U;
    case TerrainModelTag::V9:
        return 9U;
    case TerrainModelTag::V21:
        return 21U;
    case TerrainModelTag::R6:
        return TERRAIN_VERSION;
    }
    return 0U;
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
    // Explicit experimental V10 composition; false retains the e37 reference surface.
    bool use_v10_fine_terrain = false;

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
    V21Platec,
    V21Terrain,
    V21Climate,
    V21Water,
    Complete,
    V10Platec,
    V10Terrain,
    V10Climate,
    V10Water,
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

enum class V21GenerationIceSurfaceKind : uint8_t
{
    None,
    Snow,
    GlacierIce,
    SeaIce,
};

struct V21GenerationSnapshotFields final
{
    // V21 source height is source metres; mapped gameplay height uses 0.34 blocks per metre.
    std::vector<float> source_elevation_m;
    std::vector<float> temperature_celsius;
    std::vector<float> precipitation_mm_year;
    std::vector<float> wind_u;
    std::vector<float> wind_v;
    std::vector<float> current_u;
    std::vector<float> current_v;
    // Eight phase-major 256 by 256 arrays, in native row-major order.
    std::vector<float> phase_temperature_celsius;
    std::vector<float> phase_precipitation_mm_year;
    std::vector<float> land_snow_water_equivalent_m;
    std::vector<float> glacier_thickness_m;
    std::vector<float> sea_ice_thickness_m;
    // V21 water levels are game-height blocks, unlike the V9 source-metre field.
    std::vector<float> water_level_blocks;
    std::vector<uint8_t> ocean_mask;
    std::vector<uint8_t> lake_mask;
    std::vector<uint8_t> river_mask;
    std::vector<uint8_t> biome_id;
    // Phase-zero display summary; phase stock arrays remain authoritative.
    std::vector<uint8_t> ice_surface_kind;
    std::vector<uint32_t> plate_ids;
    std::vector<GenerationPlateDirection> plate_directions;
    uint32_t plate_directions_observed_after_native_step = 0U;

    bool operator==(V21GenerationSnapshotFields const&) const = default;
};

// V10 source units are metres above sea zero, with one geometry metre per block.
struct V10GenerationSnapshotFields final
{
    std::vector<float> height_raw;
    std::vector<double> source_elevation_m;
    std::vector<uint32_t> plate_ids;
    std::vector<GenerationPlateDirection> plate_directions;
    uint32_t plate_directions_observed_after_native_step = 0U;
    std::vector<double> local_effective_celsius;
    std::vector<double> ground_m;
    std::vector<double> theta_celsius;
    std::vector<double> phase_precipitation_mm_year;
    // Actual coarse producer fields; point queries clip occupancy against the continuous bed.
    std::vector<float> water_level_m;
    std::vector<uint8_t> ocean_mask;
    std::vector<uint8_t> lake_mask;
    std::vector<uint8_t> river_mask;
    std::vector<float> sea_ice_thickness_m;
    std::vector<float> land_snow_water_equivalent_m;
    std::vector<float> glacier_thickness_m;

    bool operator==(V10GenerationSnapshotFields const&) const = default;
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
    std::optional<V21GenerationSnapshotFields> v21;
    std::optional<V10GenerationSnapshotFields> v10;
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
    // R6 fields below remain empty for V9 and V21 atlases.
    std::shared_ptr<detail::V9ProviderState const> v9_provider;
    std::shared_ptr<detail::V21ProviderState const> v21_provider;
    std::shared_ptr<detail::V10ProviderState const> v10_provider;
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
