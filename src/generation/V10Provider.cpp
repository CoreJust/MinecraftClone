#include <generation/V10Provider.hpp>

#include "Sha256.hpp"
#include "V10Climate.hpp"
#include "V10ProviderInternal.hpp"
#include "V9WaterFields.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace generation
{
namespace
{
constexpr size_t SAMPLE_COUNT = 256U * 256U;

void checkCancellation(std::stop_token const cancellation)
{
    if (cancellation.stop_requested())
    {
        throw GenerationCancelled{};
    }
}

void validateHeight(double const elevation_m)
{
    if (!std::isfinite(elevation_m) || elevation_m < -512.0 || elevation_m >= 1536.0)
    {
        throw std::out_of_range("V10 height is outside the 2048-block domain at datum 512");
    }
}

detail::V10ProviderState const& state(GlobalAtlas const& atlas)
{
    if (!detail::isValidV10Atlas(atlas))
    {
        throw std::invalid_argument("V10 source atlas is not prepared");
    }
    return *atlas.v10_provider;
}

double wrapNativeCoordinate(double coordinate)
{
    double result = std::fmod(coordinate, detail::V10_FOUNDATION_PERIOD_M);
    if (result < 0.0)
    {
        result += detail::V10_FOUNDATION_PERIOD_M;
    }
    return result >= detail::V10_FOUNDATION_PERIOD_M ? 0.0 : result;
}

V10NativeBedSample nativeBedSample(detail::V10FoundationBasis const& foundation,
    detail::V10WaterPreparation const& water, std::array<double, 2U> const& xy)
{
    if (!std::isfinite(xy[0U]) || !std::isfinite(xy[1U]))
    {
        throw std::invalid_argument("V10 native bed query requires finite XY coordinates");
    }
    double const x = wrapNativeCoordinate(xy[0U]);
    double const y = wrapNativeCoordinate(xy[1U]);
    double const height = detail::sampleV10FoundationHeight(foundation, x, y);
    validateHeight(height);
    auto const [stage, owner] = water.refinement.standing(x, y);
    return {xy, height, stage, owner};
}

// Stable little-endian field serialization binds the identity to the full catalogs.
template<typename T>
void appendIdentityBytes(std::vector<uint8_t>& bytes, T value)
{
    auto const bits = [&] {
        if constexpr (std::is_same_v<T, double>)
        {
            return std::bit_cast<uint64_t>(value);
        }
        else
        {
            return static_cast<uint64_t>(value);
        }
    }();
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        bytes.push_back(static_cast<uint8_t>(bits >> (8U * i)));
    }
}

std::string identityHash(std::vector<uint8_t> const& bytes)
{
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    for (uint8_t byte : detail::sha256Digest(bytes))
    {
        result.push_back(HEX[byte >> 4U]);
        result.push_back(HEX[byte & 15U]);
    }
    return result;
}

std::string fineTerrainParentIdentity(std::span<float const> raw_height)
{
    constexpr std::string_view SOURCE_ID =
        "v10-foundation-v1;source=f775213330028b165328885c6899ac0bf9c3c0c9a93ff6e0aca591554dec6e59";
    std::vector<uint8_t> bytes(SOURCE_ID.begin(), SOURCE_ID.end());
    appendIdentityBytes(bytes, detail::V10_FINE_TERRAIN_SEED);
    appendIdentityBytes(bytes, detail::V10_FINE_TERRAIN_PERIOD_M);
    appendIdentityBytes(bytes, static_cast<uint32_t>(raw_height.size()));
    for (float const height : raw_height)
    {
        appendIdentityBytes(bytes, std::bit_cast<uint32_t>(height));
    }
    return identityHash(bytes);
}

detail::V10FineTerrain const& fineTerrain(detail::V10ProviderState const& value)
{
    detail::V10FineTerrainCache* const cache = value.fine_terrain_cache.get();
    std::call_once(cache->preparation, [&value, cache] {
        cache->recording_preparation_queries = true;
        cache->prepared_parent_queries = 0U;
        detail::V10FineTerrainParent parent{
            .seed = detail::V10_FINE_TERRAIN_SEED,
            .period_m = detail::V10_FOUNDATION_PERIOD_M,
            .identity_sha256 = fineTerrainParentIdentity(value.source.height_raw),
            .bed = [provider = &value, cache](double const x, double const y) {
                if (cache->recording_preparation_queries)
                {
                    ++cache->prepared_parent_queries;
                }
                return detail::sampleV10FoundationHeight(provider->foundation, x, y);
            },
        };
        cache->terrain = std::make_unique<detail::V10FineTerrain const>(std::move(parent));
        cache->recording_preparation_queries = false;
    });
    return *cache->terrain;
}

std::shared_ptr<river_field_candidate::PreparedRiverField const> prepareRiverBed(
    detail::V10FoundationBasis const& foundation, detail::V10WaterPreparation const& water,
    std::stop_token cancellation)
{
    namespace river = river_field_candidate;
    std::vector<river::RiverReach> reaches;
    std::vector<uint8_t> reach_bytes;
    for (auto const& r : water.refinement.reaches())
    {
        checkCancellation(cancellation);
        reaches.push_back({r.start, r.target, {r.a[0], r.a[1]}, {r.delta[0], r.delta[1]},
            r.grade0, r.grade1, r.discharge_m3_s, r.shore_truncated, r.water_stage0, r.water_stage1});
        appendIdentityBytes(reach_bytes, r.start); appendIdentityBytes(reach_bytes, r.target);
        for (double value : {r.a[0], r.a[1], r.delta[0], r.delta[1], r.grade0, r.grade1, r.discharge_m3_s})
        {
            appendIdentityBytes(reach_bytes, value);
        }
        appendIdentityBytes(reach_bytes, static_cast<uint8_t>(r.shore_truncated));
    }
    std::vector<river::ProtectedContact> contacts;
    std::vector<uint8_t> contact_bytes;
    for (auto const& c : water.refinement.protectedContacts())
    {
        checkCancellation(cancellation);
        contacts.push_back({{c.xy_m[0], c.xy_m[1]}, c.radius_m});
        for (double value : {c.xy_m[0], c.xy_m[1], c.radius_m})
        {
            appendIdentityBytes(contact_bytes, value);
        }
    }
    river::RiverFieldIdentity const identity{
        "s2-river-field-cpp-v2", "c8f9b851f7891e555165648d473b39df1ecffc9b73ae65cf5b14ed9f5ba04703",
        std::string{V10_MODEL_IDENTITY}, "153577ed6506ca7ed457922f32259935cade752092e80cf83d916af9e225249c",
        "v10-seed42-resolution256-native-water-v5", "v10-native-foundation-standing-v1",
        identityHash(reach_bytes), identityHash(contact_bytes), 42U, detail::V10_FOUNDATION_PERIOD_M};
    river::NativeBatchSampler const sampler = [&](std::span<river::WorldXY const> points)
        -> std::expected<std::vector<river::NativePointSample>, river::Failure> {
        std::vector<river::NativePointSample> result;
        result.reserve(points.size());
        for (auto const& point : points)
        {
            checkCancellation(cancellation);
            auto const native = nativeBedSample(foundation, water, {point.x_m, point.y_m});
            result.push_back({point, native.foundation_m, native.standing_stage_m, native.standing_owner});
        }
        return result;
    };
    checkCancellation(cancellation);
    auto field = river::prepareRiverField(identity, reaches, contacts, sampler, {}, {});
    checkCancellation(cancellation);
    if (!field)
    {
        throw V10RiverFieldError(field.error());
    }
    return std::move(*field);
}

double correctedBed(detail::V10ProviderState const& value, double x, double y)
{
    auto const native = nativeBedSample(value.foundation, value.water, {x, y});
    river_field_candidate::NativePointSample const input{
        {x, y}, native.foundation_m, native.standing_stage_m, native.standing_owner};
    auto const result = river_field_candidate::sampleRiverBed(*value.river_field, std::span{&input, 1U});
    if (!result)
    {
        throw V10RiverFieldError(result.error());
    }
    double const height = result->front().bed_m;
    validateHeight(height);
    return height;
}

double smoothUnit(double value)
{
    double const t = std::clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// DONT_CHECK INCLUDE_ORDER: this implementation uses the local terrain helpers above.
#include "V10ReliefGuard.inc"

double fineTerrainBed(detail::V10ProviderState const& value,
    V10NativeBedSample const& native, river_field_candidate::RiverChannelSample const& channel,
    detail::V10FineTerrainLevel const level)
{
    double const support = channel.bed.corridor_support;
    if (!std::isfinite(support) || support < 0.0 || support > 1.0)
    {
        throw std::runtime_error("V10 river field returned invalid corridor support");
    }

    double channel_clearance = 1.0;
    if (!channel.containing_courses.empty())
    {
        channel_clearance = 0.0;
    }
    else if (channel.nearest_course)
    {
        double const distance = channel.nearest_course->centreline_distance_m;
        double const halfwidth = channel.nearest_course->nominal_halfwidth_m;
        if (!std::isfinite(distance) || distance < 0.0 || !std::isfinite(halfwidth) || halfwidth <= 0.0)
        {
            throw std::runtime_error("V10 river field returned invalid channel geometry");
        }
        constexpr double CHANNEL_OPERATOR_END_MULTIPLE = 1.25;
        constexpr double FINE_CLEARANCE_END_MULTIPLE = 2.5;
        double const channel_operator_end = CHANNEL_OPERATOR_END_MULTIPLE * halfwidth;
        double const fine_falloff_end = FINE_CLEARANCE_END_MULTIPLE * halfwidth;
        channel_clearance = smoothUnit((distance - channel_operator_end)
            / (fine_falloff_end - channel_operator_end));
    }

    // This is the same cubic dry permission used by the e37 bed operator.
    double const standing_permission = smoothUnit(native.foundation_m - native.standing_stage_m);
    double const attenuation = standing_permission * channel_clearance * (1.0 - support);
    if (attenuation == 0.0)
    {
        validateHeight(channel.bed.bed_m);
        return channel.bed.bed_m;
    }

    return applyGuardedInlandRelief(value, native, channel, level, attenuation, true);
}

double fineTerrainBedAt(detail::V10ProviderState const& value, double const x, double const y)
{
    auto const native = nativeBedSample(value.foundation, value.water, {x, y});
    river_field_candidate::NativePointSample const input{
        {x, y}, native.foundation_m, native.standing_stage_m, native.standing_owner};
    auto const channel = river_field_candidate::sampleRiverChannel(*value.river_field, std::span{&input, 1U});
    if (!channel)
    {
        throw V10RiverFieldError(channel.error());
    }
    return fineTerrainBed(value, native, channel->front(), detail::V10FineTerrainLevel::L3);
}

double phaseVertexSample(std::span<float const> values, double x, double y, uint32_t phase);

V10WaterGeometrySample surfaceWaterGeometry(detail::V10ProviderState const& value,
    double const x, double const y, uint32_t const lod, bool const use_fine,
    detail::V10FineTerrainLevel const fine_level)
{
    if (lod > 3U)
    {
        throw std::invalid_argument("V10 LOD is unsupported");
    }
    auto const native = nativeBedSample(value.foundation, value.water, {x, y});
    river_field_candidate::NativePointSample const input{
        {x, y}, native.foundation_m, native.standing_stage_m, native.standing_owner};
    auto const channel = river_field_candidate::sampleRiverChannel(*value.river_field, std::span{&input, 1U});
    if (!channel)
    {
        throw V10RiverFieldError(channel.error());
    }
    double const height = use_fine
        ? fineTerrainBed(value, native, channel->front(), fine_level)
        : channel->front().bed.bed_m;
    validateHeight(height);
    auto const water = detail::queryV10WaterPoint(value.water, x, y, height, channel->front().containing_courses);
    if (!water)
    {
        throw std::runtime_error("V10 water point query failed: " + water.error().message);
    }
    return {height, water->water_level_m, water->ocean, water->lake, water->river};
}

std::array<V10SurfaceSample, 8U> surfacePhases(detail::V10ProviderState const& value,
    double const x, double const y, uint32_t const lod, bool const use_fine,
    detail::V10FineTerrainLevel const fine_level)
{
    auto const geometry = surfaceWaterGeometry(value, x, y, lod, use_fine, fine_level);
    auto const climate = detail::queryV10Climate(value.climate.forcing, x, y, geometry.elevation_m,
        {geometry.ocean, geometry.lake, geometry.river});
    std::array<V10SurfaceSample, 8U> samples;
    for (uint32_t phase = 0U; phase < samples.size(); ++phase)
    {
        double const ice = geometry.ocean
            ? phaseVertexSample(value.water.water.sea_ice_thickness_m, x, y, phase) : 0.0;
        samples[phase] = {geometry.elevation_m, geometry.water_level_m, climate.temperature_celsius[phase],
            climate.precipitation_mm_year[phase], climate.snow_water_equivalent_m[phase],
            climate.glacier_thickness_m[phase], ice, climate.biome_id,
            geometry.ocean, geometry.lake, geometry.river};
    }
    return samples;
}

std::shared_ptr<GenerationSnapshot const> snapshot(
    GenerationStage const stage, detail::V10PlatecSourceResult const& source,
    std::span<double const> elevation = {}, detail::V10ClimatePreparation const* climate = nullptr,
    detail::V10WaterPreparation const* water = nullptr)
{
    auto result = std::make_shared<GenerationSnapshot>();
    result->stage = stage;
    result->resolution = 256U;
    auto& fields = result->v10.emplace();
    fields.height_raw = source.height_raw;
    fields.plate_ids = source.current_plate_id;
    fields.plate_directions_observed_after_native_step = source.plate_direction_snapshot.observed_after_native_step;
    for (auto const& direction : source.plate_direction_snapshot.directions)
    {
        fields.plate_directions.push_back({direction.plate_id, direction.x, direction.y});
    }
    fields.source_elevation_m.assign(elevation.begin(), elevation.end());
    if (climate)
    {
        auto const& forcing = climate->forcing.fields();
        fields.local_effective_celsius = forcing.local_effective_celsius;
        fields.ground_m = forcing.ground_m;
        fields.theta_celsius = forcing.theta_celsius;
        fields.phase_precipitation_mm_year = forcing.precipitation_mm_year;
    }
    if (water)
    {
        auto const& native = water->water;
        fields.water_level_m = native.water_level_m;
        fields.ocean_mask = native.equilibrium.ocean;
        fields.lake_mask = native.equilibrium.lake;
        fields.river_mask = native.routing.river;
        fields.sea_ice_thickness_m = native.sea_ice_thickness_m;
        fields.land_snow_water_equivalent_m = native.land_inventory.snow_water_equivalent_m;
        fields.glacier_thickness_m = native.land_inventory.glacier_thickness_m;
    }
    return result;
}

double phaseVertexSample(std::span<float const> values, double x, double y, uint32_t phase)
{
    x = std::fmod(x, 65'536.0);
    y = std::fmod(y, 65'536.0);
    if (x < 0.0)
    {
        x += 65'536.0;
    }
    if (y < 0.0)
    {
        y += 65'536.0;
    }
    x /= 256.0;
    y /= 256.0;
    auto const ix = static_cast<size_t>(std::floor(x));
    auto const iy = static_cast<size_t>(std::floor(y));
    double const u = x - ix;
    double const v = y - iy;
    auto at = [&](size_t xx, size_t yy) { return values[phase * SAMPLE_COUNT + (yy % 256U) * 256U + xx % 256U]; };
    return (1.0 - v) * ((1.0 - u) * at(ix, iy) + u * at(ix + 1U, iy))
        + v * ((1.0 - u) * at(ix, iy + 1U) + u * at(ix + 1U, iy + 1U));
}

template<typename T>
bool finiteSize(std::vector<T> const& values, size_t const expected) noexcept
{
    return values.size() == expected && std::ranges::all_of(values, [](T const value) {
        return std::isfinite(value);
    });
}
} // namespace

namespace detail
{
std::shared_ptr<GlobalAtlas const> prepareV10GlobalAtlas(
    GenerationConfiguration const& configuration, std::stop_token cancellation, ProgressCallback progress)
{
    if (configuration.model_tag != TerrainModelTag::V10 || configuration.seed != 42U
        || configuration.resolution != 256U)
    {
        throw std::invalid_argument("V10 supports only its distinct seed-42, resolution-256 configuration");
    }
    checkCancellation(cancellation);
    auto source = generateV10PlatecSource(configuration.seed, cancellation, [&](V10PlatecProgress const update) {
        if (progress)
        {
            progress({GenerationStage::V10Platec, update.steps_completed,
                update.complete ? update.steps_completed : V10_PLATEC_SOURCE_STEP_LIMIT,
                update.snapshot ? snapshot(GenerationStage::V10Platec, *update.snapshot) : nullptr});
        }
    });
    if (!source)
    {
        if (source.error() == V10PlatecFailure::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error("V10 Platec source step limit exceeded");
    }
    auto foundation = prepareV10Foundation(42U, source->height_raw);
    std::vector<double> l0(SAMPLE_COUNT);
    for (size_t y = 0U; y < 256U; ++y)
    {
        checkCancellation(cancellation);
        for (size_t x = 0U; x < 256U; ++x)
        {
            double const height = sampleV10FoundationHeight(foundation, x * 256.0, y * 256.0);
            validateHeight(height);
            l0[y * 256U + x] = height;
        }
    }
    if (progress)
    {
        progress({GenerationStage::V10Terrain, 1U, 1U,
            snapshot(GenerationStage::V10Terrain, *source, l0)});
    }
    constexpr uint32_t CLIMATE_SUBSTAGE_UNITS = 1'000U;
    constexpr uint32_t CLIMATE_TOTAL_UNITS = 3U * CLIMATE_SUBSTAGE_UNITS;
    auto climate = prepareV10Climate(l0, configuration.seed, cancellation, [&](V9ClimateProgress const update) {
        if (!progress)
        {
            return;
        }
        uint32_t const substage = static_cast<uint32_t>(update.stage);
        uint32_t const fraction = update.total == 0U ? 0U : static_cast<uint32_t>(
            static_cast<uint64_t>(std::min(update.completed, update.total)) * CLIMATE_SUBSTAGE_UNITS / update.total);
        uint32_t const completed = substage >= 3U ? CLIMATE_TOTAL_UNITS
            : substage * CLIMATE_SUBSTAGE_UNITS + fraction;
        progress({GenerationStage::V10Climate, completed, CLIMATE_TOTAL_UNITS, nullptr});
    });
    if (!climate)
    {
        if (climate.error() == V9ClimateFailure::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error("V10 fresh climate preparation failed");
    }
    checkCancellation(cancellation);
    if (progress)
    {
        progress({GenerationStage::V10Climate, CLIMATE_TOTAL_UNITS, CLIMATE_TOTAL_UNITS,
            snapshot(GenerationStage::V10Climate, *source, l0, &*climate)});
    }
    checkCancellation(cancellation);
    auto water = prepareV10Water(l0, climate->climate, foundation, cancellation, [&](V9WaterProgress const update) {
        if (progress)
        {
            progress({GenerationStage::V10Water, update.completed_items,
                static_cast<uint32_t>(V10_CLIMATE_PHASE_COUNT), nullptr});
        }
    });
    if (!water)
    {
        if (water.error().kind == V9WaterFailure::Kind::Cancelled)
        {
            throw GenerationCancelled{};
        }
        throw std::runtime_error("V10 water preparation failed: " + water.error().message);
    }
    checkCancellation(cancellation);
    if (progress)
    {
        progress({GenerationStage::V10Water, static_cast<uint32_t>(V10_CLIMATE_PHASE_COUNT),
            static_cast<uint32_t>(V10_CLIMATE_PHASE_COUNT),
            snapshot(GenerationStage::V10Water, *source, l0, &*climate, &*water)});
    }
    checkCancellation(cancellation);
    auto river_field = prepareRiverBed(foundation, *water, cancellation);
    checkCancellation(cancellation);
    auto atlas = std::make_shared<GlobalAtlas>();
    atlas->configuration = configuration;
    atlas->block_sea_level = V10_BLOCK_SEA_LEVEL;
    atlas->max_height = V10_MAX_HEIGHT;
    atlas->vertical_scale = 1.0;
    std::vector<GenerationPlateDirection> directions;
    for (auto const& direction : source->plate_direction_snapshot.directions)
    {
        directions.push_back({direction.plate_id, direction.x, direction.y});
    }
    auto provider_state = std::make_shared<V10ProviderState>(V10ProviderState{
        std::move(*source), std::move(foundation), std::move(l0), std::move(*climate),
        std::move(*water), std::move(directions), std::move(river_field),
        configuration.use_v10_fine_terrain,
        std::make_unique<V10FineTerrainCache>()});
    checkCancellation(cancellation);
    atlas->v10_provider = std::move(provider_state);
    if (progress)
    {
        progress({GenerationStage::Complete, 1U, 1U, nullptr});
    }
    checkCancellation(cancellation);
    return atlas;
}

bool isValidV10Snapshot(GenerationSnapshot const& value) noexcept
{
    if (!value.v10 || value.v9 || value.v21 || value.resolution != 256U
        || value.stage < GenerationStage::V10Platec || value.stage > GenerationStage::V10Water
        || !value.elevation.empty() || !value.temperature.empty() || !value.precipitation.empty()
        || !value.water_level.empty() || !value.ocean_mask.empty() || !value.lake_mask.empty()
        || !value.river_mask.empty())
    {
        return false;
    }
    auto const& fields = *value.v10;
    bool const terrain = value.stage >= GenerationStage::V10Terrain;
    bool const climate = value.stage >= GenerationStage::V10Climate;
    bool const water = value.stage == GenerationStage::V10Water;
    auto binary = [](std::vector<uint8_t> const& values) {
        return std::ranges::all_of(values, [](uint8_t value) { return value <= 1U; });
    };
    auto nonnegative = [](std::vector<float> const& values) {
        return std::ranges::all_of(values, [](float value) { return value >= 0.0F; });
    };
    return finiteSize(fields.height_raw, SAMPLE_COUNT) && fields.plate_ids.size() == SAMPLE_COUNT
        && std::ranges::all_of(fields.height_raw, [](float const height) { return height > 0.0F; })
        && !fields.plate_directions.empty() && fields.plate_directions.size() <= 10U
        && std::ranges::all_of(fields.plate_directions, [](GenerationPlateDirection const& direction) {
            return direction.plate_id < 10U && std::isfinite(direction.x) && std::isfinite(direction.y);
        })
        && std::ranges::all_of(fields.plate_ids, [&fields](uint32_t const plate) {
            return std::ranges::any_of(fields.plate_directions, [plate](GenerationPlateDirection const& direction) {
                return direction.plate_id == plate;
            });
        })
        && finiteSize(fields.source_elevation_m, terrain ? SAMPLE_COUNT : 0U)
        && std::ranges::all_of(fields.source_elevation_m, [](double height) {
            return height >= -512.0 && height < 1536.0;
        })
        && finiteSize(fields.local_effective_celsius, climate ? 8U * 32U * 32U : 0U)
        && finiteSize(fields.ground_m, climate ? 32U * 32U : 0U)
        && finiteSize(fields.theta_celsius, climate ? 8U * 32U * 32U : 0U)
        && finiteSize(fields.phase_precipitation_mm_year, climate ? 8U * SAMPLE_COUNT : 0U)
        && finiteSize(fields.water_level_m, water ? SAMPLE_COUNT : 0U)
        && fields.ocean_mask.size() == (water ? SAMPLE_COUNT : 0U)
        && fields.lake_mask.size() == (water ? SAMPLE_COUNT : 0U)
        && fields.river_mask.size() == (water ? SAMPLE_COUNT : 0U)
        && finiteSize(fields.sea_ice_thickness_m, water ? 8U * SAMPLE_COUNT : 0U)
        && finiteSize(fields.land_snow_water_equivalent_m, water ? 8U * SAMPLE_COUNT : 0U)
        && finiteSize(fields.glacier_thickness_m, water ? 8U * SAMPLE_COUNT : 0U)
        && binary(fields.ocean_mask) && binary(fields.lake_mask) && binary(fields.river_mask)
        && nonnegative(fields.sea_ice_thickness_m) && nonnegative(fields.land_snow_water_equivalent_m)
        && nonnegative(fields.glacier_thickness_m);
}

bool isValidV10Atlas(GlobalAtlas const& atlas) noexcept
{
    if (atlas.configuration.model_tag != TerrainModelTag::V10 || atlas.configuration.seed != 42U
        || atlas.configuration.resolution != 256U || !atlas.v10_provider || atlas.v9_provider
        || atlas.v21_provider || atlas.block_sea_level != V10_BLOCK_SEA_LEVEL
        || atlas.max_height != V10_MAX_HEIGHT || atlas.vertical_scale != 1.0 || atlas.sea_level != 0.0F
        || !atlas.elevation.empty() || !atlas.temperature.empty() || !atlas.precipitation.empty())
    {
        return false;
    }
    auto const& value = *atlas.v10_provider;
    return value.source.height_raw.size() == SAMPLE_COUNT
        && value.use_v10_fine_terrain == atlas.configuration.use_v10_fine_terrain
        && value.source.current_plate_id.size() == SAMPLE_COUNT
        && value.foundation.land_magnitude_m.size() == SAMPLE_COUNT
        && value.foundation.ocean_magnitude_m.size() == SAMPLE_COUNT
        && value.foundation.coast_distance_m.size() == SAMPLE_COUNT
        && value.foundation.regional_relief_m.size() == SAMPLE_COUNT
        && value.canonical_l0_m.size() == SAMPLE_COUNT
        && value.water.water.phase_count == 8U
        && value.water.water.equilibrium.resolution == 256U
        && value.water.water.sea_ice_thickness_m.size() == 8U * SAMPLE_COUNT
        && value.water.fixed_support.size == 512U
        && !value.plate_directions.empty() && value.river_field && value.fine_terrain_cache;
}
} // namespace detail

std::vector<V9RegisteredFieldView> v10RegisteredFields(GlobalAtlas const& atlas)
{
    auto const& value = state(atlas);
    std::vector<V9RegisteredFieldView> fields;
    fields.push_back({"height_raw", V9FieldType::Float32, V9FieldRegistration::PeriodicVertex,
        {256U, 256U}, std::span<float const>{value.source.height_raw}});
    fields.push_back({"height_m", V9FieldType::Float64, V9FieldRegistration::PeriodicVertex,
        {256U, 256U}, std::span<double const>{value.canonical_l0_m}});
    fields.push_back({"plate_id", V9FieldType::UInt32, V9FieldRegistration::PeriodicVertex,
        {256U, 256U}, std::span<uint32_t const>{value.source.current_plate_id}});
    for (auto const& [name, field] : value.climate.climate.fields)
    {
        fields.push_back({name, V9FieldType::Float64, V9FieldRegistration::Native,
            field.shape, std::span<double const>{field.values}});
    }
    for (auto const& field : detail::v9WaterNativeFields(value.water.water))
    {
        V9RegisteredFieldView view;
        view.name = field.name;
        view.registration = V9FieldRegistration::Native;
        view.shape.assign(field.shape.begin(), field.shape.end());
        auto typed = [&]<typename T>() {
            return std::span<T const>{reinterpret_cast<T const*>(field.bytes.data()), field.bytes.size() / sizeof(T)};
        };
        switch (field.dtype)
        {
        case detail::V9WaterFieldType::Bool:
            view.type = V9FieldType::Bool; view.values = typed.template operator()<uint8_t>(); break;
        case detail::V9WaterFieldType::UInt8:
            view.type = V9FieldType::UInt8; view.values = typed.template operator()<uint8_t>(); break;
        case detail::V9WaterFieldType::Int32:
            view.type = V9FieldType::Int32; view.values = typed.template operator()<int32_t>(); break;
        case detail::V9WaterFieldType::Int64:
            view.type = V9FieldType::Int64; view.values = typed.template operator()<int64_t>(); break;
        case detail::V9WaterFieldType::Float32:
            view.type = V9FieldType::Float32; view.values = typed.template operator()<float>(); break;
        case detail::V9WaterFieldType::Float64:
            view.type = V9FieldType::Float64; view.values = typed.template operator()<double>(); break;
        }
        fields.push_back(std::move(view));
    }
    auto const& forcing = value.climate.forcing.fields();
    fields.push_back({"fine_local_effective_C", V9FieldType::Float64, V9FieldRegistration::PeriodicCellCentre,
        {8U, 32U, 32U}, std::span<double const>{forcing.local_effective_celsius}});
    fields.push_back({"fine_ground_m", V9FieldType::Float64, V9FieldRegistration::PeriodicCellCentre,
        {32U, 32U}, std::span<double const>{forcing.ground_m}});
    fields.push_back({"fine_theta_C", V9FieldType::Float64, V9FieldRegistration::PeriodicCellCentre,
        {8U, 32U, 32U}, std::span<double const>{forcing.theta_celsius}});
    fields.push_back({"fine_precipitation_mm_year", V9FieldType::Float64, V9FieldRegistration::PeriodicVertex,
        {8U, 256U, 256U}, std::span<double const>{forcing.precipitation_mm_year}});
    return fields;
}

std::span<uint32_t const> v10CurrentPlateIds(GlobalAtlas const& atlas)
{
    return state(atlas).source.current_plate_id;
}

std::span<GenerationPlateDirection const> v10PlateDirections(GlobalAtlas const& atlas)
{
    return state(atlas).plate_directions;
}

std::vector<V10NativeBedSample> queryV10NativeBedSamples(GlobalAtlas const& atlas,
    std::span<std::array<double, 2U> const> xy_m, std::stop_token cancellation)
{
    checkCancellation(cancellation);
    auto const& value = state(atlas);
    std::vector<V10NativeBedSample> result;
    result.reserve(xy_m.size());
    for (auto const& xy : xy_m)
    {
        checkCancellation(cancellation);
        result.push_back(nativeBedSample(value.foundation, value.water, xy));
    }
    return result;
}

std::shared_ptr<river_field_candidate::PreparedRiverField const> v10RiverBedField(GlobalAtlas const& atlas)
{
    return state(atlas).river_field;
}

double v10L0ElevationAt(GlobalAtlas const& atlas, double const x, double const y)
{
    auto const& value = state(atlas);
    return value.use_v10_fine_terrain ? fineTerrainBedAt(value, x, y) : correctedBed(value, x, y);
}

V10WaterGeometrySample sampleV10WaterGeometry(GlobalAtlas const& atlas,
    double const x, double const y, uint32_t const lod)
{
    auto const& value = state(atlas);
    return surfaceWaterGeometry(value, x, y, lod, value.use_v10_fine_terrain,
        detail::V10FineTerrainLevel::L3);
}

std::array<V10SurfaceSample, 8U> sampleV10SurfacePhases(GlobalAtlas const& atlas,
    double const x, double const y, uint32_t const lod)
{
    auto const& value = state(atlas);
    return surfacePhases(value, x, y, lod, value.use_v10_fine_terrain,
        detail::V10FineTerrainLevel::L3);
}

V10FineTerrainMetadata v10FineTerrainMetadata(GlobalAtlas const& atlas)
{
    auto const& receipt = fineTerrain(state(atlas)).receipt();
    return {
        .source_id = receipt.source_id,
        .parent_identity_sha256 = receipt.parent_identity_sha256,
        .field_identity_sha256 = receipt.field_identity_sha256,
        .l2_identity_sha256 = receipt.l2_identity_sha256,
        .l3_identity_sha256 = receipt.l3_identity_sha256,
        .plan_sha256 = receipt.plan_sha256,
        .prepared_parent_queries = receipt.prepared_parent_queries,
        .mountain_site_count = receipt.mountain_site_count,
        .valley_network_count = receipt.valley_network_count,
        .regional_site_count = receipt.regional_site_count,
        .regional_draws_sha256 = receipt.regional_draws_sha256,
        .regional_feature_sha256 = receipt.regional_feature_sha256,
    };
}

std::array<V10SurfaceSample, 8U> sampleV10FineTerrainSurfacePhases(GlobalAtlas const& atlas,
    double const x, double const y, uint32_t const lod)
{
    detail::V10FineTerrainLevel level = detail::V10FineTerrainLevel::L2;
    switch (lod)
    {
    case 2U: level = detail::V10FineTerrainLevel::L2; break;
    case 3U: level = detail::V10FineTerrainLevel::L3; break;
    default: throw std::invalid_argument("V10 fine terrain supports only LOD 2 and 3");
    }
    return surfacePhases(state(atlas), x, y, lod, true, level);
}

V10SurfaceSample sampleV10FineTerrainSurface(GlobalAtlas const& atlas, double const x, double const y,
    uint32_t const lod, uint32_t const selected_phase)
{
    if (selected_phase >= 8U)
    {
        throw std::invalid_argument("V10 phase is unsupported");
    }
    return sampleV10FineTerrainSurfacePhases(atlas, x, y, lod)[selected_phase];
}

V10SurfaceSample sampleV10AnnualMeanSurface(GlobalAtlas const& atlas, double const x, double const y,
    uint32_t const lod)
{
    auto const phases = sampleV10SurfacePhases(atlas, x, y, lod);
    V10SurfaceSample result = phases.front();
    result.temperature_celsius = 0.0;
    result.precipitation_mm_year = 0.0;
    result.snow_water_equivalent_m = 0.0;
    result.glacier_thickness_m = 0.0;
    result.sea_ice_thickness_m = 0.0;
    for (auto const& phase : phases)
    {
        result.temperature_celsius += phase.temperature_celsius;
        result.precipitation_mm_year += phase.precipitation_mm_year;
        result.snow_water_equivalent_m += phase.snow_water_equivalent_m;
        result.glacier_thickness_m += phase.glacier_thickness_m;
        result.sea_ice_thickness_m += phase.sea_ice_thickness_m;
    }
    double const phase_count = static_cast<double>(phases.size());
    result.temperature_celsius /= phase_count;
    result.precipitation_mm_year /= phase_count;
    result.snow_water_equivalent_m /= phase_count;
    result.glacier_thickness_m /= phase_count;
    result.sea_ice_thickness_m /= phase_count;
    return result;
}

V10SurfaceCover classifyV10AnnualMeanSurfaceCover(V10SurfaceSample const& sample) noexcept
{
    constexpr double BULK_COVER_STOCK_THRESHOLD_M = 0.1;
    if (sample.ocean) {
        return sample.sea_ice_thickness_m >= BULK_COVER_STOCK_THRESHOLD_M
            ? V10SurfaceCover::SeaIce
            : V10SurfaceCover::None;
    }
    if (sample.lake || sample.river) {
        return V10SurfaceCover::None;
    }
    if (sample.snow_water_equivalent_m >= BULK_COVER_STOCK_THRESHOLD_M
        || (sample.temperature_celsius < 0.0 && sample.snow_water_equivalent_m > 0.0)) {
        return V10SurfaceCover::Snow;
    }
    if (sample.glacier_thickness_m >= BULK_COVER_STOCK_THRESHOLD_M) {
        return V10SurfaceCover::GlacierIce;
    }
    return V10SurfaceCover::None;
}

V10SurfaceSample sampleV10Surface(GlobalAtlas const& atlas, double const x, double const y,
    uint32_t const lod, uint32_t const selected_phase)
{
    if (selected_phase >= 8U)
    {
        throw std::invalid_argument("V10 phase is unsupported");
    }
    return sampleV10SurfacePhases(atlas, x, y, lod)[selected_phase];
}

std::vector<V10SurfaceSample> queryV10SurfaceGrid(GlobalAtlas const& atlas,
    V10SurfaceQueryGrid const& grid, std::stop_token cancellation)
{
    if (grid.rows == 0U || grid.columns == 0U || static_cast<uint64_t>(grid.rows) * grid.columns > 1'048'576U
        || !std::isfinite(grid.origin_x_m) || !std::isfinite(grid.origin_y_m)
        || !std::isfinite(grid.spacing_m) || grid.spacing_m <= 0.0 || grid.lod > 3U || grid.selected_phase >= 8U)
    {
        throw std::invalid_argument("V10 query grid is invalid");
    }
    checkCancellation(cancellation);
    std::vector<V10SurfaceSample> result;
    result.reserve(static_cast<size_t>(grid.rows) * grid.columns);
    for (uint32_t y = 0U; y < grid.rows; ++y)
    {
        checkCancellation(cancellation);
        for (uint32_t x = 0U; x < grid.columns; ++x)
        {
            result.push_back(sampleV10Surface(atlas, grid.origin_x_m + x * grid.spacing_m,
                grid.origin_y_m + y * grid.spacing_m, grid.lod, grid.selected_phase));
        }
    }
    return result;
}

uint16_t mapV10ElevationToBlock(double const source_elevation_m)
{
    validateHeight(source_elevation_m);
    return static_cast<uint16_t>(std::floor(V10_BLOCK_SEA_LEVEL + source_elevation_m));
}
} // namespace generation
