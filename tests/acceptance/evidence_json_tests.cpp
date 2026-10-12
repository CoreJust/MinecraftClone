#include <acceptance/EvidenceJson.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

TEST(EvidenceJson, IncludesVersionedMetadataTimingAndCounters)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "scenario",
        .scenario_version = "1",
        .profile = "flat2d-v1",
        .scenario_name = "canonical_sample.core",
        .failure = {},
        .seed = 42,
        .ticks = 6,
        .clients_requested = 1,
        .clients_accepted = 1,
        .inputs_sent = 1,
        .camera_relative_inputs = 1,
        .expectations_passed = 1,
        .authoritative_tick_ms = 100,
        .replay_id = "fnv1a64:0123456789abcdef",
        .elapsed = std::chrono::milliseconds{ 17 },
        .deadline = std::chrono::milliseconds{ 1'000 },
        .passed = true,
    };
    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"schema_version\": \"mc.runtime-evidence.v1\""), std::string::npos);
    EXPECT_NE(json.find("\"mode\": \"scenario\""), std::string::npos);
    EXPECT_NE(json.find("\"scenario_version\": \"1\""), std::string::npos);
    EXPECT_NE(json.find("\"profile\": \"flat2d-v1\""), std::string::npos);
    EXPECT_NE(json.find("\"seed\": 42"), std::string::npos);
    EXPECT_NE(json.find("\"replay_id\": \"fnv1a64:0123456789abcdef\""), std::string::npos);
    EXPECT_NE(json.find("\"elapsed_ms\": 17"), std::string::npos);
    EXPECT_NE(json.find("\"deadline_ms\": 1000"), std::string::npos);
    EXPECT_NE(json.find("\"ticks\": 6"), std::string::npos);
    EXPECT_NE(json.find("\"clients_accepted\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"camera_relative_inputs\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"authoritative_tick_ms\": 100"), std::string::npos);
    EXPECT_NE(json.find("\"passed\": true"), std::string::npos);
}

TEST(EvidenceJson, EscapesControlCharactersInStringFields)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "scenario\nsmoke",
        .scenario_version = {},
        .profile = {},
        .scenario_name = {},
        .failure = "unexpected \"position\"",
        .replay_id = {},
    };
    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"mode\": \"scenario\\nsmoke\""), std::string::npos);
    EXPECT_NE(json.find("\"failure\": \"unexpected \\\"position\\\"\""), std::string::npos);
}

TEST(EvidenceJson, LabelsBenchmarkSamplesAsCpuPresentationRequestsAndPhases)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "benchmark-render",
        .scenario_version = {},
        .profile = {},
        .scenario_name = {},
        .failure = {},
        .replay_id = {},
        .benchmark = acceptance::RendererBenchmarkEvidence{
            .package_id = {},
            .platform = {},
            .hardware = {},
            .gpu = {},
            .vulkan_api_version = {},
            .present_mode = {},
            .pipeline_path = {},
            .debug_hud_enabled = true,
            .presentation_requests_per_second = 120.0,
            .presentation_request_timings = acceptance::FrameTimingSummary{
                .sample_count = 2,
                .p50 = std::chrono::nanoseconds{ 7 },
            },
            .acquire_wait_timings = acceptance::FrameTimingSummary{
                .sample_count = 2,
                .p50 = std::chrono::nanoseconds{ 3 },
            },
            .command_record_timings = acceptance::FrameTimingSummary{
                .sample_count = 2,
                .p50 = std::chrono::nanoseconds{ 2 },
            },
            .complete_present_wait_timings = acceptance::FrameTimingSummary{
                .sample_count = 2,
                .p50 = std::chrono::nanoseconds{ 2 },
            },
        },
    };

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"presentation_requests_per_second\": 120.000000"), std::string::npos);
    EXPECT_NE(json.find("\"debug_hud_enabled\": true"), std::string::npos);
    EXPECT_NE(json.find("\"cpu_presentation_request_timings_ns\""), std::string::npos);
    EXPECT_NE(json.find("\"cpu_acquire_wait_timings_ns\""), std::string::npos);
    EXPECT_NE(json.find("\"cpu_command_record_timings_ns\""), std::string::npos);
    EXPECT_NE(json.find("\"cpu_complete_submit_present_wait_timings_ns\""), std::string::npos);
    EXPECT_EQ(json.find("\"frames_per_second\""), std::string::npos);
    EXPECT_EQ(json.find("\"frame_timings_ns\""), std::string::npos);
}

