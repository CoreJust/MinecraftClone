#pragma once

#include <shared/corelang/Backend.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace server
{

enum class WorldPreparationState : uint8_t
{
    Preparing,
    Ready,
    Failed,
    Cancelled,
};

struct WorldPreparationProgress final
{
    uint64_t sequence = 0U;
    generation::GenerationProgress progress;
    std::chrono::steady_clock::time_point observed_at{};
};

struct WorldPreparationSnapshot final
{
    WorldPreparationState state = WorldPreparationState::Preparing;
    std::chrono::steady_clock::time_point request_accepted_at{};
    std::chrono::steady_clock::time_point state_changed_at{};
    std::optional<WorldPreparationProgress> latest_progress;
    std::shared_ptr<generation::GenerationSnapshot const> latest_map_snapshot;
    std::optional<std::chrono::steady_clock::time_point> latest_map_snapshot_observed_at;
    std::shared_ptr<shared::TerrainWorld const> ready_world;
    std::optional<shared::corelang::TerrainPlan> terrain_plan;
    std::string error;
};

class WorldPreparation final
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    WorldPreparation(
        generation::GenerationConfiguration configuration,
        uint64_t world_revision,
        TimePoint request_accepted_at = Clock::now(),
        shared::corelang::Backend backend = shared::corelang::Backend::Interpreter
    );
    ~WorldPreparation();

    WorldPreparation(WorldPreparation const&) = delete;
    WorldPreparation& operator=(WorldPreparation const&) = delete;
    WorldPreparation(WorldPreparation&&) = delete;
    WorldPreparation& operator=(WorldPreparation&&) = delete;

    [[nodiscard]]
    WorldPreparationSnapshot latest() const;

    [[nodiscard]]
    std::shared_ptr<shared::TerrainWorld const> readyWorld() const;

    [[nodiscard]]
    generation::GenerationConfiguration const& configuration() const noexcept { return m_configuration; }

    [[nodiscard]]
    std::optional<shared::WorldGenerationPlan> generationPlan() const;

    void cancel();

private:
    void run(std::stop_token const& cancellation);

    void publishProgress(generation::GenerationProgress const& progress);

    void publishReady(std::shared_ptr<shared::TerrainWorld const> world);

    void publishFailure(std::string error);

    void publishCancelled();

private:
    generation::GenerationConfiguration m_configuration;
    uint64_t m_world_revision;
    shared::corelang::Backend m_backend;
    TimePoint m_request_accepted_at;
    mutable std::mutex m_mutex;
    std::mutex m_join_mutex;
    WorldPreparationSnapshot m_snapshot;
    uint64_t m_progress_sequence = 0U;
    std::jthread m_worker;
};

} // namespace server
