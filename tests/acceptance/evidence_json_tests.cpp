#include <client/render/StoneFaceCapacity.hpp>

#include <acceptance/EvidenceJson.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

TEST(EvidenceJson, IdentifiesGameplayQualificationAndRawLoopOrder)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark = acceptance::GameBenchmarkEvidence{};
    evidence.game_benchmark->qualification_proof = "terrain-coverage";
    evidence.game_benchmark->raw_frames.push_back({ .loop_ordinal = 42U });
    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"qualification_proof\": \"terrain-coverage\""), std::string::npos);
    EXPECT_NE(json.find("\"loop_ordinal\": 42"), std::string::npos);
}

TEST(EvidenceJson, SerializesDriverPresentationHistoryAndRawFrameIdentity)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark = acceptance::GameBenchmarkEvidence{};
    acceptance::GameBenchmarkEvidence& benchmark = *evidence.game_benchmark;
    benchmark.presentation_timing = acceptance::GameBenchmarkPresentationTimingEvidence{};
    benchmark.presentation_timing->capability_supported = true;
    benchmark.presentation_timing->successful_requests.push_back({
        .request = { .swapchain_generation = 9U, .application_present_id = 17U },
        .loop_ordinal = 42U,
        .phase = "baseline-stationary",
    });
    benchmark.presentation_timing->records.push_back({
        .request = { .swapchain_generation = 9U, .application_present_id = 17U },
        .actual_present_time_ns = 123'456U,
    });
    benchmark.presentation_timing->polls.push_back({
        .scope = acceptance::GameBenchmarkPresentationPollScope::Loop,
        .status = acceptance::GameBenchmarkPresentationPollStatus::Records,
        .vk_result = 0,
        .returned_count = 1U,
    });
    benchmark.presentation_timing->capture_endpoint = acceptance::GameBenchmarkPresentationCaptureEndpoint{
        .request = { .swapchain_generation = 9U, .application_present_id = 17U },
        .loop_ordinal = 42U,
    };
    benchmark.presentation_timing->drain_complete = true;
    benchmark.raw_frames.push_back({
        .loop_ordinal = 42U,
        .presentation_succeeded = true,
        .swapchain_generation = 9U,
        .application_present_id = 17U,
    });

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"contract\": \"mc.actual-presentation-timing.v1\""), std::string::npos);
    EXPECT_NE(json.find("\"provider\": \"VK_GOOGLE_display_timing\""), std::string::npos);
    EXPECT_NE(json.find("\"source\": \"driver-reported\""), std::string::npos);
    EXPECT_NE(json.find("\"capability\": \"supported\""), std::string::npos);
    EXPECT_NE(json.find("\"actual_present_time_ns\": 123456"), std::string::npos);
    EXPECT_NE(json.find("\"scope\": \"loop\", \"status\": \"records\", \"vk_result\": 0, "
        "\"returned_count\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"swapchain_generation\": 9, \"application_present_id\": 17"),
        std::string::npos);
    EXPECT_NE(json.find("\"loop_ordinal\": 42, \"phase\": \"baseline-stationary\""),
        std::string::npos);
    EXPECT_NE(json.find("\"presentation_succeeded\": true, \"swapchain_generation\": 9, "
        "\"application_present_id\": 17"), std::string::npos);
    EXPECT_NE(json.find("\"drain\": { \"complete\": true, \"request_ids\": ["), std::string::npos);
}

