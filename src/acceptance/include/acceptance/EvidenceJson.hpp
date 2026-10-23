#pragma once

#include <client/render/StoneFaceCapacity.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <acceptance/ReplayInput.hpp>

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

struct GameBenchmarkFrameBudgetEvidence final {
    uint64_t target_frame_duration_ns = 0U;
    uint32_t required_in_budget_time_permille = 990U;
    uint64_t measurement_window_start_offset_ns = 0U;
    uint64_t measurement_window_end_offset_ns = 0U;
    uint64_t required_window_duration_ns = 0U;
    uint64_t measured_time_ns = 0U;
    uint64_t in_budget_time_ns = 0U;
    uint64_t sample_count = 0U;
    uint64_t fence_confirmed_frame_count = 0U;
    bool measurement_window_complete = false;
    bool passed = false;
    bool required_for_benchmark_pass = false;
};

struct GameBenchmarkFrameCompletion final {
    uint64_t sequence_id = 0U;
    uint64_t swapchain_generation = 0U;
    uint32_t frame_slot = 0U;
    uint64_t submitted_offset_ns = 0U;
    uint64_t completion_observed_offset_ns = 0U;
    std::optional<uint32_t> application_present_id = std::nullopt;
    std::string submission_phase;
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

struct GameBenchmarkPresentationRequestId final {
    uint64_t swapchain_generation = 0U;
    uint32_t application_present_id = 0U;
};

struct GameBenchmarkSuccessfulPresentationRequest final {
    GameBenchmarkPresentationRequestId request;
    uint64_t loop_ordinal = 0U;
    std::string phase;
};

struct GameBenchmarkPresentationTimingRecord final {
    GameBenchmarkPresentationRequestId request;
    uint64_t actual_present_time_ns = 0U;
};

enum class GameBenchmarkPresentationPollScope : uint8_t {
    Loop,
    Drain,
};

enum class GameBenchmarkPresentationPollStatus : uint8_t {
    Unavailable,
    Empty,
    Records,
    Incomplete,
    Error,
};

struct GameBenchmarkPresentationPoll final {
    GameBenchmarkPresentationPollScope scope = GameBenchmarkPresentationPollScope::Loop;
    GameBenchmarkPresentationPollStatus status = GameBenchmarkPresentationPollStatus::Unavailable;
    int32_t vk_result = 0;
    uint32_t returned_count = 0U;
};

struct GameBenchmarkPresentationCaptureEndpoint final {
    GameBenchmarkPresentationRequestId request;
    uint64_t loop_ordinal = 0U;
};

struct GameBenchmarkPresentationTimingEvidence final {
    bool capability_supported = false;
    std::vector<GameBenchmarkSuccessfulPresentationRequest> successful_requests;
    std::vector<GameBenchmarkPresentationTimingRecord> records;
    std::vector<GameBenchmarkPresentationPoll> polls;
    std::optional<GameBenchmarkPresentationCaptureEndpoint> capture_endpoint;
    bool drain_complete = false;
    std::vector<GameBenchmarkPresentationRequestId> drain_request_ids;
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

struct GameBenchmarkFrameSample final {
    uint64_t loop_ordinal = 0U;
    bool warm = false;
    bool uncapped = false;
    bool excluded = false;
    bool presentation_succeeded = false;
    std::optional<uint64_t> swapchain_generation = std::nullopt;
    std::optional<uint32_t> application_present_id = std::nullopt;
    std::optional<uint64_t> submitted_frame_sequence_id = std::nullopt;
    std::optional<uint32_t> submitted_frame_slot = std::nullopt;
    uint64_t submitted_frame_offset_ns = 0U;
    std::string submitted_frame_phase;
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
    std::string terrain_phase = {};
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
    struct PlayableSphere final {
        uint32_t radius_blocks = 320U;
        std::optional<uint64_t> active_coverage_epoch;
        std::optional<shared::PlayerPosition> active_coverage_center;
        std::optional<shared::PlayerPosition> world_loading_center;
        uint32_t ready_chunks = 0U;
        uint32_t required_chunks = 0U;
        bool ready = false;
        std::string coverage_scope;
        uint32_t required_authoritative_columns = 0U;
        uint32_t resident_authoritative_columns = 0U;
        uint32_t vertical_chunk_count = 0U;
        std::optional<shared::PlayerPosition> authoritative_columns_center;
    };
    std::optional<PlayableSphere> playable_sphere = std::nullopt;
    struct VisualLod final {
        uint32_t radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS;
        bool scene_active = false;
        uint32_t leaf_count = 0U;
        uint32_t actual_column_count = 0U;
        std::optional<shared::TerrainIdentity> terrain_identity;
        uint64_t session_join_epoch = 0U;
        uint64_t coverage_epoch = 0U;
        uint64_t submitted_frame_count = 0U;
        uint32_t scene_center_x = 0U;
        uint32_t scene_center_y = 0U;
        uint32_t actual_radius_blocks = 0U;
        uint32_t required_actual_columns = 0U;
        uint32_t presented_current_actual_columns = 0U;
        std::optional<shared::PlayerPosition> coverage_center;
        bool sources_current = false;
    };
    std::optional<VisualLod> visual_lod = std::nullopt;
    struct QualificationSettings final {
        uint32_t framebuffer_width = 0U;
        uint32_t framebuffer_height = 0U;
        uint32_t render_distance = 0U;
        bool debug_hud_enabled = false;
        std::string terrain_model;
        std::string camera_perspective;
        std::string world_mode;
    };
    std::optional<QualificationSettings> qualification_settings = std::nullopt;
    struct PresentedPlayer final {
        std::array<double, 3U> body{};
        uint64_t submitted_frame_count = 0U;
        uint64_t sampled_offset_ns = 0U;
    };
    std::optional<PresentedPlayer> presented_player = std::nullopt;
    std::optional<TerrainNearSample> terrain_near = std::nullopt;
    struct TerrainRadialBand final {
        uint32_t desired_count = 0U;
        uint32_t resident_count = 0U;
        uint32_t nonresident_count = 0U;
        uint32_t current_mesh_visible_count = 0U;
        uint32_t stale_or_missing_mesh_count = 0U;
    };
    std::optional<std::array<TerrainRadialBand, 16U>> diagnostic_terrain_radial_bands = std::nullopt;
    struct MeshEpisodes final {
        uint64_t entered_count = 0U;
        uint64_t completed_count = 0U;
        uint64_t censored_count = 0U;
        uint64_t completed_total_ns = 0U;
        uint64_t completed_max_ns = 0U;
        uint64_t censored_max_ns = 0U;
        uint64_t tracked_count = 0U;
        uint64_t pending_count = 0U;
        uint64_t oldest_pending_ns = 0U;
        uint64_t oldest_resident_pending_ns = 0U;
        std::array<uint64_t, 7U> completed_latency_buckets{};
        std::array<uint64_t, 7U> censored_latency_buckets{};
        uint64_t never_resident_censored_count = 0U;
        uint64_t resident_censored_count = 0U;
        uint64_t resident_censored_max_wait_ns = 0U;
        std::array<uint64_t, 7U> never_resident_censored_entry_latency_buckets{};
        std::array<uint64_t, 7U> resident_censored_wait_latency_buckets{};
        std::array<uint64_t, 7U> resident_at_departure_censored_entry_latency_buckets{};
        std::array<uint64_t, 7U> nonresident_at_departure_censored_entry_latency_buckets{};
        uint64_t unknown_departure_residency_censored_count = 0U;
        uint32_t global_active_mesh_job_count = 0U;
        uint32_t global_worker_result_count = 0U;
        uint32_t global_worker_outstanding_count = 0U;
        std::array<uint64_t, 16U> pending_by_current_band{};
        std::array<uint64_t, 16U> oldest_pending_ns_by_current_band{};
    };
    std::optional<MeshEpisodes> diagnostic_mesh_episodes = std::nullopt;
    uint64_t diagnostic_terrain_spatial_scan_duration_ns = 0U;
    std::optional<client::StoneFaceGrowthDiagnostics> renderer_stone_face_growth = std::nullopt;
    uint64_t diagnostic_render_attempt_id = 0U;
    bool diagnostic_frame_recorded = false;
    bool diagnostic_indexed_stone_quads = false;
    bool diagnostic_stone_indirect = false;
    std::optional<uint32_t> diagnostic_frame_slot = std::nullopt;
    uint64_t diagnostic_submitted_stone_quad_count = 0U;
    uint32_t diagnostic_stone_draw_count = 0U;
    std::array<double, 7> diagnostic_camera{};
    uint64_t diagnostic_cpu_acquire_wait_duration_ns = 0U;
    GameBenchmarkPresentationAcquireSample diagnostic_presentation_acquire = {};
    uint64_t diagnostic_cpu_presentation_recreation_duration_ns = 0U;
    bool diagnostic_presentation_recreation_attempted = false;
    bool diagnostic_presentation_recreation_deferred = false;
    uint64_t diagnostic_cpu_command_record_duration_ns = 0U;
    uint64_t diagnostic_cpu_visible_range_collection_duration_ns = 0U;
    uint64_t diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns = 0U;
    uint64_t diagnostic_cpu_height_tile_pending_range_order_duration_ns = 0U;
    uint64_t diagnostic_cpu_height_tile_visible_range_merge_duration_ns = 0U;
    uint64_t diagnostic_cpu_indirect_prepare_duration_ns = 0U;
    uint64_t diagnostic_cpu_stone_scene_record_duration_ns = 0U;
    uint64_t diagnostic_cpu_command_record_residual_duration_ns = 0U;
    uint64_t diagnostic_cpu_complete_present_wait_duration_ns = 0U;
    uint64_t diagnostic_client_window_input_duration_ns = 0U;
    uint64_t diagnostic_client_height_tile_interest_duration_ns = 0U;
    uint64_t diagnostic_client_preview_lod_refresh_duration_ns = 0U;
    uint32_t diagnostic_client_preview_lod_classified_bucket_count = 0U;
    uint32_t diagnostic_client_preview_lod_evaluated_tile_count = 0U;
    uint32_t diagnostic_client_preview_lod_changed_tile_count = 0U;
    uint64_t diagnostic_client_preview_mesh_pump_before_changes_duration_ns = 0U;
    uint64_t diagnostic_client_height_tile_change_and_removal_duration_ns = 0U;
    uint64_t diagnostic_client_height_tile_change_queue_duration_ns = 0U;
    uint64_t diagnostic_client_preview_mesh_remove_duration_ns = 0U;
    uint64_t diagnostic_client_preview_mesh_remove_renderer_duration_ns = 0U;
    uint64_t diagnostic_client_preview_mesh_remove_neighbor_enqueue_duration_ns = 0U;
    uint32_t diagnostic_client_preview_mesh_remove_count = 0U;
    uint64_t diagnostic_renderer_height_tile_remove_prefilter_duration_ns = 0U;
    uint64_t diagnostic_renderer_height_tile_remove_slot_retirement_duration_ns = 0U;
    uint64_t diagnostic_renderer_height_tile_remove_draw_index_duration_ns = 0U;
    uint64_t diagnostic_renderer_height_tile_remove_input_count = 0U;
    uint64_t diagnostic_renderer_height_tile_remove_matching_slot_count = 0U;
    uint64_t diagnostic_renderer_height_tile_remove_actual_slot_count = 0U;
    uint64_t diagnostic_client_preview_mesh_pump_after_changes_duration_ns = 0U;
    uint64_t diagnostic_client_camera_resolution_duration_ns = 0U;
    uint64_t diagnostic_client_render_before_resize_duration_ns = 0U;
    uint64_t diagnostic_renderer_resize_duration_ns = 0U;
    uint64_t diagnostic_client_render_after_resize_duration_ns = 0U;
    uint64_t diagnostic_renderer_call_duration_ns = 0U;
    uint64_t diagnostic_client_render_after_renderer_duration_ns = 0U;
    bool diagnostic_gpu_timestamps_enabled = false;
    std::string_view diagnostic_gpu_timestamp_reason = {};
    uint64_t diagnostic_gpu_sample_attempt_id = 0U;
    uint32_t diagnostic_gpu_sample_slot = 0U;
    uint64_t diagnostic_gpu_sample_quad_count = 0U;
    uint32_t diagnostic_gpu_sample_draw_count = 0U;
    std::optional<uint64_t> diagnostic_gpu_terrain_duration_ns = std::nullopt;
    std::optional<uint64_t> latest_gpu_frame_duration_ns = std::nullopt;
    bool diagnostic_terrain_occlusion_enabled = false;
    bool diagnostic_terrain_occlusion_oracle_frame = false;
    bool diagnostic_terrain_occlusion_filtered_frame = false;
    uint64_t diagnostic_terrain_occlusion_removed_quad_count = 0U;
    uint32_t diagnostic_terrain_occlusion_removed_range_count = 0U;
    uint32_t diagnostic_terrain_occlusion_removed_command_count = 0U;
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
    std::string world_loading_state;
    uint32_t playable_sphere_radius_blocks = 320U;
    uint32_t playable_sphere_ready_chunks = 0U;
    uint32_t playable_sphere_required_chunks = 0U;
    bool playable_sphere_ready = false;
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
    std::string movement_playable_sphere_sample_scope =
        "per-movement-loop-and-every-authoritative-position-update";
    bool movement_start_center_valid = false;
    int32_t movement_start_center_x = 0;
    int32_t movement_start_center_y = 0;
    bool movement_end_center_valid = false;
    int32_t movement_end_center_x = 0;
    int32_t movement_end_center_y = 0;
    struct AuthoritativePlayableSphereUpdate final {
        uint64_t update_ordinal = 0U;
        uint64_t offset_ns = 0U;
        uint32_t state_revision = 0U;
        uint32_t acknowledged_input_sequence = 0U;
        shared::PlayerPosition authoritative_position{};
        uint8_t movement_capabilities = 0U;
        int32_t vertical_velocity_subcells = 0;
        GameBenchmarkFrameSample::PlayableSphere playable_sphere;
        std::optional<GameBenchmarkFrameSample::VisualLod> visual_lod = std::nullopt;
    };
    uint64_t movement_authoritative_update_count = 0U;
    std::optional<AuthoritativePlayableSphereUpdate> movement_trajectory_anchor = std::nullopt;
    bool movement_playable_sphere_complete = false;
    std::vector<AuthoritativePlayableSphereUpdate> movement_authoritative_sphere_updates;
    bool complete = false;
};

struct GameBenchmarkS1TerrainPerformanceEvidence final {
    uint32_t actual_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    uint32_t visual_lod_radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS;
    std::optional<uint64_t> ready_prompt_frame_offset_ns = std::nullopt;
    std::optional<uint64_t> playable_sphere_first_ready_offset_ns = std::nullopt;
    bool initial_deadline_missed = false;
    bool playable_deadline_missed = false;
    bool movement_started_after_current_sphere_ready = false;
    bool movement_input_during_fill = false;
    uint64_t movement_start_offset_ns = 0U;
    uint64_t movement_end_offset_ns = 0U;
    uint64_t movement_authoritative_update_count = 0U;
    std::optional<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
        movement_trajectory_anchor = std::nullopt;
    // First-loop-relative time of the fresh sphere sample; the anchor keeps its original ACK time.
    std::optional<uint64_t> movement_trajectory_anchor_sphere_sample_offset_ns = std::nullopt;
    bool movement_boundary_authoritative_updates_complete = false;
    // Accepted pre-window inputs whose authoritative ACK arrives after movement begins.
    std::vector<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
        movement_boundary_authoritative_updates;
    std::vector<GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate>
        movement_authoritative_sphere_updates;
    bool movement_playable_sphere_complete = false;
    bool movement_visual_lod_samples_complete = false;
    bool complete = false;
};

struct GameBenchmarkRawInputEvidence final {
    uint64_t input_ordinal = 0U;
    uint32_t sequence = 0U;
    uint64_t sent_offset_ns = 0U;
    int64_t scheduled_offset_ns = 0;
    GameBenchmarkMovementRouteSample movement_intent{};
    shared::Direction direction{};
};

struct GameBenchmarkServerStreamSample final {
    struct PhaseMetrics final {
        uint64_t calls = 0U;
        uint64_t duration_ns = 0U;
    };

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
    uint32_t world_generation_worker_queued_count = 0U;
    uint32_t world_generation_worker_running_count = 0U;
    uint32_t world_generation_worker_completed_uncollected_count = 0U;
    PhaseMetrics coverage_refresh;
    PhaseMetrics worker_result_publication;
    PhaseMetrics queue_refill;
    PhaseMetrics delivery;
    PhaseMetrics worker_dispatch;
    uint64_t materialization_candidate_keys_examined = 0U;
    uint64_t materialization_resident_chunk_lookups = 0U;
    uint64_t materialization_admission_attempts = 0U;
    uint64_t materialization_admissions = 0U;
    uint64_t materialization_duplicate_requests = 0U;
    uint64_t chunk_delivery_candidate_keys_examined = 0U;
    uint64_t chunk_delivery_resident_chunk_lookups = 0U;
    uint64_t chunk_delivery_batches_sent = 0U;
    uint64_t chunk_delivery_chunks_sent = 0U;
    uint64_t chunk_delivery_credits_consumed = 0U;
    uint64_t world_generation_jobs_dispatched = 0U;
    uint64_t height_tile_jobs_dispatched = 0U;
    uint64_t global_server_world_resident_chunks = 0U;
    uint32_t required_near_chunk_keys = 0U;
    uint32_t required_full_chunk_keys = 0U;
    uint32_t acknowledged_chunk_keys = 0U;
    uint32_t inflight_chunk_batches = 0U;
    uint32_t inflight_chunk_keys = 0U;
    uint32_t chunk_delivery_credits = 0U;
    bool near_required_chunks_all_resident = false;
};

struct GameBenchmarkEvidence final {
    std::string workload;
    std::string qualification_proof;
    std::string movement_trajectory_contract;
    std::optional<shared::corelang::BackendReceipt> terrain_backend_receipt = std::nullopt;
    std::string package_id;
    std::string hardware;
    std::optional<std::string> gpu;
    std::string present_mode;
    std::string build_mode;
    std::string qualification_settings_manifest;
    std::optional<shared::TerrainIdentity> terrain_identity;
    uint64_t session_join_epoch = 0U;
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
    std::optional<GameBenchmarkFrameBudgetEvidence> frame_budget = std::nullopt;
    std::optional<GameBenchmarkTerrainReadinessEvidence> terrain_readiness = std::nullopt;
    std::optional<GameBenchmarkFullRadiusEvidence> full_radius_terrain = std::nullopt;
    std::optional<GameBenchmarkS1TerrainPerformanceEvidence> s1_terrain_performance = std::nullopt;
    std::optional<GameBenchmarkPresentationTimingEvidence> presentation_timing = std::nullopt;
    std::optional<GameBenchmarkStartActionEvidence> controlled_start = std::nullopt;
    std::vector<GameBenchmarkRawInputEvidence> raw_gameplay_inputs;
    std::vector<GameBenchmarkFrameSample> raw_frames;
    std::vector<GameBenchmarkFrameCompletion> raw_fence_confirmed_frames;
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
std::string stoneFaceGrowthDiagnosticsJson(client::StoneFaceGrowthDiagnostics const& diagnostics);

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
