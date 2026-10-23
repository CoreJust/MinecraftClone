#include <acceptance/ColumnMapReplay.hpp>

#include <client/ColumnMapClient.hpp>
#include <client/ColumnMapPresentation.hpp>
#include <client/GameClient.hpp>
#include <client/WorldLoading.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <acceptance/ReplayInput.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace acceptance
{

namespace
{

using Clock = std::chrono::steady_clock;
constexpr char REPLAY_LOCAL_CHARACTER = '@';
constexpr double CLEARANCE_CORRIDOR_SAMPLE_SPACING_BLOCKS = 8.0;

std::string jsonString(std::string_view const value)
{
    std::string result{"\""};
    for (char const raw_character : value) {
        uint8_t const character = static_cast<uint8_t>(raw_character);
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) {
                    constexpr std::string_view HEX{"0123456789abcdef"};
                    result += "\\u00";
                    result += HEX[(static_cast<uint32_t>(character) >> 4U) & 0x0fU];
                    result += HEX[character & 0x0fU];
                } else {
                    result += static_cast<char>(character);
                }
        }
    }
    result += '"';
    return result;
}

std::string optionalUint64Json(std::optional<uint64_t> const value)
{
    return value ? std::to_string(*value) : "null";
}

std::string playerPositionJson(shared::PlayerPosition const& position)
{
    return "{ \"x\": " + std::to_string(position.x)
        + ", \"y\": " + std::to_string(position.y)
        + ", \"z\": " + std::to_string(position.z)
        + ", \"x_subcell\": " + std::to_string(position.x_subcell)
        + ", \"y_subcell\": " + std::to_string(position.y_subcell)
        + ", \"z_subcell\": " + std::to_string(position.z_subcell) + " }";
}

std::string optionalPlayerPositionJson(std::optional<shared::PlayerPosition> const& position)
{
    return position ? playerPositionJson(*position) : "null";
}

std::string terrainIdentityJson(std::optional<shared::TerrainIdentity> const& identity)
{
    if (!identity) {
        return "null";
    }
    return "{ \"terrain_version\": " + std::to_string(identity->terrain_version)
        + ", \"revision\": " + std::to_string(identity->revision)
        + ", \"seed\": " + std::to_string(identity->seed)
        + ", \"configuration_identity\": " + jsonString(identity->configuration_identity) + " }";
}

std::string worldLoadingStateName(client::WorldLoadingState const state)
{
    switch (state) {
        case client::WorldLoadingState::Idle: return "Idle";
        case client::WorldLoadingState::Creating: return "Creating";
        case client::WorldLoadingState::PreparingAtlas: return "PreparingAtlas";
        case client::WorldLoadingState::PreparingSpawn: return "PreparingSpawn";
        case client::WorldLoadingState::ReadyForStart: return "ReadyForStart";
        case client::WorldLoadingState::Starting: return "Starting";
        case client::WorldLoadingState::Playing: return "Playing";
        case client::WorldLoadingState::Failed: return "Failed";
        case client::WorldLoadingState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

uint64_t elapsedNanoseconds(Clock::time_point const started_at, Clock::time_point const sampled_at) noexcept
{
    return sampled_at > started_at
        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sampled_at - started_at).count())
        : 0U;
}

std::optional<uint64_t> offsetFromStart(
    Clock::time_point const started_at,
    std::optional<Clock::time_point> const sampled_at
) noexcept
{
    if (!sampled_at || *sampled_at < started_at) {
        return std::nullopt;
    }
    return elapsedNanoseconds(started_at, *sampled_at);
}

void observeStartAction(
    GameBenchmarkStartActionEvidence& evidence,
    client::WorldLoadingStatus const& status,
    Clock::time_point const started_at
)
{
    if (status.ready_prompt_frame_rendered_at && !evidence.eligible_prompt_rendered_offset_ns) {
        evidence.eligible_prompt_rendered_offset_ns =
            offsetFromStart(started_at, status.ready_prompt_frame_rendered_at);
    }
    if (status.key_accepted_at && !evidence.key_accepted_offset_ns) {
        evidence.key_accepted_offset_ns = offsetFromStart(started_at, status.key_accepted_at);
    }
    if (status.start_acknowledged_at && !evidence.server_start_acknowledged_offset_ns) {
        evidence.server_start_acknowledged_offset_ns =
            offsetFromStart(started_at, status.start_acknowledged_at);
    }
    evidence.complete = evidence.synthetic_input_override_invoked
        && evidence.input_ordinal
        && evidence.eligible_prompt_rendered_offset_ns
        && evidence.synthetic_input_requested_offset_ns
        && evidence.key_accepted_offset_ns
        && evidence.server_start_acknowledged_offset_ns
        && *evidence.eligible_prompt_rendered_offset_ns <= *evidence.synthetic_input_requested_offset_ns
        && *evidence.synthetic_input_requested_offset_ns <= *evidence.key_accepted_offset_ns
        && *evidence.key_accepted_offset_ns <= *evidence.server_start_acknowledged_offset_ns;
}