TEST(EvidenceJson, SerializesDiagnosticOverridePresenceAsBooleans)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark = acceptance::GameBenchmarkEvidence{};
    EXPECT_NE(acceptance::evidenceJson(evidence).find("\"diagnostic_overrides_present\": false"),
        std::string::npos);
    evidence.game_benchmark->diagnostic_overrides_present = true;
    EXPECT_NE(acceptance::evidenceJson(evidence).find("\"diagnostic_overrides_present\": true"),
        std::string::npos);
}

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
            .qualification_proof = {},
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
            .full_radius_terrain = std::nullopt,
            .raw_frames = {
                acceptance::GameBenchmarkFrameSample{
                    .warm = true,
                    .presentation_succeeded = true,
                    .loop_duration_ns = 12'000U,
                    .network_event_count = 2U,
                    .terrain_phase = {},
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
                        .mesh_work_stages = acceptance::GameBenchmarkMeshStageSummary{
                            .client_pending_admission_count = 3U,
                            .worker_queued_count = 2U,
                            .worker_building_count = 1U,
                            .completed_result_waiting_count = 4U,
                            .oldest_client_pending_near_observed_age_ns = 11U,
                            .oldest_worker_queued_age_ns = 22U,
                            .oldest_worker_building_age_ns = 33U,
                            .oldest_completed_result_waiting_age_ns = 44U,
                        },
                        .mesh_result_drain = acceptance::GameBenchmarkMeshResultDrainSummary{
                            .drained_result_count = 10U,
                            .stale_epoch_discard_count = 2U,
                            .no_longer_interested_or_resident_discard_count = 1U,
                            .source_mismatch_requeue_count = 2U,
                            .target_detail_mismatch_requeue_count = 1U,
                            .cannot_publish_requeue_count = 1U,
                            .publish_attempt_count = 3U,
                            .publish_success_count = 2U,
                            .publish_failure_count = 1U,
                            .seam_planning_duration_ns = 55U,
                            .renderer_batch_upsert_duration_ns = 66U,
                            .renderer_free_range_prepare_duration_ns = 67U,
                            .renderer_mesh_prepare_duration_ns = 68U,
                            .renderer_capacity_prepare_duration_ns = 69U,
                            .renderer_commit_upload_duration_ns = 70U,
                        },
                    },
                    .renderer_stone_face_growth = std::nullopt,
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_terrain_duration_ns = std::nullopt,
                    .latest_gpu_frame_duration_ns = 42U,
                },
                acceptance::GameBenchmarkFrameSample{
                    .uncapped = true,
                    .presentation_succeeded = false,
                    .loop_duration_ns = 4'000U,
                    .terrain_phase = {},
                    .terrain_near = acceptance::GameBenchmarkFrameSample::TerrainNearSample{},
                    .renderer_stone_face_growth = std::nullopt,
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_terrain_duration_ns = std::nullopt,
                    .latest_gpu_frame_duration_ns = std::nullopt,
                },
                acceptance::GameBenchmarkFrameSample{
                    .excluded = true,
                    .presentation_succeeded = true,
                    .loop_duration_ns = 5'000U,
                    .terrain_phase = {},
                    .terrain_near = std::nullopt,
                    .renderer_stone_face_growth = std::nullopt,
                    .diagnostic_frame_slot = std::nullopt,
                    .diagnostic_gpu_timestamp_reason = {},
                    .diagnostic_gpu_terrain_duration_ns = std::nullopt,
                    .latest_gpu_frame_duration_ns = std::nullopt,
                },
            },
            .raw_server_stream_samples = {},
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
    EXPECT_NE(json.find("\"phase\": \"warm\", \"loop_ordinal\": 0, "
        "\"presentation_succeeded\": true"), std::string::npos);
    EXPECT_NE(json.find("\"phase\": \"uncapped\", \"loop_ordinal\": 0, "
        "\"presentation_succeeded\": false"), std::string::npos);
    EXPECT_NE(json.find("\"phase\": \"excluded\", \"loop_ordinal\": 0, "
        "\"presentation_succeeded\": true"), std::string::npos);
    EXPECT_NE(json.find("\"terrain_near\": { \"center\": { \"x\": 12, \"y\": -3 }"),
        std::string::npos);
    EXPECT_NE(json.find("\"nonresident\": 5, \"current_mesh_visible\": 210"), std::string::npos);
    EXPECT_NE(json.find("\"mesh_work_stages\": { \"client_pending_admission_count\": 3"),
        std::string::npos);
    EXPECT_NE(json.find("\"oldest_worker_queued_age_ns\": 22"), std::string::npos);
    EXPECT_NE(json.find("\"oldest_completed_result_waiting_age_ns\": 44"), std::string::npos);
    EXPECT_NE(json.find("\"mesh_result_drain\": { \"drained_result_count\": 10"),
        std::string::npos);
    EXPECT_NE(json.find("\"stale_epoch_discard_count\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"no_longer_interested_or_resident_discard_count\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"source_mismatch_requeue_count\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"target_detail_mismatch_requeue_count\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"cannot_publish_requeue_count\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"publish_attempt_count\": 3, \"publish_success_count\": 2, \"publish_failure_count\": 1"),
        std::string::npos);
    EXPECT_NE(json.find("\"seam_planning_duration_ns\": 55"), std::string::npos);
    EXPECT_NE(json.find("\"renderer_batch_upsert_duration_ns\": 66"), std::string::npos);
    EXPECT_NE(json.find("\"renderer_free_range_prepare_duration_ns\": 67"), std::string::npos);
    EXPECT_NE(json.find("\"renderer_mesh_prepare_duration_ns\": 68"), std::string::npos);
    EXPECT_NE(json.find("\"renderer_capacity_prepare_duration_ns\": 69"), std::string::npos);
    EXPECT_NE(json.find("\"renderer_commit_upload_duration_ns\": 70"), std::string::npos);
    EXPECT_NE(json.find("\"mesh_result_drain\": { \"drained_result_count\": 0"),
        std::string::npos);
    EXPECT_NE(json.find("\"terrain_readiness\": { \"contract\": \"mc.current-near-terrain-readiness.v1\""),
        std::string::npos);
    EXPECT_NE(json.find("\"readiness_deadline_kind\": \"diagnostic-progress-bound-not-performance-claim\""),
        std::string::npos);
    EXPECT_NE(json.find("\"worst_ready_latency_ns\": 3200000000"), std::string::npos);
    EXPECT_NE(json.find("\"far_mesh_admissions_while_near_pending\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"complete\": true"), std::string::npos);
    EXPECT_NE(json.find("\"raw_server_tick_durations_ns\": [100, 200]"), std::string::npos);
}

