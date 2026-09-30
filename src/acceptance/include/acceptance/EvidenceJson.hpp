#pragma once

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
    uint64_t diagnostic_render_attempt_id = 0U;
    bool diagnostic_frame_recorded = false;
    bool diagnostic_indexed_stone_quads = false;
    bool diagnostic_stone_indirect = false;
    std::optional<uint32_t> diagnostic_frame_slot;
    uint64_t diagnostic_submitted_stone_quad_count = 0U;
    uint32_t diagnostic_stone_draw_count = 0U;
    std::array<double, 7> diagnostic_camera{};
    uint64_t diagnostic_cpu_acquire_wait_duration_ns = 0U;
    uint64_t diagnostic_cpu_command_record_duration_ns = 0U;
    uint64_t diagnostic_cpu_complete_present_wait_duration_ns = 0U;
    bool diagnostic_gpu_timestamps_enabled = false;
    std::string_view diagnostic_gpu_timestamp_reason;
    uint64_t diagnostic_gpu_sample_attempt_id = 0U;
    uint32_t diagnostic_gpu_sample_slot = 0U;
    uint64_t diagnostic_gpu_sample_quad_count = 0U;
    uint32_t diagnostic_gpu_sample_draw_count = 0U;
    std::optional<uint64_t> diagnostic_gpu_terrain_duration_ns;
    std::optional<uint64_t> latest_gpu_frame_duration_ns;
};

struct GameBenchmarkEvidence final {
    std::string workload;
    std::string package_id;
    std::string hardware;
    std::optional<std::string> gpu;
    std::string present_mode;
    std::string build_mode;
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
    std::vector<GameBenchmarkFrameSample> raw_frames;
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
    std::optional<RendererBenchmarkEvidence> benchmark;
    std::optional<GameBenchmarkEvidence> game_benchmark;
    std::optional<RendererCaptureEvidence> capture;
};

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
) noexcept;

} // namespace acceptance