TEST(EvidenceJson, SeparatesFullGameRequestsFromPhysicalDisplayCadence)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "benchmark-game",
        .scenario_version = {},
        .profile = {},
        .scenario_name = {},
        .failure = {},
        .replay_id = {},
        .game_benchmark = acceptance::GameBenchmarkEvidence{
            .workload = "speed-200-movement-v2",
            .package_id = {},
            .hardware = {},
            .gpu = std::nullopt,
            .present_mode = {},
            .build_mode = {},
            .client_message_payload_bytes_sent = 128U,
            .client_message_payload_bytes_received = 256U,
            .excluded_present_requests = 1U,
            .warm_present_requests_per_second = 87.5,
            .uncapped_present_requests_per_second = 150.0,
            .cold_loop_timings = {},
            .warm_loop_timings = {},
            .uncapped_loop_timings = {},
            .server_tick_timings = {},
            .permission_publish_timings = {},
            .terrain_readiness = acceptance::GameBenchmarkTerrainReadinessEvidence{
                .center_sample_count = 380U,
                .center_change_count = 4U,
                .eligible_mesh_episode_count = 11U,
                .ready_mesh_episode_count = 9U,
                .left_near_unready_episode_count = 2U,
                .deadline_miss_count = 0U,
                .max_ready_latency_ns = 3'200'000'000U,
                .has_max_ready_latency = true,
                .max_ready_key_x = 98,
                .max_ready_key_y = -12,
                .max_ready_start_center_x = 94,
                .max_ready_start_center_y = -12,
                .accepted_near_admissions = 11U,
                .accepted_far_admissions = 6U,
                .far_admissions_while_near_pending = 0U,
                .final_desired_tile_count = 225U,
                .final_resident_tile_count = 225U,
                .final_current_mesh_visible_count = 225U,
                .final_center_valid = true,
                .complete = true,
            },
            .raw_frames = {
                acceptance::GameBenchmarkFrameSample{
                    .warm = true,
                    .presentation_succeeded = true,
                    .loop_duration_ns = 12'000U,
                    .network_event_count = 2U,
                    .terrain_near = acceptance::GameBenchmarkFrameSample::TerrainNearSample{
                        .center_valid = true,
                        .center_x = 12,
                        .center_y = -3,
                        .desired_tile_count = 225U,
                        .resident_tile_count = 220U,
                        .nonresident_tile_count = 5U,
                        .current_mesh_visible_count = 210U,
                        .pending_mesh_count = 10U,
                        .active_mesh_job_count = 5U,
                        .accepted_near_admission_delta = 2U,
                        .accepted_far_admission_delta = 1U,
                    },
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_terrain_duration_ns = std::nullopt,
                    .latest_gpu_frame_duration_ns = 42U,
                },
                acceptance::GameBenchmarkFrameSample{
                    .uncapped = true,
                    .presentation_succeeded = false,
                    .loop_duration_ns = 4'000U,
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_terrain_duration_ns = std::nullopt,
                    .latest_gpu_frame_duration_ns = std::nullopt,
                },
                acceptance::GameBenchmarkFrameSample{
                    .excluded = true,
                    .presentation_succeeded = true,
                    .loop_duration_ns = 5'000U,
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_terrain_duration_ns = std::nullopt,
                    .latest_gpu_frame_duration_ns = std::nullopt,
                },
            },
            .raw_server_tick_durations_ns = { 100U, 200U },
            .raw_permission_publish_durations_ns = {},
        },
    };

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"measurement_kind\": \"full-game-render-submit-present-request\""), std::string::npos);
    EXPECT_NE(json.find("\"workload\": \"speed-200-movement-v2\""), std::string::npos);
    EXPECT_NE(json.find("\"p99_8\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"displayed_cadence_hz\": null"), std::string::npos);
    EXPECT_NE(json.find("\"gpu\": null"), std::string::npos);
    EXPECT_NE(json.find("\"client_message_payload_bytes_sent\": 128"), std::string::npos);
    EXPECT_NE(json.find("\"client_message_payload_bytes_received\": 256"), std::string::npos);
    EXPECT_EQ(json.find("\"network_bytes_sent\""), std::string::npos);
    EXPECT_NE(json.find("\"excluded_present_requests\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"latest_gpu_frame_duration_ns\": 42"), std::string::npos);
    EXPECT_NE(json.find("\"warm_present_requests_per_second\": 87.500000"), std::string::npos);
    EXPECT_NE(json.find("\"uncapped_present_requests_per_second\": 150.000000"), std::string::npos);
    EXPECT_NE(json.find("\"phase\": \"warm\", \"presentation_succeeded\": true"), std::string::npos);
    EXPECT_NE(json.find("\"phase\": \"uncapped\", \"presentation_succeeded\": false"), std::string::npos);
    EXPECT_NE(json.find("\"phase\": \"excluded\", \"presentation_succeeded\": true"), std::string::npos);
    EXPECT_NE(json.find("\"terrain_near\": { \"center\": { \"x\": 12, \"y\": -3 }"),
        std::string::npos);
    EXPECT_NE(json.find("\"nonresident\": 5, \"current_mesh_visible\": 210"), std::string::npos);
    EXPECT_NE(json.find("\"terrain_readiness\": { \"contract\": \"mc.current-near-terrain-readiness.v1\""),
        std::string::npos);
    EXPECT_NE(json.find("\"readiness_deadline_kind\": \"diagnostic-progress-bound-not-performance-claim\""),
        std::string::npos);
    EXPECT_NE(json.find("\"worst_ready_latency_ns\": 3200000000"), std::string::npos);
    EXPECT_NE(json.find("\"far_mesh_admissions_while_near_pending\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"complete\": true"), std::string::npos);
    EXPECT_NE(json.find("\"raw_server_tick_durations_ns\": [100, 200]"), std::string::npos);
}

