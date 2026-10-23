#pragma once

#include <client/ColumnMapPresentation.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>

#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace client
{

struct ColumnMapFrameState final
{
    uint64_t sequence = 0U;
    std::chrono::steady_clock::time_point composed_at{};
    ColumnMapUpdate map_update;
    WorldLoadingStatus loading;
    std::optional<shared::TerrainIdentity> terrain_identity;
    std::optional<uint64_t> active_coverage_epoch;
    std::optional<shared::PlayerPosition> active_coverage_center;
    // 1 means an active-epoch chunk exists at any z; 0 may be absent or not resampled yet.
    std::vector<uint8_t> active_materialized_column_presence;
    bool authoritative_world_available = false;
    bool logical_frame_composed = false;
};

[[nodiscard]]
ColumnMapPalette canonicalColumnMapPalette() noexcept;

class ColumnMapClient : public GameClient
{
public:
    explicit ColumnMapClient(
        WorldWindow initial_window,
        ColumnMapPalette palette,
        shared::WorldMode mode = shared::WorldMode::Flight,
        shared::WorldConfiguration configuration = shared::World::canonicalConfiguration(),
        bool follow_player = true
    );

    [[nodiscard]]
    bool setWindow(WorldWindow window);
    void setPlayerFollowing(bool enabled) noexcept;

    [[nodiscard]]
    ColumnMapPresentation const& presentation() const noexcept;
    [[nodiscard]]
    ColumnMapFrameState const& frameState() const noexcept;

protected:
    [[nodiscard]] shared::Direction input() override;
    void render() override;
    [[nodiscard]] bool presentationSucceeded() const override;

private:
    void followPlayerWhenOutsideWindow();

private:
    ColumnMapPresentation m_presentation;
    ColumnMapFrameState m_frame_state;
    std::chrono::steady_clock::time_point m_last_request_accepted_at{};
    bool m_follow_player;
};

} // namespace client