TEST(EvidenceJson, SerializesServerNearWindowStagesOnTheBenchmarkFrameTimeline)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.mode = "benchmark-game";
    acceptance::GameBenchmarkEvidence benchmark{};
    benchmark.workload = "speed-200-full-radius-recovery-v1";
    benchmark.raw_server_stream_samples.push_back({
        .started_offset_ns = 12'500U,
        .center_x = 144,
        .center_y = -7,
        .near_desired_count = 225U,
        .near_resident_count = 188U,
        .near_queued_count = 5U,
        .near_dispatched_count = 21U,
        .near_ready_count = 4U,
        .near_inflight_count = 3U,
        .near_inflight_removals_count = 2U,
        .near_untracked_count = 4U,
        .server_queued_count = 19U,
        .server_dispatched_count = 42U,
        .server_ready_count = 8U,
        .server_inflight_addition_count = 6U,
        .server_inflight_removal_count = 7U,
        .server_inflight_delivery_count = 4U,
        .server_pending_removal_count = 9U,
        .server_resident_count = 205'861U,
        .server_admitted_addition_count = 11U,
        .server_rejected_delivery_ack_count = 1U,
        .server_delivery_credit_count = 2U,
        .worker_queued_count = 12U,
        .worker_running_count = 8U,
        .worker_completed_uncollected_count = 5U,
    });
    evidence.game_benchmark = std::move(benchmark);

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"raw_server_stream_samples\": ["), std::string::npos);
    EXPECT_NE(json.find("\"started_offset_ns\": 12500, \"center\": { \"x\": 144, \"y\": -7 }"),
        std::string::npos);
    EXPECT_NE(json.find("\"near_window\": { \"desired\": 225, \"resident\": 188"),
        std::string::npos);
    EXPECT_NE(json.find("\"inflight_removals\": 2, \"untracked\": 4 }, \"server_queued\": 19"),
        std::string::npos);
    EXPECT_NE(json.find("\"server_inflight_additions\": 6, \"server_inflight_removals\": 7"),
        std::string::npos);
    EXPECT_NE(json.find("\"server_inflight_deliveries\": 4, \"server_pending_removals\": 9"),
        std::string::npos);
    EXPECT_NE(json.find("\"server_resident\": 205861, \"server_admitted_additions\": 11"),
        std::string::npos);
    EXPECT_NE(json.find("\"server_rejected_delivery_acks\": 1, \"server_delivery_credits\": 2"),
        std::string::npos);
    EXPECT_NE(json.find("\"worker_queued\": 12, \"worker_running\": 8, \"worker_completed_uncollected\": 5"),
        std::string::npos);
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
            .qualification_proof = {},
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
            .terrain_readiness = std::nullopt,
            .full_radius_terrain = std::nullopt,
            .raw_frames = {
                acceptance::GameBenchmarkFrameSample{
                    .presentation_succeeded = false,
                    .visible_surface_face_count = 1'000U,
                    .terrain_phase = {},
                    .terrain_near = std::nullopt,
                    .renderer_stone_face_growth = std::nullopt,
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
                    .diagnostic_terrain_occlusion_enabled = true,
                    .diagnostic_terrain_occlusion_oracle_frame = true,
                    .diagnostic_terrain_occlusion_filtered_frame = false,
                    .diagnostic_terrain_occlusion_removed_quad_count = 18U,
                    .diagnostic_terrain_occlusion_removed_range_count = 2U,
                    .diagnostic_terrain_occlusion_removed_command_count = 3U,
                },
            },
            .raw_server_stream_samples = {},
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
    EXPECT_NE(json.find("\"diagnostic_cpu_visible_range_collection_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_height_tile_pending_range_order_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_height_tile_visible_range_merge_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_indirect_prepare_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_stone_scene_record_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_command_record_residual_duration_ns\": 0,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_sample_attempt_id\": 40"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_sample_quad_count\": 18"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_gpu_terrain_duration_ns\": 5000000"), std::string::npos);
    EXPECT_NE(json.find("\"latest_gpu_frame_duration_ns\": null"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_terrain_occlusion_enabled\": true"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_terrain_occlusion_oracle_frame\": true"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_terrain_occlusion_filtered_frame\": false"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_terrain_occlusion_removed_quad_count\": 18"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_terrain_occlusion_removed_range_count\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_terrain_occlusion_removed_command_count\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"terrain_readiness\": null"), std::string::npos);
}

TEST(EvidenceJson, SerializesSeparateCpuRecordingPhasesInNanoseconds)
{
    static constexpr uint64_t COLLECTION_NS = 1'234U;
    static constexpr uint64_t PREPARE_NS = 5'678U;
    static constexpr uint64_t STONE_SCENE_NS = 9'012U;
    static constexpr uint64_t RESIDUAL_NS = 3'456U;
    static constexpr uint64_t TILE_TRAVERSAL_NS = 2'345U;
    static constexpr uint64_t RANGE_ORDER_NS = 3'456U;
    static constexpr uint64_t RANGE_MERGE_NS = 4'567U;
    acceptance::GameBenchmarkFrameSample sample;
    sample.diagnostic_cpu_visible_range_collection_duration_ns = COLLECTION_NS;
    sample.diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns = TILE_TRAVERSAL_NS;
    sample.diagnostic_cpu_height_tile_pending_range_order_duration_ns = RANGE_ORDER_NS;
    sample.diagnostic_cpu_height_tile_visible_range_merge_duration_ns = RANGE_MERGE_NS;
    sample.diagnostic_cpu_indirect_prepare_duration_ns = PREPARE_NS;
    sample.diagnostic_cpu_stone_scene_record_duration_ns = STONE_SCENE_NS;
    sample.diagnostic_cpu_command_record_residual_duration_ns = RESIDUAL_NS;
    acceptance::RuntimeEvidence evidence;
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->raw_frames.push_back(sample);

    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"diagnostic_cpu_visible_range_collection_duration_ns\": 1234,"), std::string::npos);
    EXPECT_NE(
        json.find("\"diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns\": 2345,"),
        std::string::npos
    );
    EXPECT_NE(json.find("\"diagnostic_cpu_height_tile_pending_range_order_duration_ns\": 3456,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_height_tile_visible_range_merge_duration_ns\": 4567,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_indirect_prepare_duration_ns\": 5678,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_stone_scene_record_duration_ns\": 9012,"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_command_record_residual_duration_ns\": 3456,"), std::string::npos);
}