TEST(EvidenceJson, SeparatesFailedRenderAttemptsFromLaggedTerrainGpuSamples)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "benchmark-game",
        .scenario_version = {},
        .profile = {},
        .scenario_name = {},
        .failure = {},
        .replay_id = {},
        .game_benchmark = acceptance::GameBenchmarkEvidence{
            .workload = {},
            .package_id = {},
            .hardware = {},
            .gpu = std::nullopt,
            .present_mode = {},
            .build_mode = {},
            .cold_loop_timings = {},
            .warm_loop_timings = {},
            .uncapped_loop_timings = {},
            .server_tick_timings = {},
            .permission_publish_timings = {},
            .raw_frames = {
                acceptance::GameBenchmarkFrameSample{
                    .presentation_succeeded = false,
                    .visible_surface_face_count = 1'000U,
                    .diagnostic_render_attempt_id = 42U,
                    .diagnostic_frame_recorded = false,
                    .diagnostic_indexed_stone_quads = true,
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_camera = { 1.0, 2.0, 3.0, 90.0, -17.0, 0.0, 70.0 },
                    .diagnostic_cpu_acquire_wait_duration_ns = 10'000'000U,
                    .diagnostic_gpu_timestamps_enabled = true,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_sample_attempt_id = 40U,
                    .diagnostic_gpu_sample_slot = 1U,
                    .diagnostic_gpu_sample_quad_count = 18U,
                    .diagnostic_gpu_sample_draw_count = 3U,
                    .diagnostic_gpu_terrain_duration_ns = 5'000'000U,
                    .latest_gpu_frame_duration_ns = std::nullopt,
                },
            },
            .raw_server_tick_durations_ns = {},
            .raw_permission_publish_durations_ns = {},
        },
    };
    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"diagnostic_render_attempt_id\": 42"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_frame_recorded\": false"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_stone_indirect\": false"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_frame_slot\": null"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_submitted_stone_quad_count\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_stone_draw_count\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_camera\": [1, 2, 3, 90, -17, 0, 70]"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_acquire_wait_duration_ns\": 10000000"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_command_record_duration_ns\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_sample_attempt_id\": 40"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_sample_quad_count\": 18"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_terrain_duration_ns\": 5000000"), std::string::npos);
    EXPECT_NE(json.find("\"latest_gpu_frame_duration_ns\": null"), std::string::npos);
    EXPECT_NE(json.find("\"terrain_readiness\": null"), std::string::npos);
}

