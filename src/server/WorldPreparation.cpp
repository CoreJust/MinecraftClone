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

WorldPreparationProgressJournal::WorldPreparationProgressJournal()
{
    m_milestones.reserve(MAX_MILESTONE_COUNT);
}

void WorldPreparationProgressJournal::record(WorldPreparationProgress const& update)
{
    generation::GenerationStage const stage = update.progress.stage;
    auto const stage_order = [](generation::GenerationStage const value) {
        return value == generation::GenerationStage::Complete
            ? 256U : static_cast<unsigned>(value);
    };
    if (m_latest_stage && stage_order(stage) < stage_order(*m_latest_stage)) {
        throw std::logic_error{ "world preparation progress stage regressed" };
    }
    if (!m_latest_stage || stage != *m_latest_stage) {
        WorldPreparationProgress boundary = update;
        boundary.progress.snapshot.reset();
        m_milestones.push_back({
            .kind = WorldPreparationProgressMilestoneKind::StageBoundary,
            .update = std::move(boundary),
        });
        m_latest_stage = stage;
    }
    if (update.progress.snapshot) {
        // Preserve the first authoritative frame of every stage (including H0),
        // then coalesce only its live tail. Consumers revisit it by sequence.
        WorldPreparationProgressMilestone* latest_source = nullptr;
        uint32_t source_count = 0U;
        for (WorldPreparationProgressMilestone& milestone : m_milestones) {
            if (milestone.kind == WorldPreparationProgressMilestoneKind::SourceSnapshot
                && milestone.update.progress.stage == stage) {
                ++source_count;
                latest_source = &milestone;
            }
        }
        if (source_count == 2U) {
            latest_source->update = update;
            return;
        }
        m_milestones.push_back({
            .kind = WorldPreparationProgressMilestoneKind::SourceSnapshot,
            .update = update,
        });
    }
    if (m_milestones.size() > MAX_MILESTONE_COUNT) {
        throw std::logic_error{ "world preparation milestone bound exceeded" };
    }
}

bool WorldPreparationProgressThrottle::admit(
    generation::GenerationProgress const& progress,
    TimePoint const now
) noexcept
{
    bool const stage_changed = !m_last_stage || progress.stage != *m_last_stage;
    bool const progress_changed = stage_changed
        || progress.completed != m_last_completed
        || progress.total != m_last_total;
    bool const stage_complete = progress.total > 0U && progress.completed == progress.total;
    bool const interval_elapsed = !m_has_published || now - m_last_published_at >= MIN_INTERVAL;
    if (!stage_changed && !stage_complete && !(progress_changed && interval_elapsed)) {
        return false;
    }
    notePublished(progress, now);
    return true;
}

void WorldPreparationProgressThrottle::notePublished(
    generation::GenerationProgress const& progress,
    TimePoint const now
) noexcept
{
    m_last_stage = progress.stage;
    m_last_completed = progress.completed;
    m_last_total = progress.total;
    m_last_published_at = now;
    m_has_published = true;
}

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
    WorldPreparationSnapshot snapshot = m_snapshot;
    snapshot.progress_milestones = m_progress_journal.milestones();
    return snapshot;
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
        generation::GenerationConfiguration plan_request_configuration = m_configuration;
        plan_request_configuration.model_tag = generation::TerrainModelTag::R6;
        plan_request_configuration.use_v10_fine_terrain = false;
        auto terrain_plan = shared::corelang::prepareTerrainPlan(
            plan_request_configuration, m_backend, cancellation
        );
        if (!terrain_plan) {
            if (terrain_plan.error().code == shared::corelang::BackendErrorCode::Cancelled) {
                publishCancelled();
                return;
            }
            throw std::runtime_error{ "CoreLang Terrain-v2 plan failed: " + terrain_plan.error().message };
        }
        // CoreLang's manifest serializes the shared R6 fields only. Native model
        // selection and the V10 fine option are restored after it validates.
        terrain_plan->configuration.model_tag = m_configuration.model_tag;
        terrain_plan->configuration.use_v10_fine_terrain = m_configuration.use_v10_fine_terrain;
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
        || static_cast<uint8_t>(progress.stage) > static_cast<uint8_t>(generation::GenerationStage::V10Water)
        || (progress.snapshot
            && (progress.snapshot->stage != progress.stage
                || progress.snapshot->resolution != ((m_configuration.model_tag == generation::TerrainModelTag::V9
                    || m_configuration.model_tag == generation::TerrainModelTag::V21
                    || m_configuration.model_tag == generation::TerrainModelTag::V10)
                    ? 256U
                    : m_configuration.resolution)
                || progress.snapshot->v9.has_value()
                    != (m_configuration.model_tag == generation::TerrainModelTag::V9)
                || progress.snapshot->v21.has_value()
                    != (m_configuration.model_tag == generation::TerrainModelTag::V21)
                || progress.snapshot->v10.has_value()
                    != (m_configuration.model_tag == generation::TerrainModelTag::V10)
                || !generation::isValidGenerationSnapshot(*progress.snapshot)))) {
        throw std::runtime_error{ "generation emitted malformed progress or map snapshot data" };
    }

    WorldPreparationProgress const published{
        .sequence = ++m_progress_sequence,
        .progress = progress,
        .observed_at = observed_at,
    };
    m_snapshot.latest_progress = published;
    m_progress_journal.record(published);
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