std::expected<void, std::string> writeTextFile(
    std::filesystem::path const& path,
    std::string const& text
)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected("could not open evidence file: " + path.string());
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        return std::unexpected("could not write evidence file: " + path.string());
    }
    return {};
}

std::expected<void, std::string> writeBinaryFile(
    std::filesystem::path const& path,
    std::span<uint8_t const> bytes
)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected("could not open raw map snapshot: " + path.string());
    }
    output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        return std::unexpected("could not write raw map snapshot: " + path.string());
    }
    return {};
}

uint32_t countOnes(std::span<uint8_t const> values) noexcept
{
    return static_cast<uint32_t>(std::ranges::count(values, uint8_t{1U}));
}

std::optional<uint16_t> deliveredRouteSurfaceHeight(
    client::ColumnMapClient const& map_client,
    shared::Player const& player,
    shared::Direction const route_direction
)
{
    auto const signedComponent = [](uint8_t const component) noexcept {
        return component <= 127U
            ? static_cast<int32_t>(component)
            : static_cast<int32_t>(component) - 256;
    };
    double direction_x = static_cast<double>(signedComponent(route_direction.x));
    double direction_y = static_cast<double>(signedComponent(route_direction.y));
    double const direction_length = std::hypot(direction_x, direction_y);
    if (direction_length > 0.0) {
        direction_x /= direction_length;
        direction_y /= direction_length;
    }

    std::optional<shared::TerrainIdentity> const identity = map_client.activeTerrainIdentity();
    std::optional<uint16_t> maximum_height;
    double const lookahead = direction_length > 0.0
        ? GAME_BENCHMARK_FLIGHT_CLEARANCE_LOOKAHEAD_BLOCKS
        : 0.0;
    // Missing resident tiles keep the corridor unknown; this never requests terrain work.
    uint32_t const sample_count = direction_length > 0.0
        ? static_cast<uint32_t>(lookahead / CLEARANCE_CORRIDOR_SAMPLE_SPACING_BLOCKS) + 1U
        : 1U;
    for (uint32_t sample = 0U; sample < sample_count; ++sample) {
        double const distance = static_cast<double>(sample) * CLEARANCE_CORRIDOR_SAMPLE_SPACING_BLOCKS;
        std::optional<uint16_t> const height = gameBenchmarkDeliveredSurfaceHeight(
            map_client.heightTileResidency(),
            identity ? &*identity : nullptr,
            shared::playerPositionX(player) + direction_x * distance,
            shared::playerPositionY(player) + direction_y * distance
        );
        if (!height) {
            return std::nullopt;
        }
        if (!maximum_height || *height > *maximum_height) {
            maximum_height = height;
        }
    }
    return maximum_height;
}

} // namespace

std::expected<void, std::string> validateColumnMapReplayOptions(ColumnMapReplayOptions const& options)
{
    if (options.duration < std::chrono::seconds{1} || options.duration > std::chrono::seconds{120}) {
        return std::unexpected("column-map replay duration must be from 1 to 120 seconds");
    }
    if (options.workload != GameBenchmarkWorkload::DiagnosticStationary
        && options.workload != GameBenchmarkWorkload::OrdinaryTurningMovement) {
        return std::unexpected("column-map replay supports only stationary or ordinary-turning workload");
    }
    if (options.server_address_text.empty()) {
        return std::unexpected("column-map replay requires the parsed server address text");
    }
    if (options.evidence_directory.empty()) {
        return std::unexpected("column-map replay requires a fresh evidence directory");
    }

    std::error_code error;
    bool const output_exists = std::filesystem::exists(options.evidence_directory, error);
    if (error) {
        return std::unexpected("could not inspect evidence directory: " + error.message());
    }
    if (output_exists) {
        return std::unexpected("column-map evidence directory already exists; refusing to overwrite it");
    }
    std::filesystem::path const parent = options.evidence_directory.parent_path().empty()
        ? std::filesystem::current_path(error)
        : options.evidence_directory.parent_path();
    if (error || !std::filesystem::is_directory(parent, error) || error) {
        return std::unexpected("column-map evidence directory parent must already exist");
    }
    return {};
}