TEST(EvidenceJson, SerializesPresentationAcquisitionStagesAndResults)
{
    acceptance::GameBenchmarkFrameSample sample;
    sample.diagnostic_presentation_acquire = {
        .captured = true,
        .frame_slot = 2U,
        .stage_count = 2U,
        .stages = {
            acceptance::GameBenchmarkPresentationAcquireStageSample{
                .stage = acceptance::GameBenchmarkPresentationAcquireStage::SubmittedFrameFence,
                .duration_ns = 7'000'000U,
                .result = 0,
            },
            acceptance::GameBenchmarkPresentationAcquireStageSample{
                .stage = acceptance::GameBenchmarkPresentationAcquireStage::SwapchainImage,
                .duration_ns = 100'000U,
                .result = 0,
            },
        },
        .image_index = 1U,
    };
    acceptance::RuntimeEvidence evidence;
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->raw_frames.push_back(sample);

    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"diagnostic_presentation_acquire\": { \"captured\": true"), std::string::npos);
    EXPECT_NE(json.find("\"frame_slot\": 2, \"image_index\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"stage\": \"SubmittedFrameFence\", \"duration_ns\": 7000000"), std::string::npos);
    EXPECT_NE(json.find("\"vk_result\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"stage\": \"SwapchainImage\", \"duration_ns\": 100000"), std::string::npos);
}

