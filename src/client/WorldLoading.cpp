#include <client/WorldLoading.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <compare>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{

constexpr uint32_t MAX_TRACKED_MATERIALIZED_CHUNKS = 131'072U;
constexpr auto READY_PROMPT_BUDGET = std::chrono::seconds{ 4 };
constexpr auto MAC_FULL_RADIUS_BUDGET = std::chrono::seconds{ 20 };
constexpr auto WINDOWS_FULL_RADIUS_BUDGET = std::chrono::seconds{ 15 };

[[nodiscard]]
bool isValidPosition(shared::PlayerPosition const position) noexcept
{
    return position.x_subcell < shared::SUBCELLS_PER_CELL
        && position.y_subcell < shared::SUBCELLS_PER_CELL
        && position.z_subcell < shared::SUBCELLS_PER_CELL
        && shared::World::isFlightPositionInBounds(position);
}

[[nodiscard]]
bool samePosition(shared::PlayerPosition const left, shared::PlayerPosition const right) noexcept
{
    return left.x == right.x
        && left.y == right.y
        && left.z == right.z
        && left.x_subcell == right.x_subcell
        && left.y_subcell == right.y_subcell
        && left.z_subcell == right.z_subcell;
}

[[nodiscard]]
uint64_t sampleCount(uint32_t const resolution) noexcept
{
    return static_cast<uint64_t>(resolution) * resolution;
}

[[nodiscard]]
bool isValidGenerationConfiguration(generation::GenerationConfiguration const& configuration) noexcept
{
    if (configuration.resolution != 256U && configuration.resolution != 512U) {
        return false;
    }
    try {
        static_cast<void>(generation::configurationIdentity(configuration));
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]]
bool hasExpectedSize(std::vector<float> const& values, uint64_t const expected) noexcept
{
    return values.empty() || static_cast<uint64_t>(values.size()) == expected;
}

[[nodiscard]]
bool hasExpectedSize(std::vector<uint8_t> const& values, uint64_t const expected) noexcept
{
    return values.empty() || static_cast<uint64_t>(values.size()) == expected;
}

[[nodiscard]]
bool hasFiniteValues(std::vector<float> const& values) noexcept
{
    return std::ranges::all_of(values, [](float const value) { return std::isfinite(value); });
}

[[nodiscard]]
bool hasBooleanValues(std::vector<uint8_t> const& values) noexcept
{
    return std::ranges::all_of(values, [](uint8_t const value) { return value <= 1U; });
}

[[nodiscard]]
bool isValidSnapshot(
    generation::GenerationSnapshot const& snapshot,
    uint32_t const expected_resolution,
    generation::GenerationStage const latest_stage
) noexcept
{
    uint64_t const expected_samples = sampleCount(expected_resolution);
    bool const has_fields = !snapshot.elevation.empty()
        || !snapshot.temperature.empty()
        || !snapshot.precipitation.empty()
        || !snapshot.water_level.empty()
        || !snapshot.ocean_mask.empty()
        || !snapshot.lake_mask.empty()
        || !snapshot.river_mask.empty();
    return static_cast<uint8_t>(snapshot.stage) <= static_cast<uint8_t>(latest_stage)
        && static_cast<uint8_t>(snapshot.stage) <= static_cast<uint8_t>(generation::GenerationStage::Complete)
        && snapshot.resolution == expected_resolution
        && has_fields
        && hasExpectedSize(snapshot.elevation, expected_samples)
        && hasExpectedSize(snapshot.temperature, expected_samples)
        && hasExpectedSize(snapshot.precipitation, expected_samples)
        && hasExpectedSize(snapshot.water_level, expected_samples)
        && hasExpectedSize(snapshot.ocean_mask, expected_samples)
        && hasExpectedSize(snapshot.lake_mask, expected_samples)
        && hasExpectedSize(snapshot.river_mask, expected_samples)
        && hasFiniteValues(snapshot.elevation)
        && hasFiniteValues(snapshot.temperature)
        && hasFiniteValues(snapshot.precipitation)
        && hasFiniteValues(snapshot.water_level)
        && hasBooleanValues(snapshot.ocean_mask)
        && hasBooleanValues(snapshot.lake_mask)
        && hasBooleanValues(snapshot.river_mask);
}

