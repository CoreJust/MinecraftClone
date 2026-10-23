#pragma once

#include <client/ColumnMapPresentation.hpp>

#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <core/net/Address.hpp>

#include <acceptance/ReplayInput.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace acceptance
{

struct ColumnMapReplayOptions final
{
    core::Address server_address;
    std::string server_address_text;
    std::chrono::seconds duration{ 0 };
    GameBenchmarkWorkload workload = GameBenchmarkWorkload::DiagnosticStationary;
    std::filesystem::path evidence_directory;
};

struct ColumnMapReplaySnapshotMetadata final
{
    std::string file;
    std::string kind;
    uint64_t frame_sequence = 0U;
    uint64_t elapsed_ns = 0U;
    client::WorldWindow window;
    uint32_t scanned_columns = 0U;
    uint32_t ready_columns = 0U;
    uint32_t unknown_columns = 0U;
    std::string map_update_scope = "since-previous-snapshot-or-window-reset";
    uint32_t ready_pixels = 0U;
    uint32_t unknown_pixels = 0U;
    uint32_t updated_pixels = 0U;
    uint32_t active_materialized_columns = 0U;
    std::optional<uint64_t> active_coverage_epoch;
    std::optional<shared::PlayerPosition> active_coverage_center;
    std::optional<shared::TerrainIdentity> terrain_identity;
};

struct ColumnMapReplayPositionSample final
{
    uint64_t elapsed_ns = 0U;
    shared::PlayerPosition position{};
    uint8_t movement_capabilities = 0U;
};

struct ColumnMapReplayManifest final
{
    std::string result = "running";
    std::string server_address;
    std::string workload;
    uint64_t requested_duration_seconds = 0U;
    uint64_t elapsed_ns = 0U;
    uint64_t logical_loop_count = 0U;
    uint64_t logical_composition_count = 0U;
    uint64_t input_count = 0U;
    uint64_t authoritative_update_count = 0U;
    uint64_t completed_sweep_count = 0U;
    bool gpu_frames_claimed = false;
    std::string frame_semantics = "logical-cpu-column-map-composition-no-gpu-fence";
    GameBenchmarkStartActionEvidence controlled_start;
    std::string final_world_loading_state = "Idle";
    bool final_authoritative_world_available = false;
    std::optional<uint64_t> final_active_coverage_epoch;
    std::optional<shared::PlayerPosition> final_active_coverage_center;
    std::optional<shared::TerrainIdentity> final_terrain_identity;
    uint32_t final_playable_sphere_ready_chunks = 0U;
    uint32_t final_playable_sphere_required_chunks = 0U;
    bool final_playable_sphere_ready = false;
    std::vector<ColumnMapReplaySnapshotMetadata> snapshots;
    std::vector<ColumnMapReplayPositionSample> authoritative_positions;
    std::string failure;
};

[[nodiscard]]
std::expected<void, std::string> validateColumnMapReplayOptions(ColumnMapReplayOptions const& options);

// Pixel byte order: RGBA[4], material ID, coverage (Unknown=0/Ready=1),
// highest solid Z as signed int16 little-endian, updated mask, active-epoch mask.
[[nodiscard]]
std::expected<std::vector<uint8_t>, std::string> packColumnMapReplaySnapshot(
    client::ColumnMapPlanes const& planes,
    std::span<uint8_t const> updated_mask,
    std::span<uint8_t const> active_materialized_mask
);

[[nodiscard]]
std::string columnMapReplayManifestJson(ColumnMapReplayManifest const& manifest);

[[nodiscard]]
std::expected<void, std::string> runColumnMapReplay(ColumnMapReplayOptions const& options);

} // namespace acceptance
