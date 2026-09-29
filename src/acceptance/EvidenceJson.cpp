#include <acceptance/EvidenceJson.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string_view>
#include <utility>

namespace acceptance {

namespace {

std::string jsonString(std::string_view const value)
{
    std::string result;
    static constexpr std::array<char, 16> HEX_DIGITS{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    result.reserve(value.size() + 2);
    result += '"';
    for (char const ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default: {
                uint8_t const value = static_cast<uint8_t>(ch);
                if (value < 0x20) {
                    result += "\\u00";
                    result += HEX_DIGITS[value >> 4];
                    result += HEX_DIGITS[value & 0x0f];
                } else {
                    result += ch;
                }
                break;
            }
        }
    }
    result += '"';
    return result;
}

uint64_t millisecondsCount(std::chrono::milliseconds const duration)
{
    return duration.count() > 0
        ? static_cast<uint64_t>(duration.count())
        : 0;
}

uint64_t nanosecondsCount(std::chrono::nanoseconds const duration)
{
    return duration.count() > 0
        ? static_cast<uint64_t>(duration.count())
        : 0;
}

std::string frameTimingSummaryJson(FrameTimingSummary const& summary)
{
    return "{\n"
        "      \"sample_count\": " + std::to_string(summary.sample_count) + ",\n"
        "      \"p50\": " + std::to_string(nanosecondsCount(summary.p50)) + ",\n"
        "      \"p95\": " + std::to_string(nanosecondsCount(summary.p95)) + ",\n"
        "      \"p99\": " + std::to_string(nanosecondsCount(summary.p99)) + ",\n"
        "      \"p99_8\": " + std::to_string(nanosecondsCount(summary.p99_8)) + ",\n"
        "      \"max\": " + std::to_string(nanosecondsCount(summary.maximum)) + ",\n"
        "      \"mean\": " + std::to_string(nanosecondsCount(summary.mean)) + "\n"
        "    }";
}

std::string benchmarkJson(RendererBenchmarkEvidence const& benchmark)
{
    return "{\n"
        "    \"package_id\": " + jsonString(benchmark.package_id) + ",\n"
        "    \"platform\": " + jsonString(benchmark.platform) + ",\n"
        "    \"hardware\": " + jsonString(benchmark.hardware) + ",\n"
        "    \"gpu\": " + jsonString(benchmark.gpu) + ",\n"
        "    \"vulkan_api_version\": " + jsonString(benchmark.vulkan_api_version) + ",\n"
        "    \"present_mode\": " + jsonString(benchmark.present_mode) + ",\n"
        "    \"pipeline_path\": " + jsonString(benchmark.pipeline_path) + ",\n"
        "    \"validation_enabled\": "
            + std::string(benchmark.validation_enabled ? "true" : "false") + ",\n"
        "    \"debug_hud_enabled\": "
            + std::string(benchmark.debug_hud_enabled ? "true" : "false") + ",\n"
        "    \"warmup_ms\": " + std::to_string(millisecondsCount(benchmark.warmup)) + ",\n"
        "    \"sample_elapsed_ns\": "
            + std::to_string(nanosecondsCount(benchmark.sample_elapsed)) + ",\n"
        "    \"requested_resolution\": { \"width\": "
            + std::to_string(benchmark.requested_width) + ", \"height\": "
            + std::to_string(benchmark.requested_height) + " },\n"
        "    \"actual_resolution\": { \"width\": "
            + std::to_string(benchmark.actual_width) + ", \"height\": "
            + std::to_string(benchmark.actual_height) + " },\n"
        "    \"presentation_requests_per_second\": "
            + std::to_string(benchmark.presentation_requests_per_second) + ",\n"
        "    \"cpu_presentation_request_timings_ns\": "
            + frameTimingSummaryJson(benchmark.presentation_request_timings) + ",\n"
        "    \"cpu_acquire_wait_timings_ns\": "
            + frameTimingSummaryJson(benchmark.acquire_wait_timings) + ",\n"
        "    \"cpu_command_record_timings_ns\": "
            + frameTimingSummaryJson(benchmark.command_record_timings) + ",\n"
        "    \"cpu_complete_submit_present_wait_timings_ns\": "
            + frameTimingSummaryJson(benchmark.complete_present_wait_timings) + "\n"
        "  }";
}

std::string captureJson(RendererCaptureEvidence const& capture)
{
    return "{\n"
        "    \"image_path\": " + jsonString(capture.image_path) + ",\n"
        "    \"gpu\": " + jsonString(capture.gpu) + ",\n"
        "    \"vulkan_api_version\": " + jsonString(capture.vulkan_api_version) + ",\n"
        "    \"present_mode\": " + jsonString(capture.present_mode) + ",\n"
        "    \"pipeline_path\": " + jsonString(capture.pipeline_path) + ",\n"
        "    \"validation_enabled\": "
            + std::string(capture.validation_enabled ? "true" : "false") + ",\n"
        "    \"content_verified\": "
            + std::string(capture.content_verified ? "true" : "false") + ",\n"
        "    \"requested_resolution\": { \"width\": "
            + std::to_string(capture.requested_width) + ", \"height\": "
            + std::to_string(capture.requested_height) + " },\n"
        "    \"actual_resolution\": { \"width\": "
            + std::to_string(capture.actual_width) + ", \"height\": "
            + std::to_string(capture.actual_height) + " }\n"
        "  }";
}

std::string gameBenchmarkJson(GameBenchmarkEvidence const& benchmark)
{
    std::string result = "{\n"
        "    \"workload\": " + jsonString(benchmark.workload) + ",\n"
        "    \"measurement_kind\": \"full-game-render-submit-present-request\",\n"
        "    \"package_id\": " + jsonString(benchmark.package_id) + ",\n"
        "    \"hardware\": " + jsonString(benchmark.hardware) + ",\n"
        "    \"gpu\": " + (benchmark.gpu ? jsonString(*benchmark.gpu) : "null") + ",\n"
        "    \"present_mode\": " + jsonString(benchmark.present_mode) + ",\n"
        "    \"build_mode\": " + jsonString(benchmark.build_mode) + ",\n"
        "    \"cold_duration_ms\": " + std::to_string(benchmark.cold_duration_ms) + ",\n"
        "    \"warm_duration_ms\": " + std::to_string(benchmark.warm_duration_ms) + ",\n"
        "    \"uncapped_duration_ms\": " + std::to_string(benchmark.uncapped_duration_ms) + ",\n"
        "    \"framebuffer\": { \"width\": " + std::to_string(benchmark.framebuffer_width)
            + ", \"height\": " + std::to_string(benchmark.framebuffer_height) + " },\n"
        "    \"power_mode\": null,\n"
        "    \"displayed_cadence_hz\": null,\n"
        "    \"gpu_timing_attribution\": \"latest renderer sample; may lag request\",\n"
        "    \"resident_mesh_bytes\": null,\n"
        "    \"client_message_payload_bytes_sent\": "
            + std::to_string(benchmark.client_message_payload_bytes_sent) + ",\n"
        "    \"client_message_payload_bytes_received\": "
            + std::to_string(benchmark.client_message_payload_bytes_received) + ",\n"
        "    \"server_events_processed\": " + std::to_string(benchmark.server_events_processed) + ",\n"
        "    \"inputs_sent\": " + std::to_string(benchmark.inputs_sent) + ",\n"
        "    \"authoritative_player_updates\": "
            + std::to_string(benchmark.authoritative_player_updates) + ",\n"
        "    \"observed_wrap_crossings\": "
            + std::to_string(benchmark.observed_wrap_crossings) + ",\n"
        "    \"observed_capability_transitions\": "
            + std::to_string(benchmark.observed_capability_transitions) + ",\n"
        "    \"permission_publishes\": "
            + std::to_string(benchmark.permission_publishes) + ",\n"
        "    \"successful_present_requests\": "
            + std::to_string(benchmark.successful_present_requests) + ",\n"
        "    \"failed_present_requests\": "
            + std::to_string(benchmark.failed_present_requests) + ",\n"
        "    \"excluded_present_requests\": "
            + std::to_string(benchmark.excluded_present_requests) + ",\n"
        "    \"warm_present_requests_per_second\": "
            + std::to_string(benchmark.warm_present_requests_per_second) + ",\n"
        "    \"uncapped_present_requests_per_second\": "
            + std::to_string(benchmark.uncapped_present_requests_per_second) + ",\n"
        "    \"cold_loop_timings_ns\": " + frameTimingSummaryJson(benchmark.cold_loop_timings) + ",\n"
        "    \"warm_loop_timings_ns\": " + frameTimingSummaryJson(benchmark.warm_loop_timings) + ",\n"
        "    \"uncapped_loop_timings_ns\": "
            + frameTimingSummaryJson(benchmark.uncapped_loop_timings) + ",\n"
        "    \"server_tick_timings_ns\": " + frameTimingSummaryJson(benchmark.server_tick_timings) + ",\n"
        "    \"permission_publish_timings_ns\": "
            + frameTimingSummaryJson(benchmark.permission_publish_timings) + ",\n"
        "    \"raw_server_tick_durations_ns\": [";
    for (uint64_t index = 0U; index < benchmark.raw_server_tick_durations_ns.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        result += std::to_string(benchmark.raw_server_tick_durations_ns[index]);
    }
    result += "],\n    \"raw_permission_publish_durations_ns\": [";
    for (uint64_t index = 0U; index < benchmark.raw_permission_publish_durations_ns.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        result += std::to_string(benchmark.raw_permission_publish_durations_ns[index]);
    }
    result += "],\n    \"raw_frames\": [\n";
    for (uint64_t index = 0U; index < benchmark.raw_frames.size(); ++index) {
        GameBenchmarkFrameSample const& sample = benchmark.raw_frames[index];
        if (index != 0U) {
            result += ",\n";
        }
        result += "      { \"phase\": "
            + jsonString(sample.excluded ? "excluded" : sample.uncapped ? "uncapped"
                : sample.warm ? "warm" : "cold")
            + ", \"presentation_succeeded\": "
            + std::string(sample.presentation_succeeded ? "true" : "false")
            + ", \"started_offset_ns\": " + std::to_string(sample.started_offset_ns)
            + ", \"loop_duration_ns\": " + std::to_string(sample.loop_duration_ns)
            + ", \"render_duration_ns\": " + std::to_string(sample.render_duration_ns)
            + ", \"network_poll_duration_ns\": " + std::to_string(sample.network_poll_duration_ns)
            + ", \"height_tile_delivery_duration_ns\": "
                + std::to_string(sample.height_tile_delivery_duration_ns)
            + ", \"network_event_count\": " + std::to_string(sample.network_event_count)
            + ", \"cumulative_client_message_payload_bytes_sent\": "
                + std::to_string(sample.client_message_payload_bytes_sent)
            + ", \"cumulative_client_message_payload_bytes_received\": "
                + std::to_string(sample.client_message_payload_bytes_received)
            + ", \"resident_terrain_tile_count\": "
                + std::to_string(sample.resident_terrain_tile_count)
            + ", \"visible_surface_face_count\": "
                + std::to_string(sample.visible_surface_face_count)
            + ", \"latest_gpu_frame_duration_ns\": "
            + (sample.latest_gpu_frame_duration_ns
                ? std::to_string(*sample.latest_gpu_frame_duration_ns) : "null")
            + " }";
    }
    result += "\n    ]\n  }";
    return result;
}

} // namespace

std::string evidenceJson(RuntimeEvidence const& evidence)
{
    return "{\n"
        "  \"schema_version\": \"mc.runtime-evidence.v1\",\n"
        "  \"mode\": " + jsonString(evidence.mode) + ",\n"
        "  \"metadata\": {\n"
        "    \"scenario_version\": " + jsonString(evidence.scenario_version) + ",\n"
        "    \"profile\": " + jsonString(evidence.profile) + ",\n"
        "    \"scenario_name\": " + jsonString(evidence.scenario_name) + ",\n"
        "    \"seed\": " + std::to_string(evidence.seed) + ",\n"
        "    \"replay_id\": " + jsonString(evidence.replay_id) + "\n"
        "  },\n"
        "  \"timing\": {\n"
        "    \"elapsed_ms\": " + std::to_string(millisecondsCount(evidence.elapsed)) + ",\n"
        "    \"deadline_ms\": " + std::to_string(millisecondsCount(evidence.deadline)) + "\n"
        "  },\n"
        "  \"counters\": {\n"
        "    \"ticks\": " + std::to_string(evidence.ticks) + ",\n"
        "    \"clients_requested\": " + std::to_string(evidence.clients_requested) + ",\n"
        "    \"clients_accepted\": " + std::to_string(evidence.clients_accepted) + ",\n"
        "    \"server_events_processed\": " + std::to_string(evidence.server_events_processed) + ",\n"
        "    \"accepted_tick\": " + std::to_string(evidence.accepted_tick) + ",\n"
        "    \"last_effective_tick\": " + std::to_string(evidence.last_effective_tick) + ",\n"
        "    \"inputs_sent\": " + std::to_string(evidence.inputs_sent) + ",\n"
        "    \"camera_relative_inputs\": " + std::to_string(evidence.camera_relative_inputs) + ",\n"
        "    \"authoritative_tick_ms\": " + std::to_string(evidence.authoritative_tick_ms) + ",\n"
        "    \"expectations_passed\": " + std::to_string(evidence.expectations_passed) + "\n"
        "  },\n"
        "  \"passed\": " + std::string(evidence.passed ? "true" : "false") + ",\n"
        "  \"failure\": " + jsonString(evidence.failure) + ",\n"
    "  \"benchmark\": " + (
            evidence.benchmark.has_value()
                ? benchmarkJson(*evidence.benchmark)
                : "null"
        ) + ",\n"
        "  \"game_benchmark\": " + (
            evidence.game_benchmark.has_value()
                ? gameBenchmarkJson(*evidence.game_benchmark)
                : "null"
        ) + ",\n"
        "  \"capture\": " + (
            evidence.capture.has_value()
                ? captureJson(*evidence.capture)
                : "null"
        ) + "\n"
        "}\n";
}

std::expected<void, std::string> writeEvidenceJson(
    std::filesystem::path const& path,
    RuntimeEvidence const& evidence
)
{
    std::ofstream output{ path, std::ios::binary | std::ios::trunc };
    if (!output) {
        return std::unexpected("cannot open evidence output: " + path.string());
    }
    output << evidenceJson(evidence);
    if (!output) {
        return std::unexpected("cannot write evidence output: " + path.string());
    }
    return { };
}

RuntimeEvidence collectRuntimeEvidence(
    std::string mode,
    std::function<std::expected<RuntimeEvidence, std::string>()> const& operation
) noexcept
{
    try {
        auto result = operation();
        if (result.has_value()) {
            return std::move(*result);
        }
        return RuntimeEvidence{
            .mode = std::move(mode),
            .failure = std::move(result.error()),
            .passed = false,
        };
    } catch (std::exception const& error) {
        return RuntimeEvidence{
            .mode = std::move(mode),
            .failure = error.what(),
            .passed = false,
        };
    } catch (...) {
        return RuntimeEvidence{
            .mode = std::move(mode),
            .failure = "unknown renderer exception",
            .passed = false,
        };
    }
}

} // namespace acceptance