std::expected<std::vector<uint8_t>, std::string> packColumnMapReplaySnapshot(
    client::ColumnMapPlanes const& planes,
    std::span<uint8_t const> const updated_mask,
    std::span<uint8_t const> const active_materialized_mask
)
{
    uint32_t const max_pixel_count = static_cast<uint32_t>(client::ColumnMapPresentation::MAX_TILE_SIDE)
        * client::ColumnMapPresentation::MAX_TILE_SIDE;
    if (planes.rgba.size() > max_pixel_count) {
        return std::unexpected("column-map plane exceeds the supported 256 by 256 window");
    }
    uint32_t const pixel_count = static_cast<uint32_t>(planes.rgba.size());
    if (pixel_count == 0U
        || planes.material_ids.size() != pixel_count
        || planes.coverage.size() != pixel_count
        || planes.highest_solid_z.size() != pixel_count
        || updated_mask.size() != pixel_count
        || active_materialized_mask.size() != pixel_count) {
        return std::unexpected("column-map planes and masks must have the same nonzero pixel count");
    }

    std::vector<uint8_t> result;
    result.reserve(static_cast<std::vector<uint8_t>::size_type>(pixel_count) * 10U);
    for (uint32_t index = 0U; index < pixel_count; ++index) {
        result.insert(result.end(), planes.rgba[index].begin(), planes.rgba[index].end());
        result.push_back(planes.material_ids[index]);
        result.push_back(static_cast<uint8_t>(planes.coverage[index]));
        uint16_t const z = static_cast<uint16_t>(planes.highest_solid_z[index]);
        result.push_back(static_cast<uint8_t>(z & 0xffU));
        result.push_back(static_cast<uint8_t>((static_cast<uint32_t>(z) >> 8U) & 0xffU));
        result.push_back(updated_mask[index] == 0U ? 0U : 1U);
        result.push_back(active_materialized_mask[index] == 0U ? 0U : 1U);
    }
    return result;
}

