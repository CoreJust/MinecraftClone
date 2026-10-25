#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>
#include <vector>

namespace generation::detail
{

inline constexpr uint32_t V9_PLATEC_SOURCE_RESOLUTION = 256U;
inline constexpr uint32_t V9_PLATEC_SOURCE_STEP_LIMIT = 2'400U;

enum class V9TopologyRegime : uint8_t
{
    SupercontinentOriented,
    MixedLandmass,
    FragmentedLandforms,
};

struct V9PlatecParameters final
{
    V9TopologyRegime regime = V9TopologyRegime::FragmentedLandforms;
    uint64_t topology_selection_value = 0U;
    uint32_t native_engine_seed32 = 0U;
    float initial_ocean_fraction = 0.0F;
    uint32_t initial_plate_count = 0U;
};

struct V9PlateDirection final
{
    uint32_t plate_id = 0U;
    float x = 0.0F;
    float y = 0.0F;
};

enum class V9PlateDirectionSnapshotPhase : uint8_t
{
    ActiveBeforeTerminalRestart,
};

struct V9PlateDirectionSnapshot final
{
    uint32_t observed_after_native_step = 0U;
    V9PlateDirectionSnapshotPhase phase =
        V9PlateDirectionSnapshotPhase::ActiveBeforeTerminalRestart;
    std::vector<V9PlateDirection> directions;
};

struct V9PlatecSourceResult final
{
    V9PlatecParameters parameters;
    int32_t final_step = 0;
    std::vector<float> height_raw;
    std::vector<uint32_t> current_plate_id;
    V9PlateDirectionSnapshot plate_direction_snapshot;
};

struct V9PlatecProgress final
{
    uint32_t steps_completed = 0U;
    uint32_t step_limit = V9_PLATEC_SOURCE_STEP_LIMIT;
    bool complete = false;
};

enum class V9PlatecFailure : uint8_t
{
    Cancelled,
    StepLimitExceeded,
};

using V9PlatecProgressCallback = std::function<void(V9PlatecProgress)>;

[[nodiscard]]
V9PlatecParameters deriveV9PlatecParameters(uint64_t user_seed);

// Cancellation is observed between native updates. Progress reports completed updates
// against the frozen step bound, with completion carried separately from the ratio.
[[nodiscard]]
std::expected<V9PlatecSourceResult, V9PlatecFailure> generateV9PlatecSource(
    uint64_t user_seed,
    std::stop_token stop_token = {},
    V9PlatecProgressCallback progress_callback = {}
);

} // namespace generation::detail
