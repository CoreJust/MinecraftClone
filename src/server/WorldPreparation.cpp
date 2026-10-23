#include <server/WorldPreparation.hpp>

#include <shared/corelang/Backend.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>
#include <generation/Generation.hpp>

#include <exception>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace server
{

WorldPreparation::WorldPreparation(
    generation::GenerationConfiguration configuration,
    uint64_t const world_revision,
    TimePoint const request_accepted_at,
    shared::corelang::Backend const backend
)
    : m_configuration{ configuration }
    , m_world_revision{ world_revision }
    , m_backend{ backend }
    , m_request_accepted_at{ request_accepted_at }
{
    m_snapshot.request_accepted_at = m_request_accepted_at;
    m_snapshot.state_changed_at = m_request_accepted_at;

    if (m_world_revision == 0U) {
        publishFailure("Terrain-v2 world preparation failed: world revision must be nonzero.");
        return;
    }

    try {
        m_worker = std::jthread{ [this](std::stop_token const& cancellation) {
            run(cancellation);
        } };
    } catch (std::exception const& error) {
        publishFailure(std::string{ "Could not start Terrain-v2 world preparation: " } + error.what());
    } catch (...) {
        publishFailure("Could not start Terrain-v2 world preparation: unknown thread error.");
    }
}

WorldPreparation::~WorldPreparation()
{
    cancel();
}

WorldPreparationSnapshot WorldPreparation::latest() const
{
    std::lock_guard const lock{ m_mutex };
    return m_snapshot;
}

std::shared_ptr<shared::TerrainWorld const> WorldPreparation::readyWorld() const
{
    std::lock_guard const lock{ m_mutex };
    if (m_snapshot.state != WorldPreparationState::Ready) {
        return nullptr;
    }
    return m_snapshot.ready_world;
}

std::optional<shared::WorldGenerationPlan> WorldPreparation::generationPlan() const
{
    std::lock_guard const lock{ m_mutex };
    if (m_snapshot.state != WorldPreparationState::Ready || !m_snapshot.terrain_plan) {
        return std::nullopt;
    }
    return m_snapshot.terrain_plan->world_generation_plan;
}

void WorldPreparation::cancel()
{
    std::lock_guard const join_lock{ m_join_mutex };
    bool request_stop = false;
    {
        std::lock_guard const lock{ m_mutex };
        if (m_snapshot.state == WorldPreparationState::Preparing) {
            m_snapshot.state = WorldPreparationState::Cancelled;
            m_snapshot.state_changed_at = Clock::now();
            m_snapshot.ready_world.reset();
            request_stop = true;
        }
    }
    if (request_stop) {
        m_worker.request_stop();
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void WorldPreparation::run(std::stop_token const& cancellation)
{
    try {
        std::string const expected_configuration_identity = generation::configurationIdentity(m_configuration);
        auto terrain_plan = shared::corelang::prepareTerrainPlan(m_configuration, m_backend, cancellation);
        if (!terrain_plan) {
            if (terrain_plan.error().code == shared::corelang::BackendErrorCode::Cancelled) {
                publishCancelled();
                return;
            }
            throw std::runtime_error{ "CoreLang Terrain-v2 plan failed: " + terrain_plan.error().message };
        }
        if (terrain_plan->configuration != m_configuration
            || terrain_plan->receipt.requested_backend != m_backend
            || terrain_plan->receipt.actual_backend != m_backend
            || !shared::isValidWorldGenerationPlan(terrain_plan->world_generation_plan)) {
            throw std::runtime_error{ "CoreLang returned a mismatched terrain configuration, backend, or plan" };
        }
        generation::GenerationConfiguration const plan_configuration = terrain_plan->configuration;
        {
            std::lock_guard const lock{ m_mutex };
            if (m_snapshot.state != WorldPreparationState::Preparing || cancellation.stop_requested()) {
                return;
            }
            m_snapshot.terrain_plan = std::move(*terrain_plan);
        }
        std::shared_ptr<generation::GlobalAtlas const> atlas = generation::prepareAtlas(
            plan_configuration,
            cancellation,
            [this](generation::GenerationProgress const& progress) {
                publishProgress(progress);
            }
        );
        if (cancellation.stop_requested()) {
            publishCancelled();
            return;
        }
        if (!atlas || atlas->configuration != m_configuration) {
            throw std::runtime_error{ "generation returned an absent atlas or a mismatched configuration" };
        }

        auto world = std::make_shared<shared::TerrainWorld const>(std::move(atlas), m_world_revision);
        shared::TerrainIdentity const& identity = world->identity();
        if (identity.revision != m_world_revision
            || identity.seed != m_configuration.seed
            || identity.configuration_identity != expected_configuration_identity) {
            throw std::runtime_error{ "validated terrain identity does not match the accepted world request" };
        }
        if (cancellation.stop_requested()) {
            publishCancelled();
            return;
        }
        publishReady(std::move(world));
    } catch (generation::GenerationCancelled const&) {
        publishCancelled();
    } catch (std::exception const& error) {
        if (cancellation.stop_requested()) {
            publishCancelled();
        } else {
            publishFailure(std::string{ "Terrain-v2 world preparation failed: " } + error.what());
        }
    } catch (...) {
        if (cancellation.stop_requested()) {
            publishCancelled();
        } else {
            publishFailure("Terrain-v2 world preparation failed with an unknown error.");
        }
    }
}

void WorldPreparation::publishProgress(generation::GenerationProgress const& progress)
{
    TimePoint const observed_at = Clock::now();
    std::lock_guard const lock{ m_mutex };
    if (m_snapshot.state != WorldPreparationState::Preparing) {
        return;
    }
    if (progress.completed > progress.total
        || static_cast<uint8_t>(progress.stage) > static_cast<uint8_t>(generation::GenerationStage::Complete)
        || (progress.snapshot
            && (progress.snapshot->stage != progress.stage
                || progress.snapshot->resolution != m_configuration.resolution))) {
        throw std::runtime_error{ "generation emitted malformed progress or map snapshot data" };
    }

    m_snapshot.latest_progress = WorldPreparationProgress{
        .sequence = ++m_progress_sequence,
        .progress = progress,
        .observed_at = observed_at,
    };
    if (progress.snapshot) {
        m_snapshot.latest_map_snapshot = progress.snapshot;
        m_snapshot.latest_map_snapshot_observed_at = observed_at;
    }
}

void WorldPreparation::publishReady(std::shared_ptr<shared::TerrainWorld const> world)
{
    std::lock_guard const lock{ m_mutex };
    if (m_snapshot.state != WorldPreparationState::Preparing) {
        return;
    }
    m_snapshot.ready_world = std::move(world);
    m_snapshot.state = WorldPreparationState::Ready;
    m_snapshot.state_changed_at = Clock::now();
}

void WorldPreparation::publishFailure(std::string error)
{
    std::lock_guard const lock{ m_mutex };
    if (m_snapshot.state != WorldPreparationState::Preparing) {
        return;
    }
    m_snapshot.ready_world.reset();
    m_snapshot.error = std::move(error);
    m_snapshot.state = WorldPreparationState::Failed;
    m_snapshot.state_changed_at = Clock::now();
}

void WorldPreparation::publishCancelled()
{
    std::lock_guard const lock{ m_mutex };
    if (m_snapshot.state != WorldPreparationState::Preparing) {
        return;
    }
    m_snapshot.ready_world.reset();
    m_snapshot.state = WorldPreparationState::Cancelled;
    m_snapshot.state_changed_at = Clock::now();
}

} // namespace server