[[nodiscard]]
std::chrono::seconds fullRadiusBudget(client::WorldLoadingPlatform const platform) noexcept
{
    if (platform == client::WorldLoadingPlatform::Windows) {
        return WINDOWS_FULL_RADIUS_BUDGET;
    }
    return MAC_FULL_RADIUS_BUDGET;
}

} // namespace

namespace client
{

WorldLoading::WorldLoading(WorldLoadingPlatform const platform) noexcept
    : m_platform{ platform }
{
}

bool WorldLoading::begin(WorldLoadingIdentity identity, TimePoint const accepted_at)
{
    std::lock_guard const lock{ m_mutex };
    if ((m_has_identity && identity.join_epoch <= m_identity.join_epoch)
        || (m_last_event_at && accepted_at < *m_last_event_at)
        || identity.join_epoch == 0U
        || !isValidGenerationConfiguration(identity.configuration)
        || sampleCount(identity.configuration.resolution) > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    resetForIdentity(identity, accepted_at, accepted_at);
    return true;
}

bool WorldLoading::restart(WorldLoadingIdentity identity, TimePoint const restarted_at)
{
    std::lock_guard const lock{ m_mutex };
    if (!m_has_identity
        || identity.join_epoch <= m_identity.join_epoch
        || (m_last_event_at && restarted_at < *m_last_event_at)
        || !isValidGenerationConfiguration(identity.configuration)
        || sampleCount(identity.configuration.resolution) > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    resetForIdentity(identity, restarted_at, m_request_accepted_at);
    return true;
}

void WorldLoading::resetForIdentity(
    WorldLoadingIdentity identity,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    TimePoint const event_at,
    TimePoint const request_accepted_at
)
{
    clearReadiness();
    m_identity = identity;
    m_has_identity = true;
    m_request_accepted_at = request_accepted_at;
    m_last_event_at = event_at;
    m_atlas_published_at.reset();
    m_refined_spawn_at.reset();
    m_initial_sphere_ready_at.reset();
    m_ready_prompt_frame_rendered_at.reset();
    m_key_accepted_at.reset();
    m_start_acknowledged_at.reset();
    m_playable_sphere_first_ready_at.reset();
    m_generation_stage = generation::GenerationStage::Tectonics;
    m_generation_completed = 0U;
    m_generation_total = 0U;
    m_server_creation_elapsed_microseconds.reset();
    m_has_generation_progress = false;
    m_map_snapshot.reset();
    m_atlas.reset();
    m_atlas_ready = false;
    m_error.clear();
    m_state = WorldLoadingState::Creating;
}

bool WorldLoading::publishServerCreationElapsed(
    WorldLoadingIdentity const& identity,
    uint64_t const elapsed_microseconds,
    TimePoint const published_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, published_at)
        || (m_server_creation_elapsed_microseconds
            && elapsed_microseconds < *m_server_creation_elapsed_microseconds)) {
        return false;
    }
    m_server_creation_elapsed_microseconds = elapsed_microseconds;
    m_last_event_at = published_at;
    return true;
}

bool WorldLoading::bindConfiguration(
    WorldLoadingIdentity const& identity,
    generation::GenerationConfiguration configuration,
    TimePoint const bound_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!m_has_identity
        || identity != m_identity
        || m_state != WorldLoadingState::Creating
        || m_has_generation_progress
        || m_atlas_ready
        || !isValidGenerationConfiguration(configuration)
        || bound_at < m_request_accepted_at
        || (m_last_event_at && bound_at < *m_last_event_at)) {
        return false;
    }
    m_identity.configuration = configuration;
    m_last_event_at = bound_at;
    return true;
}

