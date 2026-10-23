#include <client/ColumnMapPresentation.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <core/net/Address.hpp>

#include <acceptance/ColumnMapReplay.hpp>
#include <acceptance/ReplayInput.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

acceptance::ColumnMapReplayOptions optionsAt(std::filesystem::path evidence_directory)
{
    return acceptance::ColumnMapReplayOptions{
        .server_address = core::Address::localhost(20'040U),
        .server_address_text = "127.0.0.1:20040",
        .duration = std::chrono::seconds{1},
        .workload = acceptance::GameBenchmarkWorkload::DiagnosticStationary,
        .evidence_directory = std::move(evidence_directory),
    };
}

std::filesystem::path uniqueMissingPath()
{
    return std::filesystem::temp_directory_path()
        / ("mc-column-map-replay-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        ));
}

} // namespace

TEST(ColumnMapReplay, PacksRawPlanesInDocumentedStablePixelOrder)
{
    client::ColumnMapPlanes planes{
        .rgba = {{1U, 2U, 3U, 4U}},
        .material_ids = {5U},
        .coverage = {client::ColumnCoverage::Ready},
        .highest_solid_z = {-2},
    };
    auto const packed = acceptance::packColumnMapReplaySnapshot(
        planes,
        std::vector<uint8_t>{1U},
        std::vector<uint8_t>{0U}
    );

    ASSERT_TRUE(packed);
    EXPECT_EQ(*packed, (std::vector<uint8_t>{1U, 2U, 3U, 4U, 5U, 1U, 0xfeU, 0xffU, 1U, 0U}));
}

TEST(ColumnMapReplay, ManifestPreservesLogicalFrameMeaningAndSourceIdentity)
{
    acceptance::ColumnMapReplayManifest manifest{
        .result = "failed",
        .server_address = "127.0.0.1:20040",
        .workload = "turning-x30-movement-v3",
        .requested_duration_seconds = 30U,
        .elapsed_ns = 31'000'000'000U,
        .logical_composition_count = 300U,
        .gpu_frames_claimed = false,
        .controlled_start = acceptance::GameBenchmarkStartActionEvidence{
            .synthetic_input_override_invoked = true,
            .eligible_prompt_rendered_offset_ns = 10U,
            .synthetic_input_requested_offset_ns = 20U,
            .key_accepted_offset_ns = 30U,
            .server_start_acknowledged_offset_ns = 40U,
            .input_ordinal = 4U,
            .complete = true,
        },
        .final_world_loading_state = "Playing",
        .final_active_coverage_epoch = 19U,
        .final_active_coverage_center = std::nullopt,
        .final_terrain_identity = shared::TerrainIdentity{
            .terrain_version = 7U,
            .revision = 5U,
            .seed = 42U,
            .configuration_identity = "canonical-test-world",
        },
        .final_playable_sphere_ready_chunks = 16U,
        .final_playable_sphere_required_chunks = 16U,
        .final_playable_sphere_ready = true,
        .snapshots = {acceptance::ColumnMapReplaySnapshotMetadata{
            .file = "final-window.bin",
            .kind = "final-window",
            .frame_sequence = 301U,
            .elapsed_ns = 30'999'000'000U,
            .window = client::WorldWindow{ .origin_x = 3, .origin_y = -2, .width = 256U, .height = 256U },
            .scanned_columns = 65'536U,
            .ready_columns = 60'000U,
            .unknown_columns = 5'536U,
            .ready_pixels = 60'000U,
            .unknown_pixels = 5'536U,
            .updated_pixels = 123U,
            .active_materialized_columns = 58'000U,
            .active_coverage_epoch = 19U,
            .active_coverage_center = std::nullopt,
            .terrain_identity = shared::TerrainIdentity{
                .terrain_version = 7U,
                .revision = 5U,
                .seed = 42U,
                .configuration_identity = "canonical-test-world",
            },
        }},
        .authoritative_positions = {},
        .failure = "test preserves failed evidence",
    };

    std::string const json = acceptance::columnMapReplayManifestJson(manifest);
    EXPECT_NE(json.find("\"gpu_frames_claimed\": false"), std::string::npos);
    EXPECT_NE(json.find("logical-cpu-column-map-composition-no-gpu-fence"), std::string::npos);
    EXPECT_NE(json.find("\"active_coverage_epoch\": 19"), std::string::npos);
    EXPECT_NE(json.find("\"configuration_identity\": \"canonical-test-world\""), std::string::npos);
    EXPECT_NE(json.find("\"updated_pixels\": 123"), std::string::npos);
    EXPECT_NE(json.find("\"unknown_pixels\": 5536"), std::string::npos);
    EXPECT_NE(json.find("\"elapsed_scope\": \"request_accepted_at-through-client-run-return-includes-connect-and-loading\""),
        std::string::npos);
    EXPECT_NE(json.find("test preserves failed evidence"), std::string::npos);
}

TEST(ColumnMapReplay, RejectsInvalidDurationWorkloadAndExistingEvidenceDirectory)
{
    auto invalid_duration = optionsAt(uniqueMissingPath());
    invalid_duration.duration = std::chrono::seconds{121};
    EXPECT_FALSE(acceptance::validateColumnMapReplayOptions(invalid_duration));

    auto invalid_workload = optionsAt(uniqueMissingPath());
    invalid_workload.workload = acceptance::GameBenchmarkWorkload::Speed200Movement;
    EXPECT_FALSE(acceptance::validateColumnMapReplayOptions(invalid_workload));

    std::filesystem::path const existing_directory = uniqueMissingPath();
    ASSERT_TRUE(std::filesystem::create_directory(existing_directory));
    auto existing_output = optionsAt(existing_directory);
    auto const validation = acceptance::validateColumnMapReplayOptions(existing_output);
    std::error_code cleanup_error;
    std::filesystem::remove(existing_directory, cleanup_error);
    EXPECT_FALSE(validation);
    EXPECT_NE(validation.error().find("already exists"), std::string::npos);
    EXPECT_FALSE(cleanup_error);
}

TEST(ColumnMapReplay, AcceptsTheCurrentElapsedTurningRouteContract)
{
    acceptance::ColumnMapReplayOptions replay_options = optionsAt(uniqueMissingPath());
    replay_options.workload = acceptance::GameBenchmarkWorkload::OrdinaryTurningMovement;

    EXPECT_TRUE(acceptance::validateColumnMapReplayOptions(replay_options));
    EXPECT_EQ(
        acceptance::gameBenchmarkWorkloadName(replay_options.workload),
        "turning-x30-movement-v3"
    );
}

TEST(ColumnMapReplay, UsesTheSharedX30TurningDirectionProfile)
{
    static constexpr std::chrono::seconds LOOK_LEFT_AT{ 11 };
    static constexpr std::chrono::seconds STRAFE_BACK_AT{ 19 };

    shared::Direction const initial = acceptance::gameBenchmarkDirection(
        acceptance::gameBenchmarkMovementRouteAt(std::chrono::seconds::zero())
    );
    shared::Direction const looking_left = acceptance::gameBenchmarkDirection(
        acceptance::gameBenchmarkMovementRouteAt(LOOK_LEFT_AT)
    );
    shared::Direction const strafing_back = acceptance::gameBenchmarkDirection(
        acceptance::gameBenchmarkMovementRouteAt(STRAFE_BACK_AT)
    );

    EXPECT_TRUE(initial.accelerated);
    EXPECT_EQ(initial.speedup, 30U);
    EXPECT_EQ(initial.y, 127U);
    EXPECT_EQ(initial.z, 127U);
    EXPECT_EQ(static_cast<int8_t>(looking_left.x), -89);
    EXPECT_EQ(looking_left.y, 89U);
    EXPECT_NE(strafing_back.x, looking_left.x);
    EXPECT_NE(strafing_back.y, looking_left.y);
    EXPECT_EQ(strafing_back.speedup, 30U);
}

TEST(ColumnMapReplay, RejectsMismatchedRawPlaneLengths)
{
    client::ColumnMapPlanes planes{
        .rgba = {{0U, 0U, 0U, 0U}},
        .material_ids = {},
        .coverage = {client::ColumnCoverage::Unknown},
        .highest_solid_z = {-1},
    };
    EXPECT_FALSE(acceptance::packColumnMapReplaySnapshot(
        planes,
        std::vector<uint8_t>{1U},
        std::vector<uint8_t>{0U}
    ));
}