TEST(EvidenceJson, ReportsUnsupportedTerrainTimestampScopeWithoutInventingGpuDuration)
{
    acceptance::RuntimeEvidence evidence;
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->raw_frames.push_back({
        .terrain_phase = {},
        .terrain_near = std::nullopt,
        .renderer_stone_face_growth = std::nullopt,
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

TEST(EvidenceJson, KeepsUnscannedFullRadiusCountsUnknownAndSerializesScannedRadialBands)
{
    acceptance::GameBenchmarkEvidence benchmark{};
    benchmark.workload = "speed-200-full-radius-recovery-v1";
    benchmark.full_radius_terrain.emplace();
    benchmark.full_radius_terrain->initial_barrier.phase = "initial-full-radius-warmup";
    benchmark.full_radius_terrain->initial_barrier.scan_performed = false;
    benchmark.full_radius_terrain->initial_barrier.exact_current_coverage = std::nullopt;
    benchmark.full_radius_terrain->recovery_barrier.phase = "full-radius-recovery-warmup";
    benchmark.full_radius_terrain->recovery_barrier.scan_performed = true;
    benchmark.full_radius_terrain->recovery_barrier.exact_current_coverage = false;
    benchmark.full_radius_terrain->recovery_barrier.resident_desired_count = 204'800U;
    benchmark.full_radius_terrain->recovery_barrier.radial_bands[15U].desired_count = 12'345U;
    benchmark.full_radius_terrain->recovery_barrier.radial_bands[15U].resident_count = 12'344U;
    benchmark.full_radius_terrain->recovery_barrier.radial_bands[15U].nonresident_count = 1U;
    benchmark.full_radius_terrain->recovery_barrier.radial_bands[15U].current_mesh_visible_count = 12'340U;
    benchmark.full_radius_terrain->recovery_barrier.radial_bands[15U].stale_or_missing_mesh_count = 4U;
    benchmark.full_radius_terrain->recovery_barrier.current_near_mesh_work_stages = {
        .client_pending_admission_count = 5U,
        .worker_queued_count = 4U,
        .worker_building_count = 3U,
        .completed_result_waiting_count = 2U,
        .oldest_client_pending_near_observed_age_ns = 101U,
        .oldest_worker_queued_age_ns = 202U,
        .oldest_worker_building_age_ns = 303U,
        .oldest_completed_result_waiting_age_ns = 404U,
    };
    benchmark.full_radius_terrain->recovery_barrier.current_visible_mesh_detail_counts =
        std::array<uint32_t, 5U>{ 1U, 2U, 3U, 4U, 5U };
    benchmark.full_radius_terrain->recovery_barrier.unclassified_current_visible_mesh_detail_count = 6U;
    benchmark.raw_frames.push_back({
        .terrain_phase = "speed-200-movement",
        .terrain_center_valid = true,
        .terrain_center_x = 128,
        .terrain_center_y = 256,
        .terrain_near = std::nullopt,
        .renderer_stone_face_growth = std::nullopt,
        .diagnostic_frame_slot = std::nullopt,
        .diagnostic_gpu_timestamp_reason = {},
        .diagnostic_gpu_terrain_duration_ns = std::nullopt,
        .latest_gpu_frame_duration_ns = std::nullopt,
    });
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark = std::move(benchmark);

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"exact_current_coverage\": null"), std::string::npos);
    EXPECT_NE(json.find("\"resident_desired\": null"), std::string::npos);
    EXPECT_NE(json.find("\"radial_bands\": null"), std::string::npos);
    EXPECT_NE(json.find("\"current_visible_mesh_detail_counts\": null"), std::string::npos);
    EXPECT_NE(json.find("\"exact_current_coverage\": false"), std::string::npos);
    EXPECT_NE(json.find("\"radial_band_tile_width\": 16"), std::string::npos);
    EXPECT_NE(json.find("\"desired\": 12345, \"resident\": 12344, \"nonresident\": 1"),
        std::string::npos);
    EXPECT_NE(json.find("\"stale_or_missing_mesh\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"current_near_mesh_work_stages\": { \"client_pending_admission_count\": 5"),
        std::string::npos);
    EXPECT_NE(json.find("\"oldest_completed_result_waiting_age_ns\": 404"), std::string::npos);
    EXPECT_NE(json.find(
        "\"current_visible_mesh_detail_counts\": "
        "{ \"coarse\": 1, \"coarse2\": 2, \"coarse4\": 3, \"fine\": 4, \"distant\": 5 }"
    ), std::string::npos);
    EXPECT_NE(json.find("\"unclassified_current_visible_mesh_detail\": 6"), std::string::npos);
    EXPECT_NE(json.find("\"phase\": \"speed-200-movement\", \"loop_ordinal\": 0, \"presentation_succeeded\""),
        std::string::npos);
    EXPECT_NE(json.find("\"terrain_center\": { \"x\": 128, \"y\": 256 }"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_cpu_presentation_recreation_duration_ns\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_presentation_recreation_attempted\": false"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_presentation_recreation_deferred\": false"), std::string::npos);
}

TEST(EvidenceJson, SerializesDefaultStoneFaceGrowthAndAbsentFrameDiagnostics)
{
    std::string const expected =
        "{ \"pre_growth_deadline_rejection_count\": 0"
        ", \"growth_attempt_count\": 0"
        ", \"before_recreate_count\": 0"
        ", \"deferred_recreation_count\": 0"
        ", \"recreation_exception_count\": 0"
        ", \"recreation_unknown_exception_count\": 0"
        ", \"recreation_exception_before_hook_count\": 0"
        ", \"growth_success_count\": 0"
        ", \"rollback_count\": 0"
        ", \"rollback_exception_count\": 0"
        ", \"post_growth_deadline_rejection_count\": 0"
        ", \"publication_retry_count\": 0"
        ", \"last_required_capacity\": 0"
        ", \"last_target_capacity\": 0"
        ", \"required_capacity_high_water\": 0"
        ", \"actual_capacity\": 0"
        ", \"actual_capacity_high_water\": 0"
        ", \"growth_work_duration_ns\": 0"
        ", \"first_recreation_exception_reason\": null"
        ", \"last_recreation_exception_reason\": null }";
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->full_radius_terrain.emplace();
    evidence.game_benchmark->raw_frames.emplace_back();

    EXPECT_EQ(acceptance::stoneFaceGrowthDiagnosticsJson({}), expected);
    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"renderer_stone_face_growth\": " + expected), std::string::npos);
    EXPECT_NE(json.find("\"renderer_stone_face_growth\": null"), std::string::npos);
}

