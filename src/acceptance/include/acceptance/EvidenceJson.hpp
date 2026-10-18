#pragma once

#include <client/render/StoneFaceCapacity.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acceptance {

struct FrameTimingSummary final {
    uint64_t sample_count{ 0 };
    std::chrono::nanoseconds p50{ 0 };
    std::chrono::nanoseconds p95{ 0 };
    std::chrono::nanoseconds p99{ 0 };
    std::chrono::nanoseconds p99_8{ 0 };
    std::chrono::nanoseconds maximum{ 0 };
    std::chrono::nanoseconds mean{ 0 };
};

struct GameBenchmarkMeshStageSummary final {
    uint32_t client_pending_admission_count = 0U;
    uint32_t worker_queued_count = 0U;
    uint32_t worker_building_count = 0U;
    uint32_t completed_result_waiting_count = 0U;
    uint64_t oldest_client_pending_near_observed_age_ns = 0U;
    uint64_t oldest_worker_queued_age_ns = 0U;
    uint64_t oldest_worker_building_age_ns = 0U;
    uint64_t oldest_completed_result_waiting_age_ns = 0U;
};

struct GameBenchmarkMeshResultDrainSummary final {
    uint32_t drained_result_count = 0U;
    uint32_t stale_epoch_discard_count = 0U;
    uint32_t no_longer_interested_or_resident_discard_count = 0U;
    uint32_t source_mismatch_requeue_count = 0U;
    uint32_t target_detail_mismatch_requeue_count = 0U;
    uint32_t cannot_publish_requeue_count = 0U;
    uint32_t publish_attempt_count = 0U;
    uint32_t publish_success_count = 0U;
    uint32_t publish_failure_count = 0U;
    uint64_t seam_planning_duration_ns = 0U;
    uint64_t renderer_batch_upsert_duration_ns = 0U;
    uint64_t renderer_free_range_prepare_duration_ns = 0U;
    uint64_t renderer_mesh_prepare_duration_ns = 0U;
    uint64_t renderer_capacity_prepare_duration_ns = 0U;
    uint64_t renderer_commit_upload_duration_ns = 0U;
};

struct RendererBenchmarkEvidence final {
    std::string package_id;
    std::string platform;
    std::string hardware;
    std::string gpu;
    std::string vulkan_api_version;
    std::string present_mode;
    std::string pipeline_path;
    uint32_t requested_width{ 0 };
    uint32_t requested_height{ 0 };
    uint32_t actual_width{ 0 };
    uint32_t actual_height{ 0 };
    bool validation_enabled{ false };
    bool debug_hud_enabled{ false };
    std::chrono::milliseconds warmup{ 0 };
    std::chrono::nanoseconds sample_elapsed{ 0 };
    double presentation_requests_per_second{ 0.0 };
    FrameTimingSummary presentation_request_timings;
    FrameTimingSummary acquire_wait_timings;
    FrameTimingSummary command_record_timings;
    FrameTimingSummary complete_present_wait_timings;
};

struct RendererCaptureEvidence final {
    std::string image_path;
    std::string gpu;
    std::string vulkan_api_version;
    std::string present_mode;
    std::string pipeline_path;
    uint32_t requested_width{ 0 };
    uint32_t requested_height{ 0 };
    uint32_t actual_width{ 0 };
    uint32_t actual_height{ 0 };
    bool validation_enabled{ false };
    bool content_verified{ false };
};

enum class GameBenchmarkPresentationAcquireStage : uint8_t {
    SubmittedFrameFence,
    AcquireFence,
    PresentFence,
    SwapchainImage,
};

struct GameBenchmarkPresentationAcquireStageSample final {
    GameBenchmarkPresentationAcquireStage stage = GameBenchmarkPresentationAcquireStage::SubmittedFrameFence;
    uint64_t duration_ns = 0U;
    int32_t result = 0;
};

struct GameBenchmarkPresentationAcquireSample final {
    bool captured = false;
    uint32_t frame_slot = 0U;
    uint32_t stage_count = 0U;
    std::array<GameBenchmarkPresentationAcquireStageSample, 4U> stages{};
    std::optional<uint32_t> image_index;
};

