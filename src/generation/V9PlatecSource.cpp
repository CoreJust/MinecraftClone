#include "V9PlatecSource.hpp"

#include "Sha256.hpp"

#include <lithosphere.hpp>
#include <plate.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace generation::detail
{
namespace
{

static constexpr std::string_view MODEL_NAMESPACE = "world7-pyplatec-topology-mixture-candidate-v1";
static constexpr std::string_view NATIVE_SEED_NAMESPACE = "world7-pyplatec-default-v1";
static constexpr std::string_view TOPOLOGY_DOMAIN = "topology-regime";
static constexpr std::string_view NATIVE_SEED_DOMAIN = "native-engine-seed32";
static constexpr uint64_t SUPERCONTINENT_CUTOFF = 1'844'674'407'370'955'161ULL;
static constexpr uint64_t MIXED_LANDMASS_CUTOFF = 5'534'023'222'112'865'484ULL;
static constexpr uint32_t PLATEC_EROSION_PERIOD = 60U;
static constexpr float PLATEC_FOLDING_RATIO = 0.02F;
static constexpr uint32_t PLATEC_AGGREGATION_OVERLAP_ABSOLUTE = 1'000'000U;
static constexpr float PLATEC_AGGREGATION_OVERLAP_RELATIVE = 0.33F;
static constexpr uint32_t PLATEC_CYCLE_COUNT = 1U;

void appendAscii(std::vector<uint8_t>& bytes, std::string_view text)
{
    for (char const character : text)
    {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]]
std::vector<uint8_t> seedMaterial(
    std::string_view name_space,
    uint64_t user_seed,
    std::string_view domain
)
{
    std::array<char, 20U> seed_buffer{};
    std::to_chars_result const seed_text_result = std::to_chars(
        seed_buffer.data(),
        seed_buffer.data() + seed_buffer.size(),
        user_seed
    );
    if (seed_text_result.ec != std::errc{})
    {
        throw std::runtime_error{"Could not render the full uint64 seed as decimal ASCII"};
    }

    std::string_view const seed_text{
        seed_buffer.data(),
        static_cast<std::string_view::size_type>(seed_text_result.ptr - seed_buffer.data()),
    };
    std::vector<uint8_t> bytes;
    bytes.reserve(name_space.size() + seed_text.size() + domain.size() + 4U);
    appendAscii(bytes, name_space);
    bytes.push_back(0U);
    appendAscii(bytes, seed_text);
    bytes.push_back(0U);
    appendAscii(bytes, domain);
    bytes.push_back(0U);
    bytes.push_back(static_cast<uint8_t>('0'));
    return bytes;
}

[[nodiscard]]
uint64_t bigEndianPrefix64(std::array<uint8_t, 32U> const& digest)
{
    uint64_t value = 0U;
    for (uint64_t index = 0U; index < 8U; ++index)
    {
        value = (value << 8U) | digest[static_cast<std::array<uint8_t, 32U>::size_type>(index)];
    }
    return value;
}

[[nodiscard]]
uint32_t bigEndianPrefix32(std::array<uint8_t, 32U> const& digest)
{
    uint32_t value = 0U;
    for (uint64_t index = 0U; index < 4U; ++index)
    {
        value = (value << 8U) | digest[static_cast<std::array<uint8_t, 32U>::size_type>(index)];
    }
    return value;
}

[[nodiscard]]
std::expected<V9PlatecSourceResult, V9PlatecFailure> cancelled()
{
    return std::unexpected{V9PlatecFailure::Cancelled};
}

} // namespace

V9PlatecParameters deriveV9PlatecParameters(uint64_t user_seed)
{
    std::vector<uint8_t> const selection_material = seedMaterial(
        MODEL_NAMESPACE,
        user_seed,
        TOPOLOGY_DOMAIN
    );
    std::vector<uint8_t> const native_seed_material = seedMaterial(
        NATIVE_SEED_NAMESPACE,
        user_seed,
        NATIVE_SEED_DOMAIN
    );
    uint64_t const selection_value = bigEndianPrefix64(sha256Digest(selection_material));
    uint32_t const native_seed32 = bigEndianPrefix32(sha256Digest(native_seed_material));

    if (selection_value < SUPERCONTINENT_CUTOFF)
    {
        return {
            .regime = V9TopologyRegime::SupercontinentOriented,
            .topology_selection_value = selection_value,
            .native_engine_seed32 = native_seed32,
            .initial_ocean_fraction = 0.75F,
            .initial_plate_count = 6U,
        };
    }
    if (selection_value < MIXED_LANDMASS_CUTOFF)
    {
        return {
            .regime = V9TopologyRegime::MixedLandmass,
            .topology_selection_value = selection_value,
            .native_engine_seed32 = native_seed32,
            .initial_ocean_fraction = 0.75F,
            .initial_plate_count = 24U,
        };
    }
    return {
        .regime = V9TopologyRegime::FragmentedLandforms,
        .topology_selection_value = selection_value,
        .native_engine_seed32 = native_seed32,
        .initial_ocean_fraction = 0.90F,
        .initial_plate_count = 24U,
    };
}

std::expected<V9PlatecSourceResult, V9PlatecFailure> generateV9PlatecSource(
    uint64_t user_seed,
    std::stop_token stop_token,
    V9PlatecProgressCallback progress_callback
)
{
    if (stop_token.stop_requested())
    {
        return cancelled();
    }

    V9PlatecParameters const parameters = deriveV9PlatecParameters(user_seed);
    lithosphere engine{
        static_cast<long>(parameters.native_engine_seed32),
        V9_PLATEC_SOURCE_RESOLUTION,
        V9_PLATEC_SOURCE_RESOLUTION,
        parameters.initial_ocean_fraction,
        PLATEC_EROSION_PERIOD,
        PLATEC_FOLDING_RATIO,
        PLATEC_AGGREGATION_OVERLAP_ABSOLUTE,
        PLATEC_AGGREGATION_OVERLAP_RELATIVE,
        PLATEC_CYCLE_COUNT,
        parameters.initial_plate_count,
    };

    uint32_t steps_completed = 0U;
    if (progress_callback)
    {
        progress_callback({
            .steps_completed = steps_completed,
            .step_limit = V9_PLATEC_SOURCE_STEP_LIMIT,
            .complete = engine.isFinished(),
        });
    }
    // The terminal Platec restart deletes its plates but leaves the ownership map in place.
    V9PlateDirectionSnapshot final_plate_direction_snapshot;
    while (!engine.isFinished())
    {
        if (stop_token.stop_requested())
        {
            return cancelled();
        }
        if (steps_completed == V9_PLATEC_SOURCE_STEP_LIMIT)
        {
            return std::unexpected{V9PlatecFailure::StepLimitExceeded};
        }

        uint32_t const current_plate_count = engine.getPlateCount();
        final_plate_direction_snapshot.observed_after_native_step = steps_completed;
        std::vector<V9PlateDirection>& directions = final_plate_direction_snapshot.directions;
        directions.clear();
        directions.reserve(current_plate_count);
        for (uint32_t plate_id = 0U; plate_id < current_plate_count; ++plate_id)
        {
            plate const* current_plate = engine.getPlate(plate_id);
            if (current_plate == nullptr)
            {
                throw std::runtime_error{"Platec returned an absent current plate"};
            }
            Platec::FloatVector const direction = current_plate->velocityUnitVector();
            if (!std::isfinite(direction.x()) || !std::isfinite(direction.y()))
            {
                throw std::runtime_error{"Platec returned a non-finite current plate direction"};
            }
            directions.push_back({
                .plate_id = plate_id,
                .x = direction.x(),
                .y = direction.y(),
            });
        }

        engine.update();
        ++steps_completed;
        if (progress_callback)
        {
            progress_callback({
                .steps_completed = steps_completed,
                .step_limit = V9_PLATEC_SOURCE_STEP_LIMIT,
                .complete = engine.isFinished(),
            });
        }
    }

    if (engine.getWidth() != V9_PLATEC_SOURCE_RESOLUTION
        || engine.getHeight() != V9_PLATEC_SOURCE_RESOLUTION)
    {
        throw std::runtime_error{"Platec returned dimensions outside the v9 256-square source stage"};
    }
    float const* height_raw = engine.getTopography();
    uint32_t const* current_plate_id = engine.getPlatesMap();
    if (height_raw == nullptr || current_plate_id == nullptr)
    {
        throw std::runtime_error{"Platec returned an absent terminal height or plate map"};
    }

    uint32_t const sample_count = V9_PLATEC_SOURCE_RESOLUTION * V9_PLATEC_SOURCE_RESOLUTION;
    return V9PlatecSourceResult{
        .parameters = parameters,
        .final_step = static_cast<int32_t>(steps_completed),
        .height_raw = std::vector<float>(height_raw, height_raw + sample_count),
        .current_plate_id = std::vector<uint32_t>(current_plate_id, current_plate_id + sample_count),
        .plate_direction_snapshot = std::move(final_plate_direction_snapshot),
    };
}

} // namespace generation::detail
