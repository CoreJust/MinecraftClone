#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace generation::detail
{

inline constexpr uint32_t V10_PLATEC_SOURCE_RESOLUTION = 256U;
inline constexpr uint32_t V10_PLATEC_SOURCE_STEP_LIMIT = 2400U;

struct V10PlateDirection final
{
    uint32_t plate_id = 0U;
    float x = 0.0F;
    float y = 0.0F;
};

enum class V10PlateDirectionSnapshotPhase : uint8_t
{
    ActiveBeforeTerminalRestart,
};

struct V10PlateDirectionSnapshot final
{
    uint32_t observed_after_native_step = 0U;
    V10PlateDirectionSnapshotPhase phase =
        V10PlateDirectionSnapshotPhase::ActiveBeforeTerminalRestart;
    std::vector<V10PlateDirection> directions;
};

struct V10PlatecSourceResult final
{
    int32_t final_step = 0;
    std::vector<float> height_raw;
    std::vector<uint32_t> current_plate_id;
    V10PlateDirectionSnapshot plate_direction_snapshot;
};

struct V10PlatecProgress final
{
    uint32_t steps_completed = 0U;
    bool complete = false;
    std::shared_ptr<V10PlatecSourceResult const> snapshot;
};

enum class V10PlatecFailure : uint8_t
{
    Cancelled,
    StepLimitExceeded,
};

using V10PlatecProgressCallback = std::function<void(V10PlatecProgress)>;

/*
 * Direct engine seed 42, side 256, sea fraction .65, 10 plates, two cycles.
 * Rejects other user seeds with invalid_argument. No seed hashing or topology
 * mixture. Cancellation returns no partial result. Snapshots are owned copies.
 * Native operations within this engine family serialize. The exact PyPlatec
 * 1.4.3 engine is namespace-isolated from V9, including upstream scratch. The
 * fixed reference raw-array and mountain gates establish this source contract.
**/
[[nodiscard]]
std::expected<V10PlatecSourceResult, V10PlatecFailure> generateV10PlatecSource(
    uint64_t user_seed,
    std::stop_token stop_token = {},
    V10PlatecProgressCallback progress_callback = {}
);

} // namespace generation::detail