std::string columnMapReplayManifestJson(ColumnMapReplayManifest const& manifest)
{
    GameBenchmarkStartActionEvidence const& start = manifest.controlled_start;
    std::string result = "{\n"
        "  \"schema_version\": \"mc.column-map-replay.v1\",\n"
        "  \"result\": " + jsonString(manifest.result) + ",\n"
        "  \"server_address\": " + jsonString(manifest.server_address) + ",\n"
        "  \"workload\": " + jsonString(manifest.workload) + ",\n"
        "  \"requested_duration_seconds\": " + std::to_string(manifest.requested_duration_seconds) + ",\n"
        "  \"elapsed_ns\": " + std::to_string(manifest.elapsed_ns) + ",\n"
        "  \"elapsed_scope\": \"request_accepted_at-through-client-run-return-includes-connect-and-loading\",\n"
        "  \"logical_loop_count\": " + std::to_string(manifest.logical_loop_count) + ",\n"
        "  \"logical_composition_count\": " + std::to_string(manifest.logical_composition_count) + ",\n"
        "  \"input_count\": " + std::to_string(manifest.input_count) + ",\n"
        "  \"authoritative_update_count\": " + std::to_string(manifest.authoritative_update_count) + ",\n"
        "  \"completed_sweep_count\": " + std::to_string(manifest.completed_sweep_count) + ",\n"
        "  \"gpu_frames_claimed\": false,\n"
        "  \"frame_semantics\": " + jsonString(manifest.frame_semantics) + ",\n"
        "  \"controlled_start\": { \"contract\": \"mc.controlled-world-start.v1\", \"input_source\": "
        + jsonString(start.input_source)
        + ", \"production_path\": " + jsonString(start.production_path)
        + ", \"synthetic_input_override_invoked\": "
        + (start.synthetic_input_override_invoked ? "true" : "false")
        + ", \"eligible_prompt_rendered_offset_ns\": "
        + optionalUint64Json(start.eligible_prompt_rendered_offset_ns)
        + ", \"synthetic_input_requested_offset_ns\": "
        + optionalUint64Json(start.synthetic_input_requested_offset_ns)
        + ", \"key_accepted_offset_ns\": " + optionalUint64Json(start.key_accepted_offset_ns)
        + ", \"server_start_acknowledged_offset_ns\": "
        + optionalUint64Json(start.server_start_acknowledged_offset_ns)
        + ", \"input_ordinal\": " + optionalUint64Json(start.input_ordinal)
        + ", \"offset_origin\": \"ColumnMapReplay.request_accepted_at\", \"complete\": "
        + (start.complete ? "true" : "false") + " },\n"
        "  \"final_world_loading_state\": " + jsonString(manifest.final_world_loading_state) + ",\n"
        "  \"final_authoritative_world_available\": "
        + (manifest.final_authoritative_world_available ? "true" : "false") + ",\n"
        "  \"final_active_coverage_epoch\": " + optionalUint64Json(manifest.final_active_coverage_epoch) + ",\n"
        "  \"final_active_coverage_center\": " + optionalPlayerPositionJson(manifest.final_active_coverage_center) + ",\n"
        "  \"final_terrain_identity\": " + terrainIdentityJson(manifest.final_terrain_identity) + ",\n"
        "  \"final_playable_sphere\": { \"radius_blocks\": 320, \"ready_chunks\": "
        + std::to_string(manifest.final_playable_sphere_ready_chunks)
        + ", \"required_chunks\": " + std::to_string(manifest.final_playable_sphere_required_chunks)
        + ", \"ready\": " + (manifest.final_playable_sphere_ready ? "true" : "false") + " },\n"
        "  \"snapshot_format\": { \"pixel_order\": \"row-major\", \"bytes_per_pixel\": 10, "
        "\"fields\": [\"rgba8\", \"material_id_u8\", \"coverage_u8_unknown0_ready1\", "
        "\"highest_solid_z_i16_le\", \"updated_mask_u8\", \"active_materialized_column_mask_u8\"] },\n"
        "  \"snapshots\": [";
    bool first_snapshot = true;
    for (ColumnMapReplaySnapshotMetadata const& snapshot : manifest.snapshots) {
        if (!first_snapshot) {
            result += ',';
        }
        first_snapshot = false;
        result += "\n    { \"file\": " + jsonString(snapshot.file)
            + ", \"kind\": " + jsonString(snapshot.kind)
            + ", \"frame_sequence\": " + std::to_string(snapshot.frame_sequence)
            + ", \"elapsed_ns\": " + std::to_string(snapshot.elapsed_ns)
            + ", \"window\": { \"origin_x\": " + std::to_string(snapshot.window.origin_x)
            + ", \"origin_y\": " + std::to_string(snapshot.window.origin_y)
            + ", \"width\": " + std::to_string(snapshot.window.width)
            + ", \"height\": " + std::to_string(snapshot.window.height) + " }"
            + ", \"map_update\": { \"scanned_columns\": " + std::to_string(snapshot.scanned_columns)
            + ", \"ready_columns\": " + std::to_string(snapshot.ready_columns)
            + ", \"unknown_columns\": " + std::to_string(snapshot.unknown_columns)
            + ", \"scope\": " + jsonString(snapshot.map_update_scope) + " }"
            + ", \"ready_pixels\": " + std::to_string(snapshot.ready_pixels)
            + ", \"unknown_pixels\": " + std::to_string(snapshot.unknown_pixels)
            + ", \"updated_pixels\": " + std::to_string(snapshot.updated_pixels)
            + ", \"active_materialized_columns\": "
            + std::to_string(snapshot.active_materialized_columns)
            + ", \"active_coverage_epoch\": " + optionalUint64Json(snapshot.active_coverage_epoch)
            + ", \"active_coverage_center\": "
            + optionalPlayerPositionJson(snapshot.active_coverage_center)
            + ", \"terrain_identity\": " + terrainIdentityJson(snapshot.terrain_identity) + " }";
    }
    result += "\n  ],\n  \"authoritative_positions\": [";
    bool first_position = true;
    for (ColumnMapReplayPositionSample const& sample : manifest.authoritative_positions) {
        if (!first_position) {
            result += ',';
        }
        first_position = false;
        result += "\n    { \"elapsed_ns\": " + std::to_string(sample.elapsed_ns)
            + ", \"position\": " + playerPositionJson(sample.position)
            + ", \"movement_capabilities\": " + std::to_string(sample.movement_capabilities) + " }";
    }
    result += "\n  ],\n  \"failure\": " + jsonString(manifest.failure) + "\n}\n";
    return result;
}