struct GameBenchmarkFrameSample final {
    bool warm = false;
    bool uncapped = false;
    bool excluded = false;
    bool presentation_succeeded = false;
    uint64_t started_offset_ns = 0U;
    uint64_t loop_duration_ns = 0U;
    uint64_t render_duration_ns = 0U;
    uint64_t network_poll_duration_ns = 0U;
    uint64_t height_tile_delivery_duration_ns = 0U;
    uint64_t network_event_count = 0U;
    uint64_t client_message_payload_bytes_sent = 0U;
    uint64_t client_message_payload_bytes_received = 0U;
    uint32_t resident_terrain_tile_count = 0U;
    uint32_t visible_surface_face_count = 0U;
    std::string terrain_phase;
    bool terrain_center_valid = false;
    int32_t terrain_center_x = 0;
    int32_t terrain_center_y = 0;
    struct TerrainNearSample final {
        bool center_valid = false;
        int32_t center_x = 0;
        int32_t center_y = 0;
        uint32_t desired_tile_count = 0U;
        uint32_t resident_tile_count = 0U;
        uint32_t nonresident_tile_count = 0U;
        uint32_t current_mesh_visible_count = 0U;
        uint32_t pending_mesh_count = 0U;
        uint32_t active_mesh_job_count = 0U;
        uint32_t unclassified_missing_count = 0U;
        uint64_t accepted_near_admission_delta = 0U;
        uint64_t accepted_far_admission_delta = 0U;
        uint64_t far_admission_while_near_pending_delta = 0U;
        GameBenchmarkMeshStageSummary mesh_work_stages;
        GameBenchmarkMeshResultDrainSummary mesh_result_drain;
    };
    std::optional<TerrainNearSample> terrain_near;
    struct TerrainRadialBand final {
        uint32_t desired_count = 0U;
        uint32_t resident_count = 0U;
        uint32_t nonresident_count = 0U;
        uint32_t current_mesh_visible_count = 0U;
        uint32_t stale_or_missing_mesh_count = 0U;
    };
    std::optional<std::array<TerrainRadialBand, 16U>> diagnostic_terrain_radial_bands;
    uint64_t diagnostic_terrain_spatial_scan_duration_ns = 0U;
    std::optional<client::StoneFaceGrowthDiagnostics> renderer_stone_face_growth = std::nullopt;
    uint64_t diagnostic_render_attempt_id = 0U;
    GameBenchmarkPresentationAcquireSample diagnostic_presentation_acquire = {};
    bool diagnostic_frame_recorded = false;
    bool diagnostic_indexed_stone_quads = false;
    bool diagnostic_stone_indirect = false;
    std::optional<uint32_t> diagnostic_frame_slot;
    uint64_t diagnostic_submitted_stone_quad_count = 0U;
    uint32_t diagnostic_stone_draw_count = 0U;
    std::array<double, 7> diagnostic_camera{};
    uint64_t diagnostic_cpu_acquire_wait_duration_ns = 0U;
    uint64_t diagnostic_cpu_command_record_duration_ns = 0U;
    uint64_t diagnostic_cpu_visible_range_collection_duration_ns = 0U;
    uint64_t diagnostic_cpu_indirect_prepare_duration_ns = 0U;
    uint64_t diagnostic_cpu_stone_scene_record_duration_ns = 0U;
    uint64_t diagnostic_cpu_command_record_residual_duration_ns = 0U;
    uint64_t diagnostic_cpu_complete_present_wait_duration_ns = 0U;
    uint64_t diagnostic_client_height_tile_interest_duration_ns = 0U;
    uint64_t diagnostic_client_preview_lod_refresh_duration_ns = 0U;
    uint32_t diagnostic_client_preview_lod_classified_bucket_count = 0U;
    uint32_t diagnostic_client_preview_lod_evaluated_tile_count = 0U;
    uint32_t diagnostic_client_preview_lod_changed_tile_count = 0U;
    bool diagnostic_gpu_timestamps_enabled = false;
    std::string_view diagnostic_gpu_timestamp_reason;
    uint64_t diagnostic_gpu_sample_attempt_id = 0U;
    uint32_t diagnostic_gpu_sample_slot = 0U;
    uint64_t diagnostic_gpu_sample_quad_count = 0U;
    uint32_t diagnostic_gpu_sample_draw_count = 0U;
    std::optional<uint64_t> diagnostic_gpu_terrain_duration_ns;
    std::optional<uint64_t> latest_gpu_frame_duration_ns;
};

struct GameBenchmarkTerrainReadinessEvidence final {
    uint32_t near_radius_tiles = 7U;
    uint64_t readiness_deadline_ms = 4'000U;
    uint64_t readiness_drain_ms = 4'000U;
    uint64_t center_sample_count = 0U;
    uint64_t center_change_count = 0U;
    uint64_t current_near_sample_count = 0U;
    uint64_t incomplete_current_near_sample_count = 0U;
    uint32_t max_missing_current_mesh_count = 0U;
    bool current_near_coverage_complete = false;
    uint64_t eligible_mesh_episode_count = 0U;
    uint64_t ready_mesh_episode_count = 0U;
    uint64_t left_near_unready_episode_count = 0U;
    uint64_t source_lost_before_ready_episode_count = 0U;
    uint64_t deadline_miss_count = 0U;
    uint64_t open_mesh_episode_count = 0U;
    uint64_t max_ready_latency_ns = 0U;
    bool has_max_ready_latency = false;
    int32_t max_ready_key_x = 0;
    int32_t max_ready_key_y = 0;
    int32_t max_ready_start_center_x = 0;
    int32_t max_ready_start_center_y = 0;
    uint64_t accepted_near_admissions = 0U;
    uint64_t accepted_far_admissions = 0U;
    uint64_t far_admissions_while_near_pending = 0U;
    uint64_t unclassified_missing_observation_count = 0U;
    uint32_t final_desired_tile_count = 0U;
    uint32_t final_resident_tile_count = 0U;
    uint32_t final_nonresident_tile_count = 0U;
    uint32_t final_current_mesh_visible_count = 0U;
    uint32_t final_pending_mesh_count = 0U;
    uint32_t final_active_mesh_job_count = 0U;
    uint32_t final_unclassified_missing_count = 0U;
    bool final_center_valid = false;
    bool complete = false;
};