bool WorldLoading::publishGenerationProgress(
    WorldLoadingIdentity const& identity,
    generation::GenerationProgress const& progress,
    TimePoint const published_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, published_at)
        || m_atlas_ready
        || static_cast<uint8_t>(progress.stage) > static_cast<uint8_t>(generation::GenerationStage::Complete)
        || progress.completed > progress.total
        || static_cast<uint8_t>(progress.stage) < static_cast<uint8_t>(m_generation_stage)
        || (m_has_generation_progress && progress.stage == m_generation_stage
            && (progress.total != m_generation_total || progress.completed < m_generation_completed))
        || (progress.snapshot
            && !isValidSnapshot(*progress.snapshot, identity.configuration.resolution, progress.stage))) {
        return false;
    }

    m_generation_stage = progress.stage;
    m_generation_completed = progress.completed;
    m_generation_total = progress.total;
    m_has_generation_progress = true;
    if (progress.snapshot) {
        m_map_snapshot = progress.snapshot;
    }
    if (m_state == WorldLoadingState::Creating) {
        m_state = WorldLoadingState::PreparingAtlas;
    }
    m_last_event_at = published_at;
    return true;
}

bool WorldLoading::publishAtlas(
    WorldLoadingIdentity const& identity,
    std::shared_ptr<generation::GlobalAtlas const> atlas,
    TimePoint const published_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, published_at)
        || m_atlas_ready
        || !atlas
        || atlas->configuration != identity.configuration
        || static_cast<uint64_t>(atlas->configuration.resolution) * atlas->configuration.resolution
            != atlas->elevation.size()) {
        return false;
    }

    m_atlas = std::move(atlas);
    m_atlas_ready = true;
    m_atlas_published_at = published_at;
    m_state = WorldLoadingState::PreparingSpawn;
    m_last_event_at = published_at;
    return true;
}

bool WorldLoading::publishAtlasCompletion(
    WorldLoadingIdentity const& identity,
    shared::TerrainIdentity const& terrain_identity,
    TimePoint const published_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, published_at)
        || m_atlas_ready
        || !shared::isValidTerrainIdentity(terrain_identity)
        || terrain_identity.seed != identity.configuration.seed
        || !isValidGenerationConfiguration(identity.configuration)
        || terrain_identity.configuration_identity != generation::configurationIdentity(identity.configuration)) {
        return false;
    }
    m_atlas_ready = true;
    m_atlas_published_at = published_at;
    m_state = WorldLoadingState::PreparingSpawn;
    m_last_event_at = published_at;
    return true;
}

bool WorldLoading::publishRefinedSpawn(
    WorldLoadingIdentity const& identity,
    shared::PlayerPosition const position,
    SpawnValidation const validation,
    TimePoint const refined_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, refined_at)
        || !m_atlas_ready
        || m_spawn_position
        || validation == SpawnValidation::Unknown) {
        return false;
    }
    if (validation == SpawnValidation::Unsafe) {
        m_error = "The refined spawn failed authoritative safety validation.";
        m_state = WorldLoadingState::Failed;
        m_last_event_at = refined_at;
        return true;
    }
    if (!isValidPosition(position)) {
        return false;
    }

    m_spawn_position = position;
    m_player_position = position;
    m_refined_spawn_at = refined_at;
    rebuildInitialSphere(position);
    rebuildPlayableSphere(position);
    updateReadiness(refined_at);
    m_last_event_at = refined_at;
    return true;
}