TEST(EvidenceJson, AttributesStoneFaceGrowthIdenticallyInSnapshotAndFrame)
{
    client::StoneFaceGrowthDiagnostics const diagnostics{
        .pre_growth_deadline_rejection_count = 4'294'967'297U,
        .growth_attempt_count = 2U,
        .before_recreate_count = 3U,
        .deferred_recreation_count = 4U,
        .recreation_exception_count = 5U,
        .recreation_exception_before_hook_count = 6U,
        .growth_success_count = 7U,
        .rollback_count = 8U,
        .rollback_exception_count = 9U,
        .post_growth_deadline_rejection_count = 10U,
        .publication_retry_count = 11U,
        .last_required_capacity = 12U,
        .last_target_capacity = 13U,
        .required_capacity_high_water = 14U,
        .actual_capacity = 15U,
        .actual_capacity_high_water = 16U,
        .growth_work_duration = std::chrono::nanoseconds{ 17 },
    };
    std::string const expected =
        "{ \"pre_growth_deadline_rejection_count\": 4294967297"
        ", \"growth_attempt_count\": 2"
        ", \"before_recreate_count\": 3"
        ", \"deferred_recreation_count\": 4"
        ", \"recreation_exception_count\": 5"
        ", \"recreation_unknown_exception_count\": 0"
        ", \"recreation_exception_before_hook_count\": 6"
        ", \"growth_success_count\": 7"
        ", \"rollback_count\": 8"
        ", \"rollback_exception_count\": 9"
        ", \"post_growth_deadline_rejection_count\": 10"
        ", \"publication_retry_count\": 11"
        ", \"last_required_capacity\": 12"
        ", \"last_target_capacity\": 13"
        ", \"required_capacity_high_water\": 14"
        ", \"actual_capacity\": 15"
        ", \"actual_capacity_high_water\": 16"
        ", \"growth_work_duration_ns\": 17"
        ", \"first_recreation_exception_reason\": \"\""
        ", \"last_recreation_exception_reason\": \"\" }";
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->full_radius_terrain.emplace();
    evidence.game_benchmark->full_radius_terrain->initial_barrier.renderer_stone_face_growth = diagnostics;
    evidence.game_benchmark->raw_frames.push_back({
        .terrain_phase = {},
        .terrain_near = std::nullopt,
        .renderer_stone_face_growth = diagnostics,
        .diagnostic_frame_slot = std::nullopt,
        .diagnostic_gpu_timestamp_reason = {},
        .diagnostic_gpu_terrain_duration_ns = std::nullopt,
        .latest_gpu_frame_duration_ns = std::nullopt,
    });

    EXPECT_EQ(acceptance::stoneFaceGrowthDiagnosticsJson(diagnostics), expected);
    std::string const json = acceptance::evidenceJson(evidence);
    std::string const nested = "\"renderer_stone_face_growth\": " + expected;
    auto const snapshot_offset = json.find(nested);
    ASSERT_NE(snapshot_offset, std::string::npos);
    auto const frame_offset = json.find(nested, snapshot_offset + nested.size());
    ASSERT_NE(frame_offset, std::string::npos);
    EXPECT_EQ(json.find(nested, frame_offset + nested.size()), std::string::npos);
}

TEST(EvidenceJson, EscapesNativeRecreationReasonsAndDistinguishesUnknownFromKnownEmpty)
{
    client::StoneFaceGrowthDiagnostics diagnostics;
    diagnostics.recordRecreationExceptionReason("submitted fence \"wait\"\n");
    diagnostics.recreation_exception_count = 1U;
    diagnostics.recordRecreationExceptionReason("acquire\\slot\t");
    diagnostics.recreation_exception_count = 2U;
    std::string const known_json = acceptance::stoneFaceGrowthDiagnosticsJson(diagnostics);
    EXPECT_NE(known_json.find("\"first_recreation_exception_reason\": \"submitted fence \\\"wait\\\"\\n\""),
        std::string::npos);
    EXPECT_NE(known_json.find("\"last_recreation_exception_reason\": \"acquire\\\\slot\\t\""), std::string::npos);

    diagnostics = {};
    diagnostics.recreation_exception_count = 3U;
    diagnostics.recreation_unknown_exception_count = 3U;
    std::string const unknown_json = acceptance::stoneFaceGrowthDiagnosticsJson(diagnostics);
    EXPECT_NE(unknown_json.find("\"recreation_unknown_exception_count\": 3"), std::string::npos);
    EXPECT_NE(unknown_json.find("\"first_recreation_exception_reason\": null"), std::string::npos);
    EXPECT_NE(unknown_json.find("\"last_recreation_exception_reason\": null"), std::string::npos);

    diagnostics.recordRecreationExceptionReason("");
    ++diagnostics.recreation_exception_count;
    std::string const empty_json = acceptance::stoneFaceGrowthDiagnosticsJson(diagnostics);
    EXPECT_NE(empty_json.find("\"first_recreation_exception_reason\": \"\""), std::string::npos);
    EXPECT_NE(empty_json.find("\"last_recreation_exception_reason\": \"\""), std::string::npos);
}

TEST(EvidenceJson, SeparatesStationaryLoopAttemptsFromSuccessfulRequestIntervals)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    auto& full_radius = evidence.game_benchmark->full_radius_terrain.emplace();
    full_radius.baseline_loop_attempts = 4U;
    full_radius.baseline_failed_present_requests = 1U;
    full_radius.baseline_present_requests = 3U;
    full_radius.baseline_loop_timings.sample_count = 4U;
    full_radius.baseline_successful_present_request_intervals.sample_count = 2U;
    full_radius.recovered_loop_attempts = 6U;
    full_radius.recovered_failed_present_requests = 2U;
    full_radius.recovered_present_requests = 4U;
    full_radius.recovered_loop_timings.sample_count = 6U;
    full_radius.recovered_successful_present_request_intervals.sample_count = 3U;

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"loop_timing_scope\": \"all-stationary-loop-attempts\""), std::string::npos);
    EXPECT_NE(json.find("consecutive-successful-request-completions-not-physical-display-cadence"),
        std::string::npos);
    EXPECT_NE(json.find("\"baseline_loop_attempts\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"baseline_failed_present_requests\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"baseline_loop_timings_ns\": {\n      \"sample_count\": 4"), std::string::npos);
    EXPECT_NE(json.find("\"baseline_successful_present_request_intervals_ns\": {\n      \"sample_count\": 2"),
        std::string::npos);
    EXPECT_NE(json.find("\"recovered_loop_attempts\": 6"), std::string::npos);
    EXPECT_NE(json.find("\"recovered_failed_present_requests\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"recovered_loop_timings_ns\": {\n      \"sample_count\": 6"), std::string::npos);
    EXPECT_NE(json.find("\"recovered_successful_present_request_intervals_ns\": {\n      \"sample_count\": 3"),
        std::string::npos);
}