struct GameBenchmarkFullTerrainSnapshot final {
    struct RadialBand final {
        uint32_t desired_count = 0U;
        uint32_t resident_count = 0U;
        uint32_t nonresident_count = 0U;
        uint32_t current_mesh_visible_count = 0U;
        uint32_t stale_or_missing_mesh_count = 0U;
    };
    std::string phase;
    uint64_t phase_elapsed_ms = 0U;
    bool scan_performed = false;
    bool potentially_ready = false;
    std::optional<bool> exact_current_coverage;
    bool center_valid = false;
    int32_t center_x = 0;
    int32_t center_y = 0;
    uint32_t desired_count = 0U;
    uint32_t resident_total_count = 0U;
    uint32_t visible_mesh_count = 0U;
    uint32_t renderer_mesh_count = 0U;
    uint32_t renderer_face_count = 0U;
    uint32_t renderer_draw_face_count = 0U;
    uint32_t renderer_stone_face_capacity = 0U;
    client::StoneFaceGrowthDiagnostics renderer_stone_face_growth;
    uint32_t pending_delivery_batch_count = 0U;
    uint32_t pending_residency_change_count = 0U;
    uint32_t pending_mesh_count = 0U;
    uint32_t active_mesh_job_count = 0U;
    uint32_t dirty_mesh_job_count = 0U;
    uint32_t worker_result_count = 0U;
    uint32_t worker_outstanding_count = 0U;
    uint32_t pending_removal_count = 0U;
    std::optional<uint32_t> resident_desired_count;
    std::optional<uint32_t> nonresident_desired_count;
    std::optional<uint32_t> current_handle_mesh_visible_count;
    std::optional<uint32_t> stale_or_missing_mesh_count;
    std::optional<uint32_t> unclassified_resident_missing_count;
    std::optional<uint32_t> visible_outside_interest_count;
    std::optional<std::array<uint32_t, 5U>> current_visible_mesh_detail_counts;
    std::optional<uint32_t> unclassified_current_visible_mesh_detail_count;
    std::array<RadialBand, 16U> radial_bands{};
    GameBenchmarkMeshStageSummary current_near_mesh_work_stages;
};

struct GameBenchmarkFullRadiusEvidence final {
    uint32_t render_distance = 256U;
    uint32_t expected_tile_count = 205'861U;
    uint64_t warmup_timeout_ms = 180'000U;
    uint64_t initial_warmup_elapsed_ms = 0U;
    uint64_t baseline_stationary_elapsed_ms = 0U;
    uint64_t movement_elapsed_ms = 0U;
    uint64_t recovery_warmup_elapsed_ms = 0U;
    uint64_t recovered_stationary_elapsed_ms = 0U;
    bool initial_barrier_reached = false;
    bool recovery_barrier_reached = false;
    GameBenchmarkFullTerrainSnapshot initial_barrier;
    GameBenchmarkFullTerrainSnapshot recovery_barrier;
    uint64_t baseline_loop_attempts = 0U;
    uint64_t recovered_loop_attempts = 0U;
    uint64_t baseline_failed_present_requests = 0U;
    uint64_t recovered_failed_present_requests = 0U;
    uint64_t baseline_present_requests = 0U;
    uint64_t recovered_present_requests = 0U;
    double baseline_present_requests_per_second = 0.0;
    double recovered_present_requests_per_second = 0.0;
    FrameTimingSummary baseline_loop_timings;
    FrameTimingSummary recovered_loop_timings;
    FrameTimingSummary baseline_successful_present_request_intervals;
    FrameTimingSummary recovered_successful_present_request_intervals;
    uint64_t movement_center_change_count = 0U;
    bool movement_start_center_valid = false;
    int32_t movement_start_center_x = 0;
    int32_t movement_start_center_y = 0;
    bool movement_end_center_valid = false;
    int32_t movement_end_center_x = 0;
    int32_t movement_end_center_y = 0;
    bool complete = false;
};