bool WorldLoading::publishMaterializedChunk(
    WorldLoadingIdentity const& identity,
    shared::Chunk const& chunk,
    TimePoint const published_at
)
{
    std::lock_guard const lock{ m_mutex };
    shared::ChunkCoordinate const coordinate = chunk.coordinate();
    if (!acceptsIdentityAndTime(identity, published_at) || !shared::WorldBounds::isValidChunk(coordinate)) {
        return false;
    }

    if (!m_materialized_chunks.contains(coordinate)
        && static_cast<uint32_t>(m_materialized_chunks.size()) >= MAX_TRACKED_MATERIALIZED_CHUNKS) {
        for (auto iterator = m_materialized_chunks.begin(); iterator != m_materialized_chunks.end();) {
            if (!m_initial_required_chunks.contains(*iterator)) {
                iterator = m_materialized_chunks.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (static_cast<uint32_t>(m_materialized_chunks.size()) >= MAX_TRACKED_MATERIALIZED_CHUNKS) {
            return false;
        }
    }

    auto const [iterator, inserted] = m_materialized_chunks.insert(coordinate);
    static_cast<void>(iterator);
    if (inserted) {
        if (m_initial_required_chunks.contains(coordinate)) {
            ++m_initial_ready_chunks;
        }
    }
    updateReadiness(published_at);
    m_last_event_at = published_at;
    return true;
}

bool WorldLoading::publishAuthoritativeColumn(
    WorldLoadingIdentity const& identity,
    shared::ChunkCoordinate const coordinate,
    TimePoint const published_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, published_at)
        || !shared::WorldBounds::isValidChunk(coordinate) || coordinate.z != 0) {
        return false;
    }
    if (!m_authoritative_columns.contains(coordinate)
        && m_authoritative_columns.size() >= shared::MAX_AUTHORITATIVE_COLUMNS_PER_PLAYER) {
        return false;
    }
    if (m_authoritative_columns.insert(coordinate).second) {
        if (m_required_authoritative_columns.contains(coordinate)) {
            ++m_ready_authoritative_columns;
        }
        if (m_startup_required_authoritative_columns.contains(coordinate)) {
            ++m_startup_ready_authoritative_columns;
        }
    }
    updateReadiness(published_at);
    m_last_event_at = published_at;
    return true;
}

bool WorldLoading::evictAuthoritativeColumn(
    WorldLoadingIdentity const& identity,
    shared::ChunkCoordinate const coordinate,
    TimePoint const evicted_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, evicted_at)
        || !shared::WorldBounds::isValidChunk(coordinate) || coordinate.z != 0) {
        return false;
    }
    if (m_authoritative_columns.erase(coordinate) != 0U) {
        if (m_required_authoritative_columns.contains(coordinate)) {
            --m_ready_authoritative_columns;
        }
        if (m_startup_required_authoritative_columns.contains(coordinate)) {
            --m_startup_ready_authoritative_columns;
        }
        if (m_authoritative_columns.empty()) {
            m_startup_prefetch_primed_at.reset();
            if (m_player_position) {
                rebuildPlayableSphere(*m_player_position);
            }
        }
    }
    updateReadiness(evicted_at);
    m_last_event_at = evicted_at;
    return true;
}

bool WorldLoading::evictChunk(
    WorldLoadingIdentity const& identity,
    shared::ChunkCoordinate const coordinate,
    TimePoint const evicted_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, evicted_at) || !shared::WorldBounds::isValidChunk(coordinate)) {
        return false;
    }

    if (m_materialized_chunks.erase(coordinate) > 0U) {
        if (m_initial_required_chunks.contains(coordinate)) {
            --m_initial_ready_chunks;
        }
    }
    updateReadiness(evicted_at);
    m_last_event_at = evicted_at;
    return true;
}

bool WorldLoading::updatePlayerPosition(
    WorldLoadingIdentity const& identity,
    shared::PlayerPosition const position,
    TimePoint const observed_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, observed_at)
        || !m_spawn_position
        || !isValidPosition(position)) {
        return false;
    }
    if (!m_player_position || !samePosition(*m_player_position, position)) {
        bool const moved_column = !m_player_position
            || m_player_position->x / shared::Chunk::SIDE_LENGTH != position.x / shared::Chunk::SIDE_LENGTH
            || m_player_position->y / shared::Chunk::SIDE_LENGTH != position.y / shared::Chunk::SIDE_LENGTH;
        m_player_position = position;
        if (moved_column) {
            rebuildPlayableSphere(position);
        }
        updateReadiness(observed_at);
    }
    m_last_event_at = observed_at;
    return true;
}