TEST(EvidenceJson, PreservesPresentationRecreationDiagnosticsPerFrame)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    auto& frame = evidence.game_benchmark->raw_frames.emplace_back();
    frame.diagnostic_cpu_presentation_recreation_duration_ns = 8'000'001U;
    frame.diagnostic_presentation_recreation_attempted = true;
    frame.diagnostic_presentation_recreation_deferred = true;

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"diagnostic_cpu_presentation_recreation_duration_ns\": 8000001"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_presentation_recreation_attempted\": true"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_presentation_recreation_deferred\": true"), std::string::npos);
}

TEST(EvidenceJson, PreservesPlatformFrameBudgetEvidence)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    evidence.game_benchmark->frame_budget = acceptance::GameBenchmarkFrameBudgetEvidence{
        .target_frame_duration_ns = 2'000'000U,
        .required_in_budget_time_permille = 998U,
        .measured_time_ns = 1'001'000'000U,
        .in_budget_time_ns = 999'000'000U,
        .sample_count = 1'000U,
        .passed = true,
        .required_for_benchmark_pass = true,
    };

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"frame_budget\": { \"target_frame_duration_ns\": 2000000"),
        std::string::npos);
    EXPECT_NE(json.find("\"required_in_budget_time_permille\": 998"), std::string::npos);
    EXPECT_NE(json.find("\"measured_time_ns\": 1001000000"), std::string::npos);
    EXPECT_NE(json.find("\"in_budget_time_ns\": 999000000"), std::string::npos);
    EXPECT_NE(json.find("\"sample_count\": 1000, \"passed\": true, \"required_for_benchmark_pass\": true }"),
        std::string::npos);
}

TEST(EvidenceJson, PreservesPlayerRenderStageTimingsPerFrame)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    auto& frame = evidence.game_benchmark->raw_frames.emplace_back();
    frame.diagnostic_client_window_input_duration_ns = 500'001U;
    frame.diagnostic_client_height_tile_interest_duration_ns = 600'002U;
    frame.diagnostic_client_preview_lod_refresh_duration_ns = 610'002U;
    frame.diagnostic_client_preview_lod_classified_bucket_count = 11U;
    frame.diagnostic_client_preview_lod_evaluated_tile_count = 12U;
    frame.diagnostic_client_preview_lod_changed_tile_count = 13U;
    frame.diagnostic_client_preview_mesh_pump_before_changes_duration_ns = 700'003U;
    frame.diagnostic_client_height_tile_change_and_removal_duration_ns = 800'004U;
    frame.diagnostic_client_height_tile_change_queue_duration_ns = 850'004U;
    frame.diagnostic_client_preview_mesh_remove_duration_ns = 875'004U;
    frame.diagnostic_client_preview_mesh_remove_renderer_duration_ns = 880'004U;
    frame.diagnostic_client_preview_mesh_remove_neighbor_enqueue_duration_ns = 890'004U;
    frame.diagnostic_client_preview_mesh_remove_count = 7U;
    frame.diagnostic_renderer_height_tile_remove_prefilter_duration_ns = 891'004U;
    frame.diagnostic_renderer_height_tile_remove_slot_retirement_duration_ns = 892'004U;
    frame.diagnostic_renderer_height_tile_remove_draw_index_duration_ns = 893'004U;
    frame.diagnostic_renderer_height_tile_remove_input_count = 128U;
    frame.diagnostic_renderer_height_tile_remove_matching_slot_count = 120U;
    frame.diagnostic_renderer_height_tile_remove_actual_slot_count = 119U;
    frame.diagnostic_client_preview_mesh_pump_after_changes_duration_ns = 900'005U;
    frame.diagnostic_client_camera_resolution_duration_ns = 1'000'006U;
    frame.diagnostic_client_render_before_resize_duration_ns = 1'000'001U;
    frame.diagnostic_renderer_resize_duration_ns = 2'000'002U;
    frame.diagnostic_client_render_after_resize_duration_ns = 3'000'003U;
    frame.diagnostic_renderer_call_duration_ns = 4'000'004U;
    frame.diagnostic_client_render_after_renderer_duration_ns = 5'000'005U;

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"diagnostic_client_window_input_duration_ns\": 500001"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_height_tile_interest_duration_ns\": 600002"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_lod_refresh_duration_ns\": 610002"),
              std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_lod_classified_bucket_count\": 11"),
              std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_lod_evaluated_tile_count\": 12"),
              std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_lod_changed_tile_count\": 13"),
              std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_mesh_pump_before_changes_duration_ns\": 700003"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_height_tile_change_and_removal_duration_ns\": 800004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_height_tile_change_queue_duration_ns\": 850004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_mesh_remove_duration_ns\": 875004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_mesh_remove_renderer_duration_ns\": 880004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_mesh_remove_neighbor_enqueue_duration_ns\": 890004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_mesh_remove_count\": 7"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_height_tile_remove_prefilter_duration_ns\": 891004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_height_tile_remove_slot_retirement_duration_ns\": 892004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_height_tile_remove_draw_index_duration_ns\": 893004"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_height_tile_remove_input_count\": 128"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_height_tile_remove_matching_slot_count\": 120"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_height_tile_remove_actual_slot_count\": 119"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_preview_mesh_pump_after_changes_duration_ns\": 900005"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_camera_resolution_duration_ns\": 1000006"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_render_before_resize_duration_ns\": 1000001"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_resize_duration_ns\": 2000002"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_render_after_resize_duration_ns\": 3000003"),
        std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_renderer_call_duration_ns\": 4000004"), std::string::npos);
    EXPECT_NE(json.find("\"diagnostic_client_render_after_renderer_duration_ns\": 5000005"),
        std::string::npos);
}