struct GameBenchmarkServerStreamSample final {
    uint64_t started_offset_ns = 0U;
    int32_t center_x = 0;
    int32_t center_y = 0;
    uint32_t near_desired_count = 0U;
    uint32_t near_resident_count = 0U;
    uint32_t near_queued_count = 0U;
    uint32_t near_dispatched_count = 0U;
    uint32_t near_ready_count = 0U;
    uint32_t near_inflight_count = 0U;
    uint32_t near_inflight_removals_count = 0U;
    uint32_t near_untracked_count = 0U;
    uint32_t server_queued_count = 0U;
    uint32_t server_dispatched_count = 0U;
    uint32_t server_ready_count = 0U;
    uint32_t server_inflight_addition_count = 0U;
    uint32_t server_inflight_removal_count = 0U;
    uint32_t server_inflight_delivery_count = 0U;
    uint32_t server_pending_removal_count = 0U;
    uint32_t server_resident_count = 0U;
    uint32_t server_admitted_addition_count = 0U;
    uint64_t server_rejected_delivery_ack_count = 0U;
    uint32_t server_delivery_credit_count = 0U;
    uint32_t worker_queued_count = 0U;
    uint32_t worker_running_count = 0U;
    uint32_t worker_completed_uncollected_count = 0U;
};

struct GameBenchmarkEvidence final {
    std::string workload;
    std::string package_id;
    std::string hardware;
    std::optional<std::string> gpu;
    std::string present_mode;
    std::string build_mode;
    bool diagnostic_overrides_present = false;
    uint64_t cold_duration_ms = 0U;
    uint64_t warm_duration_ms = 0U;
    uint64_t uncapped_duration_ms = 0U;
    uint32_t framebuffer_width = 0U;
    uint32_t framebuffer_height = 0U;
    uint64_t server_events_processed = 0U;
    uint64_t client_message_payload_bytes_sent = 0U;
    uint64_t client_message_payload_bytes_received = 0U;
    uint64_t inputs_sent = 0U;
    uint64_t authoritative_player_updates = 0U;
    uint64_t observed_wrap_crossings = 0U;
    uint64_t observed_capability_transitions = 0U;
    uint64_t permission_publishes = 0U;
    uint64_t successful_present_requests = 0U;
    uint64_t failed_present_requests = 0U;
    uint64_t excluded_present_requests = 0U;
    double warm_present_requests_per_second = 0.0;
    double uncapped_present_requests_per_second = 0.0;
    FrameTimingSummary cold_loop_timings;
    FrameTimingSummary warm_loop_timings;
    FrameTimingSummary uncapped_loop_timings;
    FrameTimingSummary server_tick_timings;
    FrameTimingSummary permission_publish_timings;
    std::optional<GameBenchmarkTerrainReadinessEvidence> terrain_readiness;
    std::optional<GameBenchmarkFullRadiusEvidence> full_radius_terrain;
    std::vector<GameBenchmarkFrameSample> raw_frames;
    std::vector<GameBenchmarkServerStreamSample> raw_server_stream_samples;
    std::vector<uint64_t> raw_server_tick_durations_ns;
    std::vector<uint64_t> raw_permission_publish_durations_ns;
};

struct RuntimeEvidence final {
    std::string mode;
    std::string scenario_version;
    std::string profile;
    std::string scenario_name;
    std::string failure;
    uint64_t seed{ 0 };
    uint64_t ticks{ 0 };
    uint64_t clients_requested{ 0 };
    uint64_t clients_accepted{ 0 };
    uint64_t server_events_processed{ 0 };
    uint64_t accepted_tick{ 0 };
    uint64_t last_effective_tick{ 0 };
    uint64_t inputs_sent{ 0 };
    uint64_t camera_relative_inputs{ 0 };
    uint64_t expectations_passed{ 0 };
    uint64_t authoritative_tick_ms{ 0 };
    std::string replay_id;
    std::chrono::milliseconds elapsed{ 0 };
    std::chrono::milliseconds deadline{ 0 };
    bool passed{ false };
    std::optional<RendererBenchmarkEvidence> benchmark = std::nullopt;
    std::optional<GameBenchmarkEvidence> game_benchmark = std::nullopt;
    std::optional<RendererCaptureEvidence> capture = std::nullopt;
};

[[nodiscard]]
std::string stoneFaceGrowthDiagnosticsJson(
    client::StoneFaceGrowthDiagnostics const& diagnostics
);

[[nodiscard]]
std::string evidenceJson(RuntimeEvidence const& evidence);

[[nodiscard]]
std::expected<void, std::string> writeEvidenceJson(
    std::filesystem::path const& path,
    RuntimeEvidence const& evidence
);

[[nodiscard]]
RuntimeEvidence collectRuntimeEvidence(
    std::string mode,
    std::function<std::expected<RuntimeEvidence, std::string>()> const& operation
);

} // namespace acceptance