bool WorldLoading::markReadyPromptFrameRendered(
    WorldLoadingIdentity const& identity,
    TimePoint const rendered_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, rendered_at) || m_state != WorldLoadingState::ReadyForStart) {
        return false;
    }
    if (!m_ready_prompt_frame_rendered_at) {
        m_ready_prompt_frame_rendered_at = rendered_at;
    }
    m_last_event_at = rendered_at;
    return true;
}

LoadingInputResult WorldLoading::acceptStartInput(
    WorldLoadingIdentity const& identity,
    TimePoint const accepted_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, accepted_at)) {
        return LoadingInputResult::Consumed;
    }
    if (m_state == WorldLoadingState::Playing) {
        m_last_event_at = accepted_at;
        return LoadingInputResult::Forward;
    }
    if (m_state == WorldLoadingState::ReadyForStart
        && m_ready_prompt_frame_rendered_at
        && accepted_at >= *m_ready_prompt_frame_rendered_at) {
        m_key_accepted_at = accepted_at;
        m_state = WorldLoadingState::Starting;
        m_last_event_at = accepted_at;
        return LoadingInputResult::Started;
    }
    m_last_event_at = accepted_at;
    return LoadingInputResult::Consumed;
}

bool WorldLoading::acknowledgeStart(
    WorldLoadingIdentity const& identity,
    bool const accepted,
    std::string_view const failure,
    TimePoint const acknowledged_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, acknowledged_at)
        || m_state != WorldLoadingState::Starting) {
        return false;
    }
    if (!accepted) {
        m_error.assign(failure.empty() ? "The server rejected starting this world." : failure);
        m_state = WorldLoadingState::Failed;
        m_last_event_at = acknowledged_at;
        return true;
    }
    if (!m_atlas_ready || !m_spawn_position || !initialSphereReady()) {
        return false;
    }
    m_start_acknowledged_at = acknowledged_at;
    m_state = WorldLoadingState::Playing;
    m_last_event_at = acknowledged_at;
    return true;
}

bool WorldLoading::fail(
    WorldLoadingIdentity const& identity,
    std::string_view const reason,
    TimePoint const failed_at
)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, failed_at) || reason.empty()) {
        return false;
    }
    m_error.assign(reason);
    m_state = WorldLoadingState::Failed;
    m_last_event_at = failed_at;
    return true;
}

bool WorldLoading::cancel(WorldLoadingIdentity const& identity, TimePoint const cancelled_at)
{
    std::lock_guard const lock{ m_mutex };
    if (!acceptsIdentityAndTime(identity, cancelled_at)) {
        return false;
    }
    clearReadiness();
    m_atlas.reset();
    m_atlas_ready = false;
    m_map_snapshot.reset();
    m_state = WorldLoadingState::Cancelled;
    m_last_event_at = cancelled_at;
    return true;
}

