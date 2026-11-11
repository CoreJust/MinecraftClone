#include "V10PlatecSource.hpp"

#include "../../third_party/platec_v10/bridge.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace generation::detail
{
namespace
{
std::mutex s_v10_platec_mutex;
constexpr uint32_t COUNT = V10_PLATEC_SOURCE_RESOLUTION * V10_PLATEC_SOURCE_RESOLUTION;

V10PlatecSourceResult capture(
    mc_v10_platec_handle const* engine, uint32_t steps, V10PlateDirectionSnapshot const& motion
)
{
    float const* heights = mc_v10_platec_heights(engine);
    uint32_t const* plates = mc_v10_platec_plates(engine);
    if (heights == nullptr || plates == nullptr)
    {
        throw std::runtime_error("V10 Platec returned an absent source map");
    }
    return {
        .final_step = static_cast<int32_t>(steps),
        .height_raw = std::vector<float>(heights, heights + COUNT),
        .current_plate_id = std::vector<uint32_t>(plates, plates + COUNT),
        .plate_direction_snapshot = motion,
    };
}

} // namespace

std::expected<V10PlatecSourceResult, V10PlatecFailure> generateV10PlatecSource(
    uint64_t user_seed, std::stop_token stop_token, V10PlatecProgressCallback progress_callback
)
{
    if (user_seed != 42U)
    {
        throw std::invalid_argument("V10 Platec source requires seed 42");
    }
    if (stop_token.stop_requested())
    {
        return std::unexpected{V10PlatecFailure::Cancelled};
    }
    std::unique_lock source_lock{s_v10_platec_mutex};
    if (stop_token.stop_requested())
    {
        return std::unexpected{V10PlatecFailure::Cancelled};
    }
    // Frozen PlatecWorldV10 -> V9 -> signed-source factory parameters. The
    // engine seed is direct, deliberately independent of the V9 seed bridge.
    std::unique_ptr<mc_v10_platec_handle, decltype(&mc_v10_platec_destroy)> engine{
        mc_v10_platec_create(), mc_v10_platec_destroy,
    };
    uint32_t steps = 0U;
    V10PlateDirectionSnapshot motion;
    auto capture_directions = [&] {
        uint32_t const count = mc_v10_platec_copy_directions(engine.get(), nullptr, 0U);
        std::vector<mc_v10_platec_direction> native(count);
        static_cast<void>(mc_v10_platec_copy_directions(engine.get(), native.data(), count));
        motion.observed_after_native_step = steps;
        motion.directions.clear();
        motion.directions.reserve(count);
        for (auto const& direction : native)
        {
            if (!std::isfinite(direction.x) || !std::isfinite(direction.y))
            {
                throw std::runtime_error("V10 Platec returned a non-finite current plate direction");
            }
            motion.directions.push_back({direction.plate_id, direction.x, direction.y});
        }
    };
    capture_directions();
    auto last_snapshot_at = std::chrono::steady_clock::time_point{};
    auto publish = [&] {
        if (!progress_callback)
        {
            return;
        }
        auto const now = std::chrono::steady_clock::now();
        std::shared_ptr<V10PlatecSourceResult const> snapshot;
        if (steps == 0U || mc_v10_platec_is_finished(engine.get()) != 0
            || now - last_snapshot_at >= std::chrono::milliseconds{250})
        {
            snapshot = std::make_shared<V10PlatecSourceResult const>(capture(engine.get(), steps, motion));
            last_snapshot_at = now;
        }
        V10PlatecProgress const update{steps, mc_v10_platec_is_finished(engine.get()) != 0, std::move(snapshot)};
        source_lock.unlock();
        try
        {
            progress_callback(update);
        }
        catch (...)
        {
            source_lock.lock();
            throw;
        }
        source_lock.lock();
    };
    publish();
    while (mc_v10_platec_is_finished(engine.get()) == 0)
    {
        if (stop_token.stop_requested())
        {
            return std::unexpected{V10PlatecFailure::Cancelled};
        }
        if (steps == V10_PLATEC_SOURCE_STEP_LIMIT)
        {
            return std::unexpected{V10PlatecFailure::StepLimitExceeded};
        }
        mc_v10_platec_update(engine.get());
        ++steps;
        // Terminal restart deletes plates while leaving ownership IDs. Retain
        // the actual last active directions, tagged with their observation step.
        if (mc_v10_platec_is_finished(engine.get()) == 0)
        {
            capture_directions();
        }
        publish();
    }
    auto result = capture(engine.get(), steps, motion);
    for (float const height : result.height_raw)
    {
        if (!std::isfinite(height) || height <= 0.0F)
        {
            throw std::runtime_error("V10 Platec source requires finite positive raw heights");
        }
    }
    return result;
}

} // namespace generation::detail