std::expected<void, std::string> runColumnMapReplay(ColumnMapReplayOptions const& options)
{
    if (auto const valid = validateColumnMapReplayOptions(options); !valid) {
        return std::unexpected(valid.error());
    }
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(options.evidence_directory, filesystem_error) || filesystem_error) {
        return std::unexpected("could not create fresh evidence directory: "
            + (filesystem_error ? filesystem_error.message() : options.evidence_directory.string()));
    }

    Clock::time_point const started_at = Clock::now();
    Clock::time_point const deadline = started_at + options.duration;
    ColumnMapReplayManifest manifest{
        .result = "running",
        .server_address = options.server_address_text,
        .workload = std::string{gameBenchmarkWorkloadName(options.workload)},
        .requested_duration_seconds = static_cast<uint64_t>(options.duration.count()),
        .controlled_start = {},
        .final_active_coverage_epoch = std::nullopt,
        .final_active_coverage_center = std::nullopt,
        .final_terrain_identity = std::nullopt,
        .snapshots = {},
        .authoritative_positions = {},
        .failure = {},
    };
    std::optional<std::string> failure;
    std::optional<Clock::time_point> movement_started_at;
    std::optional<shared::Player> latest_authoritative_local_player;
    auto const persistManifest = [&]() -> std::expected<void, std::string> {
        return writeTextFile(options.evidence_directory / "manifest.json", columnMapReplayManifestJson(manifest));
    };
    if (auto const written = persistManifest(); !written) {
        return std::unexpected(written.error());
    }

    client::ColumnMapClient map_client{
        client::WorldWindow{ .origin_x = 0, .origin_y = 0, .width = 256U, .height = 256U },
        client::canonicalColumnMapPalette(),
        shared::WorldMode::Flight,
        shared::World::canonicalConfiguration(),
        true,
    };
    uint64_t loop_count = 0U;
    uint64_t logical_composition_count = 0U;
    uint64_t input_count = 0U;
    uint64_t authoritative_update_count = 0U;
    uint64_t next_snapshot_number = 1U;
    client::WorldWindow tracked_window = map_client.presentation().window();
    uint32_t updated_pixel_count = 0U;
    uint32_t sweep_scanned_columns = 0U;
    uint32_t sweep_ready_columns = 0U;
    uint32_t sweep_unknown_columns = 0U;
    std::vector<uint8_t> updated_mask(
        static_cast<std::vector<uint8_t>::size_type>(tracked_window.width) * tracked_window.height,
        0U
    );
    auto const addSnapshot = [&](std::string kind, Clock::time_point const sampled_at) -> std::expected<void, std::string> {
        client::ColumnMapPresentation const& presentation = map_client.presentation();
        client::ColumnMapPlanes const& planes = presentation.planes();
        client::ColumnMapFrameState const& state = map_client.frameState();
        auto packed = packColumnMapReplaySnapshot(
            planes,
            updated_mask,
            state.active_materialized_column_presence
        );
        if (!packed) {
            return std::unexpected(packed.error());
        }
        std::string filename;
        if (kind == "completed-sweep") {
            std::ostringstream name;
            name << "sweep-" << std::setw(6) << std::setfill('0') << next_snapshot_number << ".bin";
            filename = name.str();
            ++next_snapshot_number;
        } else {
            filename = "final-window.bin";
        }
        if (auto const written = writeBinaryFile(options.evidence_directory / filename, *packed); !written) {
            return std::unexpected(written.error());
        }
        uint32_t ready_pixels = 0U;
        for (client::ColumnCoverage const coverage : planes.coverage) {
            ready_pixels += coverage == client::ColumnCoverage::Ready ? 1U : 0U;
        }
        manifest.snapshots.push_back(ColumnMapReplaySnapshotMetadata{
            .file = filename,
            .kind = std::move(kind),
            .frame_sequence = state.sequence,
            .elapsed_ns = elapsedNanoseconds(started_at, sampled_at),
            .window = presentation.window(),
            .scanned_columns = sweep_scanned_columns,
            .ready_columns = sweep_ready_columns,
            .unknown_columns = sweep_unknown_columns,
            .ready_pixels = ready_pixels,
            .unknown_pixels = static_cast<uint32_t>(planes.coverage.size()) - ready_pixels,
            .updated_pixels = updated_pixel_count,
            .active_materialized_columns = countOnes(state.active_materialized_column_presence),
            .active_coverage_epoch = state.active_coverage_epoch,
            .active_coverage_center = state.active_coverage_center,
            .terrain_identity = state.terrain_identity,
        });
        return {};
    };

    auto const updateStartAction = [&](client::WorldLoadingStatus const& status) {
        observeStartAction(manifest.controlled_start, status, started_at);
    };
    client::GameClientBenchmarkHooks const hooks{
        .deadline = deadline,
        .should_stop = [&] {
            return failure.has_value() || Clock::now() >= deadline;
        },
        .is_uncapped_phase = {},
        .input_override = [&](uint64_t const ordinal) -> std::optional<shared::Direction> {
            if (map_client.worldLoadingStatus().state != client::WorldLoadingState::Playing) {
                return shared::Direction{};
            }
            if (options.workload != GameBenchmarkWorkload::OrdinaryTurningMovement) {
                return gameBenchmarkDirection(options.workload, ordinal);
            }
            static_cast<void>(ordinal);
            Clock::time_point const sampled_at = Clock::now();
            std::chrono::nanoseconds const elapsed = movement_started_at
                ? std::chrono::duration_cast<std::chrono::nanoseconds>(sampled_at - *movement_started_at)
                : std::chrono::nanoseconds::zero();
            GameBenchmarkMovementRouteSample route = gameBenchmarkRepeatingMovementRouteAt(
                elapsed
            );
            std::optional<double> player_z;
            std::optional<uint16_t> corridor_height;
            if (latest_authoritative_local_player) {
                player_z = shared::playerPositionZ(*latest_authoritative_local_player);
                corridor_height = deliveredRouteSurfaceHeight(
                    map_client,
                    *latest_authoritative_local_player,
                    gameBenchmarkDirection(route)
                );
            }
            route = gameBenchmarkApplyFlightClearance(route, player_z, corridor_height);
            return gameBenchmarkDirection(route);
        },
        .start_input_override = [&](uint64_t const ordinal) {
            GameBenchmarkStartActionEvidence& start = manifest.controlled_start;
            if (start.synthetic_input_override_invoked) {
                return false;
            }
            client::WorldLoadingStatus const status = map_client.worldLoadingStatus();
            if (status.state != client::WorldLoadingState::ReadyForStart
                || !status.ready_prompt_frame_rendered_at) {
                return false;
            }
            updateStartAction(status);
            start.synthetic_input_override_invoked = true;
            start.synthetic_input_requested_offset_ns = elapsedNanoseconds(started_at, Clock::now());
            start.input_ordinal = ordinal;
            return true;
        },
        .on_loop = [&](client::GameClientLoopSample const& sample) {
            ++loop_count;
            input_count += sample.input_sent ? 1U : 0U;
            client::ColumnMapFrameState const& state = map_client.frameState();
            logical_composition_count += state.logical_frame_composed ? 1U : 0U;
            updateStartAction(state.loading);

            if (state.logical_frame_composed) {
                client::ColumnMapPresentation const& presentation = map_client.presentation();
                client::WorldWindow const& current_window = presentation.window();
                if (current_window != tracked_window) {
                    tracked_window = current_window;
                    updated_mask.assign(
                        static_cast<std::vector<uint8_t>::size_type>(tracked_window.width)
                            * tracked_window.height,
                        0U
                    );
                    updated_pixel_count = 0U;
                    sweep_scanned_columns = 0U;
                    sweep_ready_columns = 0U;
                    sweep_unknown_columns = 0U;
                }
                sweep_scanned_columns += state.map_update.scanned_columns;
                sweep_ready_columns += state.map_update.ready_columns;
                sweep_unknown_columns += state.map_update.unknown_columns;
                uint32_t const pixel_count = static_cast<uint32_t>(updated_mask.size());
                uint32_t const scanned = std::min(state.map_update.scanned_columns, pixel_count);
                if (pixel_count != 0U && scanned != 0U) {
                    uint32_t const next = presentation.nextColumnIndex();
                    uint32_t const first = (next + pixel_count - scanned) % pixel_count;
                    for (uint32_t offset = 0U; offset < scanned; ++offset) {
                        uint8_t& updated = updated_mask[(first + offset) % pixel_count];
                        if (updated == 0U) {
                            updated = 1U;
                            ++updated_pixel_count;
                        }
                    }
                }
                bool const final_window = Clock::now() >= deadline;
                bool const complete_sweep = updated_pixel_count == updated_mask.size();
                if (final_window) {
                    if (auto const added = addSnapshot("final-window", sample.completed_at); !added) {
                        failure = added.error();
                    }
                } else if (complete_sweep) {
                    if (auto const added = addSnapshot("completed-sweep", sample.completed_at); !added) {
                        failure = added.error();
                    } else {
                        ++manifest.completed_sweep_count;
                        std::ranges::fill(updated_mask, uint8_t{0U});
                        updated_pixel_count = 0U;
                        sweep_scanned_columns = 0U;
                        sweep_ready_columns = 0U;
                        sweep_unknown_columns = 0U;
                    }
                }
            }
            manifest.logical_loop_count = loop_count;
            manifest.logical_composition_count = logical_composition_count;
            manifest.input_count = input_count;
            manifest.authoritative_update_count = authoritative_update_count;
        },
        .on_authoritative_player = [&](shared::Player const& player) {
            if (player.ch == REPLAY_LOCAL_CHARACTER) {
                latest_authoritative_local_player = player;
            }
            ++authoritative_update_count;
            manifest.authoritative_update_count = authoritative_update_count;
            manifest.authoritative_positions.push_back(ColumnMapReplayPositionSample{
                .elapsed_ns = elapsedNanoseconds(started_at, Clock::now()),
                .position = shared::PlayerPosition{
                    .x = player.x,
                    .y = player.y,
                    .z = player.z,
                    .x_subcell = player.x_subcell,
                    .y_subcell = player.y_subcell,
                    .z_subcell = player.z_subcell,
                },
                .movement_capabilities = player.movement_capabilities.bits,
            });
        },
        .on_input_sent = [&] (
            shared::ClientInputMessage const&,
            Clock::time_point const sent_at,
            Clock::time_point const
        ) {
            if (options.workload != GameBenchmarkWorkload::OrdinaryTurningMovement) {
                return;
            }
            if (!movement_started_at) {
                movement_started_at = sent_at;
            }
        },
        .on_authoritative_state = {},
    };

    try {
        map_client.run(options.server_address, REPLAY_LOCAL_CHARACTER, &hooks, started_at);
    } catch (std::exception const& exception) {
        failure = exception.what();
    }

    client::ColumnMapFrameState const& final_state = map_client.frameState();
    updateStartAction(final_state.loading);
    manifest.elapsed_ns = elapsedNanoseconds(started_at, Clock::now());
    manifest.logical_loop_count = loop_count;
    manifest.logical_composition_count = logical_composition_count;
    manifest.input_count = input_count;
    manifest.authoritative_update_count = authoritative_update_count;
    manifest.final_world_loading_state = worldLoadingStateName(final_state.loading.state);
    manifest.final_authoritative_world_available = final_state.authoritative_world_available;
    manifest.final_active_coverage_epoch = final_state.active_coverage_epoch;
    manifest.final_active_coverage_center = final_state.active_coverage_center;
    manifest.final_terrain_identity = final_state.terrain_identity;
    manifest.final_playable_sphere_ready_chunks = final_state.loading.playable_sphere_ready_chunks;
    manifest.final_playable_sphere_required_chunks = final_state.loading.playable_sphere_required_chunks;
    manifest.final_playable_sphere_ready = final_state.loading.playable_sphere_ready;
    if (!failure && !gameBenchmarkStartActionComplete(manifest.controlled_start)) {
        failure = "production loading start did not complete through the synthetic controlled-key path";
    }
    if (!failure && logical_composition_count == 0U) {
        failure = "the run produced no logical column-map compositions";
    }
    if (final_state.logical_frame_composed
        && (manifest.snapshots.empty() || manifest.snapshots.back().kind != "final-window")) {
        auto const added = addSnapshot("final-window", final_state.composed_at);
        if (!added) {
            failure = added.error();
        }
    }
    manifest.failure = failure.value_or(std::string{});
    manifest.result = failure ? "failed" : "completed";
    if (auto const written = persistManifest(); !written) {
        return std::unexpected(written.error());
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return {};
}

} // namespace acceptance