WorldLoadingStatus WorldLoading::status(TimePoint const now) const
{
    std::lock_guard const lock{ m_mutex };
    std::chrono::seconds const radius_budget = fullRadiusBudget(m_platform);
    WorldLoadingStatus result{
        .state = m_state,
        .request_accepted_at = m_request_accepted_at,
        .ready_prompt_deadline = m_request_accepted_at + READY_PROMPT_BUDGET,
        .full_radius_deadline = m_request_accepted_at + radius_budget,
        .atlas_published_at = m_atlas_published_at,
        .refined_spawn_at = m_refined_spawn_at,
        .initial_sphere_ready_at = m_initial_sphere_ready_at,
        .ready_prompt_frame_rendered_at = m_ready_prompt_frame_rendered_at,
        .key_accepted_at = m_key_accepted_at,
        .start_acknowledged_at = m_start_acknowledged_at,
        .playable_sphere_first_ready_at = m_playable_sphere_first_ready_at,
        .startup_prefetch_primed_at = m_startup_prefetch_primed_at,
        .safe_spawn_position = m_spawn_position,
        .current_player_position = m_player_position,
        .initial_sphere_ready_chunks = m_initial_ready_chunks,
        .initial_sphere_required_chunks = static_cast<uint32_t>(m_initial_required_chunks.size()),
        .playable_sphere_ready_chunks = m_ready_authoritative_columns
            * (shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH),
        .playable_sphere_required_chunks = static_cast<uint32_t>(m_required_authoritative_columns.size())
            * (shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH),
        .resident_authoritative_columns = m_ready_authoritative_columns,
        .required_authoritative_columns = static_cast<uint32_t>(m_required_authoritative_columns.size()),
        .actual_chunk_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
        .visual_lod_radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        .generation_stage = m_generation_stage,
        .generation_completed = m_generation_completed,
        .generation_total = m_generation_total,
        .server_creation_elapsed_microseconds = m_server_creation_elapsed_microseconds,
        .atlas_ready = m_atlas_ready,
        .start_pending = m_state == WorldLoadingState::Starting,
        .spawn_safe = m_spawn_position.has_value(),
        .initial_sphere_ready = initialSphereReady(),
        .playable_sphere_ready = playableSphereReady(),
        .map_snapshot = m_map_snapshot,
        .atlas = m_atlas,
        .error = m_error,
    };
    result.ready_prompt_deadline_missed = now > result.ready_prompt_deadline
        && (!m_ready_prompt_frame_rendered_at || *m_ready_prompt_frame_rendered_at > result.ready_prompt_deadline);
    result.full_radius_deadline_missed = now > result.full_radius_deadline
        && (!m_playable_sphere_first_ready_at || *m_playable_sphere_first_ready_at > result.full_radius_deadline);
    return result;
}

std::vector<shared::ChunkCoordinate> WorldLoading::requiredChunkKeys(
    shared::PlayerPosition const center,
    uint32_t const radius_blocks
)
{
    if (radius_blocks > shared::MAX_SPHERE_RADIUS_BLOCKS) {
        throw std::invalid_argument("Dense sphere exceeds supported radius; use authoritative terrain columns.");
    }
    return shared::requiredChunkKeys(center, radius_blocks);
}

uint64_t WorldLoading::ChunkCoordinateHash::operator()(
    shared::ChunkCoordinate const coordinate
) const noexcept
{
    uint64_t hash = static_cast<uint32_t>(coordinate.x);
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(coordinate.y)) + 0x9e37'79b9'7f4a'7c15ULL
        + (hash << 6U) + (hash >> 2U);
    hash ^= static_cast<uint64_t>(static_cast<uint32_t>(coordinate.z)) + 0x9e37'79b9'7f4a'7c15ULL
        + (hash << 6U) + (hash >> 2U);
    return hash;
}

bool WorldLoading::acceptsIdentityAndTime(
    WorldLoadingIdentity const& identity,
    TimePoint const at
) const noexcept
{
    return m_has_identity
        && identity == m_identity
        && at >= m_request_accepted_at
        && (!m_last_event_at || at >= *m_last_event_at)
        && m_state != WorldLoadingState::Idle
        && m_state != WorldLoadingState::Failed
        && m_state != WorldLoadingState::Cancelled;
}

bool WorldLoading::initialSphereReady() const noexcept
{
    return m_spawn_position
        && (m_initial_required_chunks.empty()
            || m_initial_ready_chunks == static_cast<uint32_t>(m_initial_required_chunks.size()));
}

bool WorldLoading::playableSphereReady() const noexcept
{
    return m_player_position
        && m_startup_prefetch_primed_at
        && !m_required_authoritative_columns.empty()
        && m_ready_authoritative_columns == static_cast<uint32_t>(m_required_authoritative_columns.size());
}

