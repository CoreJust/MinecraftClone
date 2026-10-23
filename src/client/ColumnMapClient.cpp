#include <client/ColumnMapClient.hpp>

#include <client/ColumnMapPresentation.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>

#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace client
{

ColumnMapPalette canonicalColumnMapPalette() noexcept
{
    ColumnMapPalette palette{};
    palette.colors[static_cast<uint8_t>(shared::Block::Air)] = { 189U, 217U, 237U, 255U };
    palette.colors[static_cast<uint8_t>(shared::Block::Stone)] = { 128U, 128U, 128U, 255U };
    palette.colors[static_cast<uint8_t>(shared::Block::Grass)] = { 92U, 147U, 69U, 255U };
    palette.colors[static_cast<uint8_t>(shared::Block::Dirt)] = { 136U, 103U, 72U, 255U };
    palette.colors[static_cast<uint8_t>(shared::Block::Bedrock)] = { 42U, 40U, 49U, 255U };
    palette.colors[static_cast<uint8_t>(shared::Block::Snow)] = { 244U, 247U, 250U, 255U };
    palette.colors[static_cast<uint8_t>(shared::Block::Ice)] = { 128U, 201U, 214U, 255U };
    return palette;
}

ColumnMapClient::ColumnMapClient(
    WorldWindow const initial_window,
    ColumnMapPalette palette,
    shared::WorldMode const mode,
    shared::WorldConfiguration const configuration,
    bool const follow_player
)
    : GameClient{ mode, configuration, true }
    , m_presentation{ palette }
    , m_follow_player{ follow_player }
{
    if (!setWindow(initial_window)) {
        throw std::invalid_argument{ "ColumnMapClient window must be between 1 and 256 columns per side" };
    }
}

bool ColumnMapClient::setWindow(WorldWindow const window)
{
    if (!m_presentation.setWindow(window)) {
        return false;
    }
    m_frame_state.active_materialized_column_presence.assign(
        m_presentation.planes().coverage.size(),
        0U
    );
    return true;
}

void ColumnMapClient::setPlayerFollowing(bool const enabled) noexcept
{
    m_follow_player = enabled;
}

ColumnMapPresentation const& ColumnMapClient::presentation() const noexcept
{
    return m_presentation;
}

ColumnMapFrameState const& ColumnMapClient::frameState() const noexcept
{
    return m_frame_state;
}

shared::Direction ColumnMapClient::input()
{
    return shared::Direction{ .x = 0U, .y = 0U };
}

void ColumnMapClient::render()
{
    std::chrono::steady_clock::time_point const now = std::chrono::steady_clock::now();
    shared::SparseWorld const* world = authoritativeWorld();
    WorldLoadingStatus loading = worldLoadingStatus();
    std::optional<shared::TerrainIdentity> const terrain_identity = activeTerrainIdentity();
    std::optional<uint64_t> const active_coverage_epoch = activeCoverageEpoch();
    std::optional<shared::PlayerPosition> const active_coverage_center = activeCoverageCenter();
    if (loading.state == WorldLoadingState::Failed || loading.state == WorldLoadingState::Cancelled) {
        world = nullptr;
    }

    bool const request_changed = loading.request_accepted_at != m_last_request_accepted_at;
    bool const coverage_epoch_changed = active_coverage_epoch != m_frame_state.active_coverage_epoch;
    bool const coverage_center_changed = active_coverage_center.has_value()
        != m_frame_state.active_coverage_center.has_value()
        || (active_coverage_center && m_frame_state.active_coverage_center
            && (active_coverage_center->x != m_frame_state.active_coverage_center->x
                || active_coverage_center->y != m_frame_state.active_coverage_center->y
                || active_coverage_center->z != m_frame_state.active_coverage_center->z
                || active_coverage_center->x_subcell != m_frame_state.active_coverage_center->x_subcell
                || active_coverage_center->y_subcell != m_frame_state.active_coverage_center->y_subcell
                || active_coverage_center->z_subcell != m_frame_state.active_coverage_center->z_subcell));
    bool const lost_world = !world && m_frame_state.authoritative_world_available;
    if (request_changed || lost_world) {
        static_cast<void>(m_presentation.setWindow(m_presentation.window()));
        m_last_request_accepted_at = loading.request_accepted_at;
    }
    if (request_changed || coverage_epoch_changed || coverage_center_changed || lost_world
        || m_frame_state.active_materialized_column_presence.size()
            != m_presentation.planes().coverage.size()) {
        m_frame_state.active_materialized_column_presence.assign(
            m_presentation.planes().coverage.size(),
            0U
        );
    }

    if (world != nullptr) {
        followPlayerWhenOutsideWindow();
    }

    ColumnMapUpdate map_update;
    uint32_t const first_column_index = m_presentation.nextColumnIndex();
    if (world != nullptr) {
        map_update = m_presentation.update(
            *world,
            ColumnMapPresentation::MAX_COLUMNS_PER_UPDATE
        );
    }
    uint32_t const column_count = static_cast<uint32_t>(
        m_frame_state.active_materialized_column_presence.size()
    );
    if (column_count > 0U && active_coverage_epoch && world != nullptr) {
        WorldWindow const& window = m_presentation.window();
        for (uint32_t offset = 0U; offset < map_update.scanned_columns; ++offset) {
            uint32_t const index = (first_column_index + offset) % column_count;
            uint32_t const x_offset = index % window.width;
            uint32_t const y_offset = index / window.width;
            uint32_t const world_x = (shared::WorldBounds::wrapHorizontal(window.origin_x) + x_offset)
                % shared::WorldExtent::WIDTH;
            uint32_t const world_y = (shared::WorldBounds::wrapHorizontal(window.origin_y) + y_offset)
                % shared::WorldExtent::WIDTH;
            bool active = false;
            uint32_t const vertical_chunk_count = shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH;
            for (uint32_t z = 0U; z < vertical_chunk_count && !active; ++z) {
                active = activeCoverageContains(shared::ChunkCoordinate{
                    .x = static_cast<int32_t>(world_x / shared::Chunk::SIDE_LENGTH),
                    .y = static_cast<int32_t>(world_y / shared::Chunk::SIDE_LENGTH),
                    .z = static_cast<int32_t>(z),
                });
            }
            m_frame_state.active_materialized_column_presence[index] = active ? 1U : 0U;
        }
    }

    if (loading.state == WorldLoadingState::ReadyForStart) {
        static_cast<void>(markLoadingPromptRendered(now));
        loading = worldLoadingStatus();
    }

    ++m_frame_state.sequence;
    m_frame_state.composed_at = now;
    m_frame_state.map_update = map_update;
    m_frame_state.loading = std::move(loading);
    m_frame_state.terrain_identity = terrain_identity;
    m_frame_state.active_coverage_epoch = active_coverage_epoch;
    m_frame_state.active_coverage_center = active_coverage_center;
    m_frame_state.authoritative_world_available = world != nullptr;
    m_frame_state.logical_frame_composed = true;
}

bool ColumnMapClient::presentationSucceeded() const
{
    // CPU map composition has no GPU present fence and cannot count as a rendered game frame.
    return false;
}

void ColumnMapClient::followPlayerWhenOutsideWindow()
{
    if (!m_follow_player) {
        return;
    }

    std::optional<shared::Player> const player = predictedLocalPlayer();
    if (!player) {
        return;
    }

    int64_t const player_x = static_cast<int64_t>(std::floor(shared::playerPositionX(*player)));
    int64_t const player_y = static_cast<int64_t>(std::floor(shared::playerPositionY(*player)));
    WorldWindow const& current = m_presentation.window();
    uint32_t const world_x = shared::WorldBounds::wrapHorizontal(player_x);
    uint32_t const world_y = shared::WorldBounds::wrapHorizontal(player_y);
    uint32_t const origin_x = shared::WorldBounds::wrapHorizontal(current.origin_x);
    uint32_t const origin_y = shared::WorldBounds::wrapHorizontal(current.origin_y);
    uint32_t const x_offset = (world_x + shared::WorldExtent::WIDTH - origin_x)
        % shared::WorldExtent::WIDTH;
    uint32_t const y_offset = (world_y + shared::WorldExtent::WIDTH - origin_y)
        % shared::WorldExtent::WIDTH;
    if (x_offset < current.width && y_offset < current.height) {
        return;
    }

    static_cast<void>(setWindow(WorldWindow{
        .origin_x = player_x - static_cast<int64_t>(current.width / 2U),
        .origin_y = player_y - static_cast<int64_t>(current.height / 2U),
        .width = current.width,
        .height = current.height,
    }));
}

} // namespace client
