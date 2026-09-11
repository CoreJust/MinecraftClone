#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace acceptance {

struct FrameTimingSummary final {
    uint64_t sample_count{ 0 };
    std::chrono::nanoseconds p50{ 0 };
    std::chrono::nanoseconds p95{ 0 };
    std::chrono::nanoseconds p99{ 0 };
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
    std::chrono::milliseconds warmup{ 0 };
    std::chrono::nanoseconds sample_elapsed{ 0 };
    double presentation_requests_per_second{ 0.0 };
    FrameTimingSummary presentation_request_timings;
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