void WorldLoading::rebuildInitialSphere(shared::PlayerPosition const center)
{
    std::vector<shared::ChunkCoordinate> const keys = requiredChunkKeys(
        center,
        INITIAL_READY_SPHERE_RADIUS_BLOCKS
    );
    m_initial_required_chunks.clear();
    m_initial_required_chunks.reserve(keys.size());
    m_initial_required_chunks.insert(keys.begin(), keys.end());
    m_initial_ready_chunks = 0U;
    for (shared::ChunkCoordinate const key : m_initial_required_chunks) {
        if (m_materialized_chunks.contains(key)) {
            ++m_initial_ready_chunks;
        }
    }
}

void WorldLoading::rebuildPlayableSphere(shared::PlayerPosition const center)
{
    std::vector<shared::ChunkCoordinate> const keys = shared::requiredTerrainColumns(
        center,
        PLAYABLE_TERRAIN_RADIUS_BLOCKS
    );
    m_required_authoritative_columns.clear();
    m_required_authoritative_columns.reserve(keys.size());
    m_required_authoritative_columns.insert(keys.begin(), keys.end());
    m_ready_authoritative_columns = 0U;
    for (shared::ChunkCoordinate const key : m_required_authoritative_columns) {
        if (m_authoritative_columns.contains(key)) {
            ++m_ready_authoritative_columns;
        }
    }
    if (!m_startup_prefetch_primed_at) {
        auto const reserve = shared::requiredTerrainColumns(center, shared::AUTHORITATIVE_PREFETCH_RADIUS_BLOCKS);
        m_startup_required_authoritative_columns = ChunkKeySet{reserve.begin(), reserve.end()};
        m_startup_ready_authoritative_columns = 0U;
        for (auto const key : m_startup_required_authoritative_columns) {
            m_startup_ready_authoritative_columns += m_authoritative_columns.contains(key) ? 1U : 0U;
        }
    }
}

void WorldLoading::updateReadiness(TimePoint const at)
{
    if (!m_startup_prefetch_primed_at && !m_startup_required_authoritative_columns.empty()
        && m_startup_ready_authoritative_columns == m_startup_required_authoritative_columns.size()) {
        m_startup_prefetch_primed_at = at;
    }
    bool const ready_for_start = m_atlas_ready && m_spawn_position && initialSphereReady();
    if (initialSphereReady() && !m_initial_sphere_ready_at) {
        m_initial_sphere_ready_at = at;
    }
    if (playableSphereReady() && !m_playable_sphere_first_ready_at) {
        m_playable_sphere_first_ready_at = at;
    }
    if (!ready_for_start && m_state != WorldLoadingState::Playing) {
        m_ready_prompt_frame_rendered_at.reset();
    }
    refreshReadyState();
}

void WorldLoading::refreshReadyState()
{
    if (m_state == WorldLoadingState::Failed
        || m_state == WorldLoadingState::Cancelled
        || m_state == WorldLoadingState::Starting
        || m_state == WorldLoadingState::Playing
        || !m_atlas_ready) {
        return;
    }
    if (!m_spawn_position || !initialSphereReady()) {
        m_state = WorldLoadingState::PreparingSpawn;
        return;
    }
    m_state = WorldLoadingState::ReadyForStart;
}

void WorldLoading::clearReadiness() noexcept
{
    m_spawn_position.reset();
    m_player_position.reset();
    m_materialized_chunks.clear();
    m_initial_required_chunks.clear();
    m_authoritative_columns.clear();
    m_required_authoritative_columns.clear();
    m_startup_required_authoritative_columns.clear();
    m_startup_ready_authoritative_columns = 0U;
    m_startup_prefetch_primed_at.reset();
    m_initial_ready_chunks = 0U;
    m_ready_authoritative_columns = 0U;
}

} // namespace client