TEST(EvidenceJson, ReportsUnsupportedTerrainTimestampScopeWithoutInventingGpuDuration)
{
    acceptance::RuntimeEvidence evidence;
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->raw_frames.push_back({
        .diagnostic_frame_slot = std::nullopt,
        .diagnostic_gpu_timestamp_reason = "unsupported: MoltenVK may defer in-render-pass timestamps",
        .diagnostic_gpu_terrain_duration_ns = std::nullopt,
        .latest_gpu_frame_duration_ns = std::nullopt,
    });
    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"diagnostic_gpu_timestamps_enabled\": false"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_timestamp_reason\": \"unsupported: MoltenVK"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_terrain_duration_ns\": null"), std::string::npos);
    EXPECT_NE(json.find("\"latest_gpu_frame_duration_ns\": null"), std::string::npos);
}

TEST(EvidenceJson, ConvertsExpectedFailureToSerializableEvidence)
{
    acceptance::RuntimeEvidence const evidence = acceptance::collectRuntimeEvidence(
        "capture-render",
        []() -> std::expected<acceptance::RuntimeEvidence, std::string> {
            return std::unexpected("capture unavailable");
        }
    );

    EXPECT_EQ(evidence.mode, "capture-render");
    EXPECT_EQ(evidence.failure, "capture unavailable");
    EXPECT_FALSE(evidence.passed);
    EXPECT_NE(acceptance::evidenceJson(evidence).find("\"passed\": false"), std::string::npos);
}

TEST(EvidenceJson, ConvertsThrownFailureToSerializableEvidence)
{
    acceptance::RuntimeEvidence const evidence = acceptance::collectRuntimeEvidence(
        "benchmark-render",
        []() -> std::expected<acceptance::RuntimeEvidence, std::string> {
            throw std::runtime_error{ "device failed" };
        }
    );

    EXPECT_EQ(evidence.mode, "benchmark-render");
    EXPECT_EQ(evidence.failure, "device failed");
    EXPECT_FALSE(evidence.passed);
}

TEST(EvidenceJson, KeepsOriginalModeWhenFailureCallbackChangesCallerInput)
{
    std::string mode{ "capture-render" };
    acceptance::RuntimeEvidence const evidence = acceptance::collectRuntimeEvidence(
        mode,
        [&mode]() -> std::expected<acceptance::RuntimeEvidence, std::string> {
            mode = "changed";
            return std::unexpected("capture unavailable");
        }
    );

    EXPECT_EQ(mode, "changed");
    EXPECT_EQ(evidence.mode, "capture-render");
    EXPECT_EQ(evidence.failure, "capture unavailable");
    EXPECT_FALSE(evidence.passed);
}

} // namespace