TEST(EvidenceJson, PreservesTimedTerrainRadialBands)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    auto& frame = evidence.game_benchmark->raw_frames.emplace_back();
    frame.diagnostic_terrain_radial_bands.emplace();
    (*frame.diagnostic_terrain_radial_bands)[0U] = {
        .desired_count = 5U,
        .resident_count = 3U,
        .nonresident_count = 2U,
        .current_mesh_visible_count = 1U,
        .stale_or_missing_mesh_count = 2U,
    };
    frame.diagnostic_terrain_spatial_scan_duration_ns = 12'345U;

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"diagnostic_terrain_spatial_scan_duration_ns\": 12345"), std::string::npos);
    EXPECT_NE(json.find("\"desired\": 5, \"resident\": 3, \"nonresident\": 2, "
        "\"current_mesh_visible\": 1, \"stale_or_missing_mesh\": 2"), std::string::npos);
}

TEST(EvidenceJson, PreservesMeshEpisodeLossAndLatencyInsteadOfOnlyCompletedSamples)
{
    acceptance::RuntimeEvidence evidence{};
    evidence.game_benchmark.emplace();
    auto& frame = evidence.game_benchmark->raw_frames.emplace_back();
    frame.diagnostic_mesh_episodes.emplace();
    auto& episodes = *frame.diagnostic_mesh_episodes;
    episodes.entered_count = 13U;
    episodes.completed_count = 7U;
    episodes.censored_count = 3U;
    episodes.pending_count = 3U;
    episodes.tracked_count = 10U;
    episodes.completed_max_ns = 120'000'000U;
    episodes.censored_max_ns = 4'000'000'000U;
    episodes.oldest_pending_ns = 2'000'000'000U;
    episodes.oldest_resident_pending_ns = 1'000'000'000U;
    episodes.censored_latency_buckets[5U] = 3U;
    episodes.never_resident_censored_count = 1U;
    episodes.resident_censored_count = 2U;
    episodes.resident_censored_max_wait_ns = 900'000'000U;
    episodes.never_resident_censored_entry_latency_buckets[5U] = 1U;
    episodes.resident_censored_wait_latency_buckets[0U] = 2U;
    episodes.resident_at_departure_censored_entry_latency_buckets[4U] = 1U;
    episodes.nonresident_at_departure_censored_entry_latency_buckets[5U] = 1U;
    episodes.unknown_departure_residency_censored_count = 1U;
    episodes.global_active_mesh_job_count = 87U;
    episodes.global_worker_result_count = 65U;
    episodes.global_worker_outstanding_count = 128U;
    episodes.pending_by_current_band[7U] = 3U;
    std::string const json = acceptance::evidenceJson(evidence);
    EXPECT_NE(json.find("\"diagnostic_mesh_episodes\": {"), std::string::npos);
    EXPECT_NE(json.find("\"censored_count\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"censored_max_ns\": 4000000000"), std::string::npos);
    EXPECT_NE(json.find("\"oldest_pending_ns\": 2000000000"), std::string::npos);
    EXPECT_NE(json.find("\"oldest_resident_pending_ns\": 1000000000"), std::string::npos);
    EXPECT_NE(json.find("\"censored_latency_buckets\": [0, 0, 0, 0, 0, 3, 0]"), std::string::npos);
    EXPECT_NE(json.find("\"never_resident_censored_count\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"resident_censored_count\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"resident_censored_max_wait_ns\": 900000000"), std::string::npos);
    EXPECT_NE(json.find("\"never_resident_censored_entry_latency_buckets\": [0, 0, 0, 0, 0, 1, 0]"),
        std::string::npos);
    EXPECT_NE(json.find("\"resident_censored_wait_latency_buckets\": [2, 0, 0, 0, 0, 0, 0]"),
        std::string::npos);
    EXPECT_NE(json.find("\"resident_at_departure_censored_entry_latency_buckets\": [0, 0, 0, 0, 1, 0, 0]"),
        std::string::npos);
    EXPECT_NE(json.find("\"nonresident_at_departure_censored_entry_latency_buckets\": [0, 0, 0, 0, 0, 1, 0]"),
        std::string::npos);
    EXPECT_NE(json.find("\"unknown_departure_residency_censored_count\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"global_active_mesh_job_count\": 87"), std::string::npos);
    EXPECT_NE(json.find("\"global_worker_result_count\": 65"), std::string::npos);
    EXPECT_NE(json.find("\"global_worker_outstanding_count\": 128"), std::string::npos);
    EXPECT_NE(json.find("\"pending_by_current_band\": [0, 0, 0, 0, 0, 0, 0, 3"), std::string::npos);
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
