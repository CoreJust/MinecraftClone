#include <acceptance/EvidenceJson.hpp>

#include <client/render/StoneFaceCapacity.hpp>
#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>

#include <acceptance/ReplayInput.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
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
                uint8_t const byte_value = static_cast<uint8_t>(ch);
                if (byte_value < 0x20) {
                    result += "\\u00";
                    result += HEX_DIGITS[static_cast<uint32_t>(byte_value) >> 4U];
                    result += HEX_DIGITS[static_cast<uint32_t>(byte_value) & 0x0fU];
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

std::string phaseMetricsJson(GameBenchmarkServerStreamSample::PhaseMetrics const& phase)
{
    return "{ \"calls\": " + std::to_string(phase.calls)
        + ", \"duration_ns\": " + std::to_string(phase.duration_ns) + " }";
}

std::string_view terrainBackendName(shared::corelang::Backend const backend) noexcept
{
    switch (backend) {
        case shared::corelang::Backend::Interpreter: return "interpreter";
        case shared::corelang::Backend::Jit: return "jit";
        case shared::corelang::Backend::Aot: return "aot";
    }
    return "unknown";
}

std::string sha256Hex(std::array<uint8_t, 32U> const& digest)
{
    static constexpr std::array<char, 16U> HEX_DIGITS{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string result;
    result.reserve(digest.size() * 2U);
    for (uint8_t const byte : digest) {
        result += HEX_DIGITS[static_cast<uint32_t>(byte >> 4U)];
        result += HEX_DIGITS[static_cast<uint32_t>(byte & 0x0fU)];
    }
    return result;
}

std::string terrainBackendReceiptJson(
    std::optional<shared::corelang::BackendReceipt> const& receipt
)
{
    if (!receipt) {
        return "null";
    }
    return "{ \"requested_backend\": " + jsonString(terrainBackendName(receipt->requested_backend))
        + ", \"actual_backend\": " + jsonString(terrainBackendName(receipt->actual_backend))
        + ", \"source_sha256\": " + jsonString(sha256Hex(receipt->source_sha256))
        + ", \"bytecode_sha256\": " + jsonString(sha256Hex(receipt->bytecode_sha256))
        + ", \"native_image_sha256\": "
        + (receipt->native_image_sha256 ? jsonString(sha256Hex(*receipt->native_image_sha256)) : "null")
        + ", \"compiler_version\": " + jsonString(receipt->compiler_version)
        + ", \"target\": " + jsonString(receipt->target) + " }";
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

std::string frameBudgetJson(GameBenchmarkFrameBudgetEvidence const& evidence)
{
    return "{ \"measurement_scope\": \"fence-confirmed-render-frame-completion-observations\", "
        "\"target_frame_duration_ns\": " + std::to_string(evidence.target_frame_duration_ns)
        + ", \"required_in_budget_time_permille\": "
        + std::to_string(evidence.required_in_budget_time_permille)
        + ", \"measurement_window_start_offset_ns\": "
        + std::to_string(evidence.measurement_window_start_offset_ns)
        + ", \"measurement_window_end_offset_ns\": "
        + std::to_string(evidence.measurement_window_end_offset_ns)
        + ", \"required_window_duration_ns\": "
        + std::to_string(evidence.required_window_duration_ns)
        + ", \"measured_time_ns\": " + std::to_string(evidence.measured_time_ns)
        + ", \"in_budget_time_ns\": " + std::to_string(evidence.in_budget_time_ns)
        + ", \"sample_count\": " + std::to_string(evidence.sample_count)
        + ", \"fence_confirmed_frame_count\": " + std::to_string(evidence.fence_confirmed_frame_count)
        + ", \"measurement_window_complete\": "
        + (evidence.measurement_window_complete ? "true" : "false")
        + ", \"passed\": " + (evidence.passed ? "true" : "false")
        + ", \"required_for_benchmark_pass\": "
        + (evidence.required_for_benchmark_pass ? "true" : "false") + " }";
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

std::string diagnosticCameraJson(std::array<double, 7> const& camera)
{
    std::string result = "[";
    for (size_t index = 0U; index < camera.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        std::array<char, 64> buffer{};
        auto const converted = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            camera[index],
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10
        );
        result.append(buffer.data(), converted.ptr);
    }
    return result + "]";
}

std::string jsonNumber(double const value)
{
    std::array<char, 64> buffer{};
    auto const converted = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10
    );
    return std::string{buffer.data(), converted.ptr};
}

[[nodiscard]]
std::string_view presentationAcquireStageName(GameBenchmarkPresentationAcquireStage const stage)
{
    switch (stage) {
    case GameBenchmarkPresentationAcquireStage::SubmittedFrameFence:
        return "SubmittedFrameFence";
    case GameBenchmarkPresentationAcquireStage::AcquireFence:
        return "AcquireFence";
    case GameBenchmarkPresentationAcquireStage::PresentFence:
        return "PresentFence";
    case GameBenchmarkPresentationAcquireStage::SwapchainImage:
        return "SwapchainImage";
    }
    throw std::logic_error("unknown serialized presentation acquisition stage");
}

[[nodiscard]]
std::string presentationAcquireDiagnosticsJson(GameBenchmarkPresentationAcquireSample const& sample)
{
    if (sample.stage_count > sample.stages.size()) {
        throw std::invalid_argument("presentation acquisition diagnostic exceeds its fixed stage bound");
    }
    std::string result = "{ \"captured\": ";
    result += sample.captured ? "true" : "false";
    result += ", \"frame_slot\": " + std::to_string(sample.frame_slot);
    result += ", \"image_index\": ";
    result += sample.image_index ? std::to_string(*sample.image_index) : "null";
    result += ", \"stages\": [";
    for (uint32_t index = 0U; index < sample.stage_count; ++index) {
        if (index != 0U) {
            result += ", ";
        }
        GameBenchmarkPresentationAcquireStageSample const& stage = sample.stages[index];
        result += "{ \"stage\": ";
        result += jsonString(presentationAcquireStageName(stage.stage));
        result += ", \"duration_ns\": " + std::to_string(stage.duration_ns);
        result += ", \"vk_result\": " + std::to_string(stage.result) + " }";
    }
    return result + "] }";
}

std::string meshWorkStagesJson(GameBenchmarkMeshStageSummary const& stages)
{
    return "{ \"client_pending_admission_count\": "
        + std::to_string(stages.client_pending_admission_count)
        + ", \"worker_queued_count\": " + std::to_string(stages.worker_queued_count)
        + ", \"worker_building_count\": " + std::to_string(stages.worker_building_count)
        + ", \"completed_result_waiting_count\": "
        + std::to_string(stages.completed_result_waiting_count)
        + ", \"oldest_client_pending_near_observed_age_ns\": "
        + std::to_string(stages.oldest_client_pending_near_observed_age_ns)
        + ", \"oldest_worker_queued_age_ns\": " + std::to_string(stages.oldest_worker_queued_age_ns)
        + ", \"oldest_worker_building_age_ns\": " + std::to_string(stages.oldest_worker_building_age_ns)
        + ", \"oldest_completed_result_waiting_age_ns\": "
        + std::to_string(stages.oldest_completed_result_waiting_age_ns) + " }";
}

std::string meshResultDrainJson(GameBenchmarkMeshResultDrainSummary const& drain)
{
    return "{ \"drained_result_count\": " + std::to_string(drain.drained_result_count)
        + ", \"stale_epoch_discard_count\": " + std::to_string(drain.stale_epoch_discard_count)
        + ", \"no_longer_interested_or_resident_discard_count\": "
            + std::to_string(drain.no_longer_interested_or_resident_discard_count)
        + ", \"source_mismatch_requeue_count\": " + std::to_string(drain.source_mismatch_requeue_count)
        + ", \"target_detail_mismatch_requeue_count\": "
            + std::to_string(drain.target_detail_mismatch_requeue_count)
        + ", \"cannot_publish_requeue_count\": " + std::to_string(drain.cannot_publish_requeue_count)
        + ", \"publish_attempt_count\": " + std::to_string(drain.publish_attempt_count)
        + ", \"publish_success_count\": " + std::to_string(drain.publish_success_count)
        + ", \"publish_failure_count\": " + std::to_string(drain.publish_failure_count)
        + ", \"seam_planning_duration_ns\": " + std::to_string(drain.seam_planning_duration_ns)
        + ", \"renderer_batch_upsert_duration_ns\": "
            + std::to_string(drain.renderer_batch_upsert_duration_ns)
        + ", \"renderer_free_range_prepare_duration_ns\": "
            + std::to_string(drain.renderer_free_range_prepare_duration_ns)
        + ", \"renderer_mesh_prepare_duration_ns\": "
            + std::to_string(drain.renderer_mesh_prepare_duration_ns)
        + ", \"renderer_capacity_prepare_duration_ns\": "
            + std::to_string(drain.renderer_capacity_prepare_duration_ns)
        + ", \"renderer_commit_upload_duration_ns\": "
            + std::to_string(drain.renderer_commit_upload_duration_ns) + " }";
}

std::string terrainNearSampleJson(GameBenchmarkFrameSample::TerrainNearSample const& sample)
{
    return "{ \"center\": "
        + (sample.center_valid
            ? "{ \"x\": " + std::to_string(sample.center_x) + ", \"y\": "
                + std::to_string(sample.center_y) + " }"
            : "null")
        + ", \"desired\": " + std::to_string(sample.desired_tile_count)
        + ", \"resident\": " + std::to_string(sample.resident_tile_count)
        + ", \"nonresident\": " + std::to_string(sample.nonresident_tile_count)
        + ", \"current_mesh_visible\": " + std::to_string(sample.current_mesh_visible_count)
        + ", \"pending\": " + std::to_string(sample.pending_mesh_count)
        + ", \"active\": " + std::to_string(sample.active_mesh_job_count)
        + ", \"unclassified_missing\": " + std::to_string(sample.unclassified_missing_count)
        + ", \"accepted_near_admission_delta\": "
            + std::to_string(sample.accepted_near_admission_delta)
        + ", \"accepted_far_admission_delta\": "
            + std::to_string(sample.accepted_far_admission_delta)
        + ", \"far_admission_while_near_pending_delta\": "
            + std::to_string(sample.far_admission_while_near_pending_delta)
        + ", \"mesh_work_stages\": " + meshWorkStagesJson(sample.mesh_work_stages)
        + ", \"mesh_result_drain\": " + meshResultDrainJson(sample.mesh_result_drain)
        + " }";
}

std::string terrainRadialBandsJson(std::array<GameBenchmarkFrameSample::TerrainRadialBand, 16U> const& bands)
{
    std::string result = "[";
    for (size_t index = 0U; index < bands.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        auto const& band = bands[index];
        result += "{ \"desired\": " + std::to_string(band.desired_count)
            + ", \"resident\": " + std::to_string(band.resident_count)
            + ", \"nonresident\": " + std::to_string(band.nonresident_count)
            + ", \"current_mesh_visible\": " + std::to_string(band.current_mesh_visible_count)
            + ", \"stale_or_missing_mesh\": " + std::to_string(band.stale_or_missing_mesh_count) + " }";
    }
    result.push_back(']');
    return result;
}

std::string meshEpisodesJson(GameBenchmarkFrameSample::MeshEpisodes const& episodes)
{
    auto const array_json = [](auto const& values) {
        std::string result = "[";
        for (size_t index = 0U; index < values.size(); ++index) {
            if (index != 0U) {
                result += ", ";
            }
            result += std::to_string(values[index]);
        }
        return result + "]";
    };
    return "{ \"scope\": \"interest-entry-to-first-current-handle-publication\", "
        "\"latency_bucket_upper_bounds_ns\": [100000000, 250000000, 500000000, "
        "1000000000, 2000000000, 5000000000, null], "
        "\"current_band_width_tiles\": 16, "
        "\"resident_age_clock\": \"first-resident-enqueue-observation\", "
        "\"censor_residency_scope\": \"ever-observed-resident-during-interest-episode\", "
        "\"departure_residency_scope\": \"current-residency-at-interest-departure-or-explicit-unknown\", "
        "\"entered_count\": " + std::to_string(episodes.entered_count)
        + ", \"completed_count\": " + std::to_string(episodes.completed_count)
        + ", \"censored_count\": " + std::to_string(episodes.censored_count)
        + ", \"completed_total_ns\": " + std::to_string(episodes.completed_total_ns)
        + ", \"completed_max_ns\": " + std::to_string(episodes.completed_max_ns)
        + ", \"censored_max_ns\": " + std::to_string(episodes.censored_max_ns)
        + ", \"tracked_count\": " + std::to_string(episodes.tracked_count)
        + ", \"pending_count\": " + std::to_string(episodes.pending_count)
        + ", \"oldest_pending_ns\": " + std::to_string(episodes.oldest_pending_ns)
        + ", \"oldest_resident_pending_ns\": " + std::to_string(episodes.oldest_resident_pending_ns)
        + ", \"completed_latency_buckets\": " + array_json(episodes.completed_latency_buckets)
        + ", \"censored_latency_buckets\": " + array_json(episodes.censored_latency_buckets)
        + ", \"never_resident_censored_count\": " + std::to_string(episodes.never_resident_censored_count)
        + ", \"resident_censored_count\": " + std::to_string(episodes.resident_censored_count)
        + ", \"resident_censored_max_wait_ns\": " + std::to_string(episodes.resident_censored_max_wait_ns)
        + ", \"never_resident_censored_entry_latency_buckets\": "
            + array_json(episodes.never_resident_censored_entry_latency_buckets)
        + ", \"resident_censored_wait_latency_buckets\": "
            + array_json(episodes.resident_censored_wait_latency_buckets)
        + ", \"resident_at_departure_censored_entry_latency_buckets\": "
            + array_json(episodes.resident_at_departure_censored_entry_latency_buckets)
        + ", \"nonresident_at_departure_censored_entry_latency_buckets\": "
            + array_json(episodes.nonresident_at_departure_censored_entry_latency_buckets)
        + ", \"unknown_departure_residency_censored_count\": "
            + std::to_string(episodes.unknown_departure_residency_censored_count)
        + ", \"global_active_mesh_job_count\": " + std::to_string(episodes.global_active_mesh_job_count)
        + ", \"global_worker_result_count\": " + std::to_string(episodes.global_worker_result_count)
        + ", \"global_worker_outstanding_count\": " + std::to_string(episodes.global_worker_outstanding_count)
        + ", \"pending_by_current_band\": " + array_json(episodes.pending_by_current_band)
        + ", \"oldest_pending_ns_by_current_band\": " + array_json(episodes.oldest_pending_ns_by_current_band)
        + " }";
}

std::string terrainReadinessJson(GameBenchmarkTerrainReadinessEvidence const& evidence)
{
    return "{ \"contract\": \"mc.current-near-terrain-readiness.v1\", "
        "\"measurement_scope\": \"current-center-radius-7\", "
        "\"near_radius_tiles\": " + std::to_string(evidence.near_radius_tiles)
        + ", \"readiness_deadline_ms\": " + std::to_string(evidence.readiness_deadline_ms)
        + ", \"readiness_deadline_kind\": \"diagnostic-progress-bound-not-performance-claim\""
        + ", \"drain_ms\": " + std::to_string(evidence.readiness_drain_ms)
        + ", \"center_sample_count\": " + std::to_string(evidence.center_sample_count)
        + ", \"center_change_count\": " + std::to_string(evidence.center_change_count)
        + ", \"current_near_sample_count\": " + std::to_string(evidence.current_near_sample_count)
        + ", \"incomplete_current_near_sample_count\": "
            + std::to_string(evidence.incomplete_current_near_sample_count)
        + ", \"max_missing_current_mesh_count\": "
            + std::to_string(evidence.max_missing_current_mesh_count)
        + ", \"current_near_coverage_complete\": "
            + std::string(evidence.current_near_coverage_complete ? "true" : "false")
        + ", \"eligible_resident_missing_mesh_episodes\": "
            + std::to_string(evidence.eligible_mesh_episode_count)
        + ", \"ready_mesh_episodes\": " + std::to_string(evidence.ready_mesh_episode_count)
        + ", \"left_near_unready_episodes\": "
            + std::to_string(evidence.left_near_unready_episode_count)
        + ", \"source_lost_before_ready_episodes\": "
            + std::to_string(evidence.source_lost_before_ready_episode_count)
        + ", \"deadline_miss_count\": " + std::to_string(evidence.deadline_miss_count)
        + ", \"open_mesh_episode_count\": " + std::to_string(evidence.open_mesh_episode_count)
        + ", \"worst_ready_latency_ns\": "
            + (evidence.has_max_ready_latency ? std::to_string(evidence.max_ready_latency_ns) : "null")
        + ", \"worst_ready_key\": "
            + (evidence.has_max_ready_latency
                ? "{ \"x\": " + std::to_string(evidence.max_ready_key_x) + ", \"y\": "
                    + std::to_string(evidence.max_ready_key_y) + " }"
                : "null")
        + ", \"worst_ready_start_center\": "
            + (evidence.has_max_ready_latency
                ? "{ \"x\": " + std::to_string(evidence.max_ready_start_center_x) + ", \"y\": "
                    + std::to_string(evidence.max_ready_start_center_y) + " }"
                : "null")
        + ", \"accepted_near_mesh_admissions\": "
            + std::to_string(evidence.accepted_near_admissions)
        + ", \"accepted_far_mesh_admissions\": "
            + std::to_string(evidence.accepted_far_admissions)
        + ", \"far_mesh_admissions_while_near_pending\": "
            + std::to_string(evidence.far_admissions_while_near_pending)
        + ", \"unclassified_current_near_missing_observations\": "
            + std::to_string(evidence.unclassified_missing_observation_count)
        + ", \"final_center_valid\": " + std::string(evidence.final_center_valid ? "true" : "false")
        + ", \"final_desired\": " + std::to_string(evidence.final_desired_tile_count)
        + ", \"final_resident\": " + std::to_string(evidence.final_resident_tile_count)
        + ", \"final_nonresident\": " + std::to_string(evidence.final_nonresident_tile_count)
        + ", \"final_current_mesh_visible\": "
            + std::to_string(evidence.final_current_mesh_visible_count)
        + ", \"final_pending\": " + std::to_string(evidence.final_pending_mesh_count)
        + ", \"final_active\": " + std::to_string(evidence.final_active_mesh_job_count)
        + ", \"final_unclassified_missing\": "
            + std::to_string(evidence.final_unclassified_missing_count)
        + ", \"complete\": " + std::string(evidence.complete ? "true" : "false") + " }";
}

std::string optionalCountJson(std::optional<uint32_t> const count)
{
    return count ? std::to_string(*count) : "null";
}

std::string optionalUint64Json(std::optional<uint64_t> const value)
{
    return value ? std::to_string(*value) : "null";
}

std::string optionalBoolJson(std::optional<bool> const value)
{
    return value ? (*value ? "true" : "false") : "null";
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

std::string playableSphereJson(GameBenchmarkFrameSample::PlayableSphere const& sphere)
{
    std::string result = "{ \"radius_blocks\": " + std::to_string(sphere.radius_blocks)
        + ", \"active_coverage_epoch\": "
            + (sphere.active_coverage_epoch ? std::to_string(*sphere.active_coverage_epoch) : "null")
        + ", \"active_coverage_center\": " + optionalPlayerPositionJson(sphere.active_coverage_center)
        + ", \"world_loading_center\": " + optionalPlayerPositionJson(sphere.world_loading_center)
        + ", \"ready_chunks\": " + std::to_string(sphere.ready_chunks)
        + ", \"required_chunks\": " + std::to_string(sphere.required_chunks)
        + ", \"ready\": " + (sphere.ready ? "true" : "false");
    if (!sphere.coverage_scope.empty()) {
        result += ", \"coverage_scope\": " + jsonString(sphere.coverage_scope)
            + ", \"required_authoritative_columns\": " + std::to_string(sphere.required_authoritative_columns)
            + ", \"resident_authoritative_columns\": " + std::to_string(sphere.resident_authoritative_columns)
            + ", \"vertical_chunk_count\": " + std::to_string(sphere.vertical_chunk_count)
            + ", \"authoritative_columns_center\": " + optionalPlayerPositionJson(sphere.authoritative_columns_center);
    }
    return result + " }";
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

std::string visualLodJson(GameBenchmarkFrameSample::VisualLod const& sample)
{
    return "{ \"contract\": \"mc.presented-terrain-coverage.v1\", \"radius_blocks\": "
        + std::to_string(sample.radius_blocks)
        + ", \"scene_active\": " + (sample.scene_active ? "true" : "false")
        + ", \"leaf_count\": " + std::to_string(sample.leaf_count)
        + ", \"actual_column_count\": " + std::to_string(sample.actual_column_count)
        + ", \"terrain_identity\": " + terrainIdentityJson(sample.terrain_identity)
        + ", \"session_join_epoch\": " + std::to_string(sample.session_join_epoch)
        + ", \"coverage_epoch\": " + std::to_string(sample.coverage_epoch)
        + ", \"submitted_frame_count\": " + std::to_string(sample.submitted_frame_count)
        + ", \"scene_center\": { \"x\": " + std::to_string(sample.scene_center_x)
            + ", \"y\": " + std::to_string(sample.scene_center_y) + " }"
        + ", \"actual_radius_blocks\": " + std::to_string(sample.actual_radius_blocks)
        + ", \"required_actual_columns\": " + std::to_string(sample.required_actual_columns)
        + ", \"presented_current_actual_columns\": " + std::to_string(sample.presented_current_actual_columns)
        + ", \"coverage_center\": " + optionalPlayerPositionJson(sample.coverage_center)
        + ", \"sources_current\": " + (sample.sources_current ? "true" : "false") + " }";
}

std::string qualificationSettingsJson(
    std::optional<GameBenchmarkFrameSample::QualificationSettings> const& sample
)
{
    if (!sample) {
        return "null";
    }
    return "{ \"framebuffer\": { \"width\": " + std::to_string(sample->framebuffer_width)
        + ", \"height\": " + std::to_string(sample->framebuffer_height) + " }"
        + ", \"render_distance\": " + std::to_string(sample->render_distance)
        + ", \"debug_hud_enabled\": " + (sample->debug_hud_enabled ? "true" : "false")
        + ", \"terrain_model\": " + jsonString(sample->terrain_model)
        + ", \"camera_perspective\": " + jsonString(sample->camera_perspective)
        + ", \"world_mode\": " + jsonString(sample->world_mode) + " }";
}

std::string presentedPlayerJson(std::optional<GameBenchmarkFrameSample::PresentedPlayer> const& sample)
{
    if (!sample) {
        return "null";
    }
    return "{ \"body\": [" + std::to_string(sample->body[0]) + ", " + std::to_string(sample->body[1])
        + ", " + std::to_string(sample->body[2]) + "]"
        + ", \"submitted_frame_count\": " + std::to_string(sample->submitted_frame_count)
        + ", \"sampled_offset_ns\": " + std::to_string(sample->sampled_offset_ns) + " }";
}

std::string optionalVisualLodField(
    std::optional<GameBenchmarkFrameSample::VisualLod> const& sample
)
{
    return sample ? ", \"visual_lod\": " + visualLodJson(*sample) : std::string{};
}

std::string fullTerrainSnapshotJson(GameBenchmarkFullTerrainSnapshot const& snapshot)
{
    std::string detail_counts = "null";
    if (snapshot.current_visible_mesh_detail_counts) {
        std::array<uint32_t, 5U> const& counts = *snapshot.current_visible_mesh_detail_counts;
        detail_counts = "{ \"coarse\": " + std::to_string(counts[0U])
            + ", \"coarse2\": " + std::to_string(counts[1U])
            + ", \"coarse4\": " + std::to_string(counts[2U])
            + ", \"fine\": " + std::to_string(counts[3U])
            + ", \"distant\": " + std::to_string(counts[4U]) + " }";
    }
    std::string result = "{ \"phase\": " + jsonString(snapshot.phase)
        + ", \"phase_elapsed_ms\": " + std::to_string(snapshot.phase_elapsed_ms)
        + ", \"scan_performed\": " + (snapshot.scan_performed ? "true" : "false")
        + ", \"potentially_ready\": " + (snapshot.potentially_ready ? "true" : "false")
        + ", \"exact_current_coverage\": " + optionalBoolJson(snapshot.exact_current_coverage)
        + ", \"world_loading_state\": " + jsonString(snapshot.world_loading_state)
        + ", \"playable_sphere_radius_blocks\": " + std::to_string(snapshot.playable_sphere_radius_blocks)
        + ", \"playable_sphere_ready_chunks\": " + std::to_string(snapshot.playable_sphere_ready_chunks)
        + ", \"playable_sphere_required_chunks\": " + std::to_string(snapshot.playable_sphere_required_chunks)
        + ", \"playable_sphere_ready\": " + (snapshot.playable_sphere_ready ? "true" : "false")
        + ", \"center\": "
            + (snapshot.center_valid
                ? "{ \"x\": " + std::to_string(snapshot.center_x) + ", \"y\": "
                    + std::to_string(snapshot.center_y) + " }"
                : "null")
        + ", \"desired\": " + std::to_string(snapshot.desired_count)
        + ", \"resident_total\": " + std::to_string(snapshot.resident_total_count)
        + ", \"resident_desired\": " + optionalCountJson(snapshot.resident_desired_count)
        + ", \"nonresident_desired\": " + optionalCountJson(snapshot.nonresident_desired_count)
        + ", \"visible_meshes\": " + std::to_string(snapshot.visible_mesh_count)
        + ", \"current_handle_mesh_visible\": "
            + optionalCountJson(snapshot.current_handle_mesh_visible_count)
        + ", \"stale_or_missing_mesh\": " + optionalCountJson(snapshot.stale_or_missing_mesh_count)
        + ", \"unclassified_resident_missing\": "
            + optionalCountJson(snapshot.unclassified_resident_missing_count)
        + ", \"visible_outside_interest\": " + optionalCountJson(snapshot.visible_outside_interest_count)
        + ", \"current_visible_mesh_detail_counts\": " + detail_counts
        + ", \"unclassified_current_visible_mesh_detail\": "
            + optionalCountJson(snapshot.unclassified_current_visible_mesh_detail_count)
        + ", \"renderer_mesh_slots\": " + std::to_string(snapshot.renderer_mesh_count)
        + ", \"renderer_face_count\": " + std::to_string(snapshot.renderer_face_count)
        + ", \"renderer_draw_face_count\": " + std::to_string(snapshot.renderer_draw_face_count)
        + ", \"renderer_stone_face_capacity\": "
            + std::to_string(snapshot.renderer_stone_face_capacity)
        + ", \"renderer_stone_face_growth\": "
            + stoneFaceGrowthDiagnosticsJson(snapshot.renderer_stone_face_growth)
        + ", \"pending_delivery_batches\": " + std::to_string(snapshot.pending_delivery_batch_count)
        + ", \"pending_residency_changes\": " + std::to_string(snapshot.pending_residency_change_count)
        + ", \"pending_mesh_queue\": " + std::to_string(snapshot.pending_mesh_count)
        + ", \"active_mesh_jobs\": " + std::to_string(snapshot.active_mesh_job_count)
        + ", \"dirty_mesh_jobs\": " + std::to_string(snapshot.dirty_mesh_job_count)
        + ", \"worker_results\": " + std::to_string(snapshot.worker_result_count)
        + ", \"worker_outstanding\": " + std::to_string(snapshot.worker_outstanding_count)
        + ", \"pending_removals\": " + std::to_string(snapshot.pending_removal_count)
        + ", \"current_near_mesh_work_stages\": "
            + meshWorkStagesJson(snapshot.current_near_mesh_work_stages)
        + ", \"radial_band_tile_width\": 16, \"radial_bands\": "
        + (snapshot.scan_performed ? "[" : "null");
    if (snapshot.scan_performed) {
        for (size_t index = 0U; index < snapshot.radial_bands.size(); ++index) {
            if (index != 0U) {
                result += ", ";
            }
            GameBenchmarkFullTerrainSnapshot::RadialBand const& band = snapshot.radial_bands[index];
            result += "{ \"desired\": " + std::to_string(band.desired_count)
                + ", \"resident\": " + std::to_string(band.resident_count)
                + ", \"nonresident\": " + std::to_string(band.nonresident_count)
                + ", \"current_mesh_visible\": " + std::to_string(band.current_mesh_visible_count)
                + ", \"stale_or_missing_mesh\": " + std::to_string(band.stale_or_missing_mesh_count) + " }";
        }
        result += ']';
    }
    result += " }";
    return result;
}

std::string fullRadiusTerrainJson(GameBenchmarkFullRadiusEvidence const& evidence)
{
    std::string result = "{ \"contract\": \"mc.full-radius-terrain-barriers.v1\", "
        "\"render_distance\": " + std::to_string(evidence.render_distance)
        + ", \"expected_tile_count\": " + std::to_string(evidence.expected_tile_count)
        + ", \"warmup_timeout_ms\": " + std::to_string(evidence.warmup_timeout_ms)
        + ", \"warmup_timeout_kind\": \"diagnostic-bound-not-performance-claim\""
        + ", \"initial_warmup_elapsed_ms\": " + std::to_string(evidence.initial_warmup_elapsed_ms)
        + ", \"initial_barrier_reached\": " + (evidence.initial_barrier_reached ? "true" : "false")
        + ", \"initial_barrier\": " + fullTerrainSnapshotJson(evidence.initial_barrier)
        + ", \"baseline_stationary_elapsed_ms\": "
            + std::to_string(evidence.baseline_stationary_elapsed_ms)
        + ", \"loop_timing_scope\": \"all-stationary-loop-attempts\""
        + ", \"successful_present_request_interval_scope\": "
            "\"consecutive-successful-request-completions-not-physical-display-cadence\""
        + ", \"baseline_loop_attempts\": " + std::to_string(evidence.baseline_loop_attempts)
        + ", \"baseline_failed_present_requests\": "
            + std::to_string(evidence.baseline_failed_present_requests)
        + ", \"baseline_present_requests\": " + std::to_string(evidence.baseline_present_requests)
        + ", \"baseline_present_requests_per_second\": "
            + std::to_string(evidence.baseline_present_requests_per_second)
        + ", \"baseline_loop_timings_ns\": " + frameTimingSummaryJson(evidence.baseline_loop_timings)
        + ", \"baseline_successful_present_request_intervals_ns\": "
            + frameTimingSummaryJson(evidence.baseline_successful_present_request_intervals)
        + ", \"movement_elapsed_ms\": " + std::to_string(evidence.movement_elapsed_ms)
        + ", \"movement_center_change_count\": " + std::to_string(evidence.movement_center_change_count)
        + ", \"movement_start_center\": "
            + (evidence.movement_start_center_valid
                ? "{ \"x\": " + std::to_string(evidence.movement_start_center_x) + ", \"y\": "
                    + std::to_string(evidence.movement_start_center_y) + " }" : "null")
        + ", \"movement_end_center\": "
            + (evidence.movement_end_center_valid
                ? "{ \"x\": " + std::to_string(evidence.movement_end_center_x) + ", \"y\": "
                    + std::to_string(evidence.movement_end_center_y) + " }" : "null")
        + ", \"recovery_warmup_elapsed_ms\": " + std::to_string(evidence.recovery_warmup_elapsed_ms)
        + ", \"recovery_barrier_reached\": " + (evidence.recovery_barrier_reached ? "true" : "false")
        + ", \"recovery_barrier\": " + fullTerrainSnapshotJson(evidence.recovery_barrier)
        + ", \"recovered_stationary_elapsed_ms\": "
            + std::to_string(evidence.recovered_stationary_elapsed_ms)
        + ", \"recovered_loop_attempts\": " + std::to_string(evidence.recovered_loop_attempts)
        + ", \"recovered_failed_present_requests\": "
            + std::to_string(evidence.recovered_failed_present_requests)
        + ", \"recovered_present_requests\": " + std::to_string(evidence.recovered_present_requests)
        + ", \"recovered_present_requests_per_second\": "
            + std::to_string(evidence.recovered_present_requests_per_second)
        + ", \"recovered_loop_timings_ns\": " + frameTimingSummaryJson(evidence.recovered_loop_timings)
        + ", \"recovered_successful_present_request_intervals_ns\": "
            + frameTimingSummaryJson(evidence.recovered_successful_present_request_intervals)
        + ", \"movement_playable_sphere_contract\": "
            "\"mc.authoritative-materialized-playable-sphere.v1\""
        + ", \"movement_playable_sphere_sample_scope\": "
            + jsonString(evidence.movement_playable_sphere_sample_scope)
        + ", \"movement_authoritative_update_count\": "
            + std::to_string(evidence.movement_authoritative_update_count)
        + ", \"movement_trajectory_anchor\": "
            + (evidence.movement_trajectory_anchor
                ? "{ \"update_ordinal\": "
                    + std::to_string(evidence.movement_trajectory_anchor->update_ordinal)
                    + ", \"offset_ns\": " + std::to_string(evidence.movement_trajectory_anchor->offset_ns)
                    + ", \"state_revision\": "
                        + std::to_string(evidence.movement_trajectory_anchor->state_revision)
                    + ", \"acknowledged_input_sequence\": "
                        + std::to_string(evidence.movement_trajectory_anchor->acknowledged_input_sequence)
                    + ", \"authoritative_position\": "
                        + playerPositionJson(evidence.movement_trajectory_anchor->authoritative_position)
                    + ", \"movement_capabilities\": "
                        + std::to_string(evidence.movement_trajectory_anchor->movement_capabilities)
                    + ", \"vertical_velocity_subcells\": "
                        + std::to_string(evidence.movement_trajectory_anchor->vertical_velocity_subcells)
                    + ", \"playable_sphere\": "
                        + playableSphereJson(evidence.movement_trajectory_anchor->playable_sphere) + " }"
                : "null")
        + ", \"movement_playable_sphere_complete\": "
            + (evidence.movement_playable_sphere_complete ? "true" : "false")
        + ", \"movement_authoritative_sphere_updates\": [";
    for (size_t index = 0U; index < evidence.movement_authoritative_sphere_updates.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& update =
            evidence.movement_authoritative_sphere_updates[index];
        result += "{ \"update_ordinal\": " + std::to_string(update.update_ordinal)
            + ", \"offset_ns\": " + std::to_string(update.offset_ns)
            + ", \"state_revision\": " + std::to_string(update.state_revision)
            + ", \"acknowledged_input_sequence\": "
                + std::to_string(update.acknowledged_input_sequence)
            + ", \"authoritative_position\": " + playerPositionJson(update.authoritative_position)
            + ", \"movement_capabilities\": " + std::to_string(update.movement_capabilities)
            + ", \"vertical_velocity_subcells\": "
                + std::to_string(update.vertical_velocity_subcells)
            + ", \"playable_sphere\": " + playableSphereJson(update.playable_sphere)
            + optionalVisualLodField(update.visual_lod) + " }";
    }
    result += "], \"complete\": ";
    result += evidence.complete ? "true" : "false";
    result += " }";
    return result;
}

std::string s1TerrainPerformanceJson(GameBenchmarkS1TerrainPerformanceEvidence const& evidence)
{
    auto const updateJson = [](GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const& update) {
        return "{ \"update_ordinal\": " + std::to_string(update.update_ordinal)
            + ", \"offset_ns\": " + std::to_string(update.offset_ns)
            + ", \"state_revision\": " + std::to_string(update.state_revision)
            + ", \"acknowledged_input_sequence\": "
                + std::to_string(update.acknowledged_input_sequence)
            + ", \"authoritative_position\": " + playerPositionJson(update.authoritative_position)
            + ", \"movement_capabilities\": " + std::to_string(update.movement_capabilities)
            + ", \"vertical_velocity_subcells\": "
                + std::to_string(update.vertical_velocity_subcells)
            + ", \"playable_sphere\": " + playableSphereJson(update.playable_sphere)
            + optionalVisualLodField(update.visual_lod) + " }";
    };

    std::string result = "{ \"contract\": \"mc.s1-lod-performance.v2\", "
        "\"timing_origin\": \"world-request-accepted\", \"initial_radius_blocks\": 48, "
        "\"actual_radius_blocks\": " + std::to_string(evidence.actual_radius_blocks)
        + ", \"visual_lod_radius_blocks\": " + std::to_string(evidence.visual_lod_radius_blocks)
        + ", \"ready_prompt_frame_offset_ns\": "
            + optionalUint64Json(evidence.ready_prompt_frame_offset_ns)
        + ", \"playable_sphere_first_ready_offset_ns\": "
            + optionalUint64Json(evidence.playable_sphere_first_ready_offset_ns)
        + ", \"initial_deadline_missed\": "
            + (evidence.initial_deadline_missed ? "true" : "false")
        + ", \"playable_deadline_missed\": "
            + (evidence.playable_deadline_missed ? "true" : "false")
        + ", \"movement_started_after_current_sphere_ready\": "
            + (evidence.movement_started_after_current_sphere_ready ? "true" : "false")
        + ", \"movement_input_during_fill\": "
            + (evidence.movement_input_during_fill ? "true" : "false")
        + ", \"movement_start_offset_ns\": " + std::to_string(evidence.movement_start_offset_ns)
        + ", \"movement_end_offset_ns\": " + std::to_string(evidence.movement_end_offset_ns)
        + ", \"movement_authoritative_update_count\": "
            + std::to_string(evidence.movement_authoritative_update_count)
        + ", \"movement_trajectory_anchor\": "
            + (evidence.movement_trajectory_anchor
                ? updateJson(*evidence.movement_trajectory_anchor) : "null")
        + ", \"movement_trajectory_anchor_sphere_sample_offset_ns\": "
            + optionalUint64Json(evidence.movement_trajectory_anchor_sphere_sample_offset_ns)
        + ", \"movement_boundary_authoritative_updates_complete\": "
            + (evidence.movement_boundary_authoritative_updates_complete ? "true" : "false")
        + ", \"movement_boundary_authoritative_updates\": [";
    for (size_t index = 0U; index < evidence.movement_boundary_authoritative_updates.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        result += updateJson(evidence.movement_boundary_authoritative_updates[index]);
    }
    result += "], \"movement_authoritative_sphere_updates\": [";
    for (size_t index = 0U; index < evidence.movement_authoritative_sphere_updates.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        result += updateJson(evidence.movement_authoritative_sphere_updates[index]);
    }
    result += "], \"movement_playable_sphere_complete\": ";
    result += evidence.movement_playable_sphere_complete ? "true" : "false";
    result += ", \"movement_visual_lod_samples_complete\": ";
    result += evidence.movement_visual_lod_samples_complete ? "true" : "false";
    result += ", \"complete\": ";
    result += evidence.complete ? "true" : "false";
    result += " }";
    return result;
}

std::string_view presentationPollScopeName(GameBenchmarkPresentationPollScope const scope)
{
    switch (scope) {
    case GameBenchmarkPresentationPollScope::Loop:
        return "loop";
    case GameBenchmarkPresentationPollScope::Drain:
        return "drain";
    }
    throw std::logic_error("unknown serialized presentation timing poll scope");
}

std::string_view presentationPollStatusName(GameBenchmarkPresentationPollStatus const status)
{
    switch (status) {
    case GameBenchmarkPresentationPollStatus::Unavailable:
        return "unavailable";
    case GameBenchmarkPresentationPollStatus::Empty:
        return "empty";
    case GameBenchmarkPresentationPollStatus::Records:
        return "records";
    case GameBenchmarkPresentationPollStatus::Incomplete:
        return "incomplete";
    case GameBenchmarkPresentationPollStatus::Error:
        return "error";
    }
    throw std::logic_error("unknown serialized presentation timing poll status");
}

std::string presentationRequestIdJson(GameBenchmarkPresentationRequestId const& request)
{
    return "{ \"swapchain_generation\": " + std::to_string(request.swapchain_generation)
        + ", \"application_present_id\": " + std::to_string(request.application_present_id) + " }";
}

std::string presentationTimingJson(GameBenchmarkPresentationTimingEvidence const& evidence)
{
    std::string result = "{\n"
        "      \"contract\": \"mc.actual-presentation-timing.v1\",\n"
        "      \"provider\": \"VK_GOOGLE_display_timing\",\n"
        "      \"source\": \"driver-reported\",\n"
        "      \"capability\": \"" + std::string(evidence.capability_supported ? "supported" : "unavailable") + "\",\n"
        "      \"clock_domain\": \"vulkan-display-timing-monotonic-nanoseconds-unspecified-origin\",\n"
        "      \"successful_requests\": [";
    for (uint64_t index = 0U; index < evidence.successful_requests.size(); ++index) {
        GameBenchmarkSuccessfulPresentationRequest const& request = evidence.successful_requests[index];
        if (index != 0U) {
            result += ',';
        }
        result += "\n        { \"swapchain_generation\": "
            + std::to_string(request.request.swapchain_generation)
            + ", \"application_present_id\": "
            + std::to_string(request.request.application_present_id)
            + ", \"loop_ordinal\": " + std::to_string(request.loop_ordinal)
            + ", \"phase\": " + jsonString(request.phase) + " }";
    }
    result += "\n      ],\n      \"records\": [";
    for (uint64_t index = 0U; index < evidence.records.size(); ++index) {
        GameBenchmarkPresentationTimingRecord const& record = evidence.records[index];
        if (index != 0U) {
            result += ',';
        }
        result += "\n        { \"swapchain_generation\": "
            + std::to_string(record.request.swapchain_generation)
            + ", \"application_present_id\": "
            + std::to_string(record.request.application_present_id)
            + ", \"actual_present_time_ns\": "
            + std::to_string(record.actual_present_time_ns) + " }";
    }
    result += "\n      ],\n      \"polls\": [";
    for (uint64_t index = 0U; index < evidence.polls.size(); ++index) {
        GameBenchmarkPresentationPoll const& poll = evidence.polls[index];
        if (index != 0U) {
            result += ',';
        }
        result += "\n        { \"scope\": " + jsonString(presentationPollScopeName(poll.scope))
            + ", \"status\": " + jsonString(presentationPollStatusName(poll.status))
            + ", \"vk_result\": " + std::to_string(poll.vk_result)
            + ", \"returned_count\": " + std::to_string(poll.returned_count) + " }";
    }
    result += "\n      ],\n      \"capture_endpoint\": ";
    if (evidence.capture_endpoint) {
        result += "{ \"swapchain_generation\": "
            + std::to_string(evidence.capture_endpoint->request.swapchain_generation)
            + ", \"application_present_id\": "
            + std::to_string(evidence.capture_endpoint->request.application_present_id)
            + ", \"loop_ordinal\": "
            + std::to_string(evidence.capture_endpoint->loop_ordinal) + " }";
    } else {
        result += "null";
    }
    result += ",\n      \"drain\": { \"complete\": "
        + std::string(evidence.drain_complete ? "true" : "false") + ", \"request_ids\": [";
    for (uint64_t index = 0U; index < evidence.drain_request_ids.size(); ++index) {
        if (index != 0U) {
            result += ',';
        }
        result += "\n        " + presentationRequestIdJson(evidence.drain_request_ids[index]);
    }
    result += "\n      ] }\n    }";
    return result;
}

std::string startActionJson(GameBenchmarkStartActionEvidence const& evidence)
{
    return "{ \"contract\": \"mc.controlled-world-start.v1\", \"input_source\": "
        + jsonString(evidence.input_source)
        + ", \"production_path\": " + jsonString(evidence.production_path)
        + ", \"synthetic_input_override_invoked\": "
        + std::string(evidence.synthetic_input_override_invoked ? "true" : "false")
        + ", \"eligible_prompt_rendered_offset_ns\": "
        + optionalUint64Json(evidence.eligible_prompt_rendered_offset_ns)
        + ", \"synthetic_input_requested_offset_ns\": "
        + optionalUint64Json(evidence.synthetic_input_requested_offset_ns)
        + ", \"key_accepted_offset_ns\": " + optionalUint64Json(evidence.key_accepted_offset_ns)
        + ", \"server_start_acknowledged_offset_ns\": "
        + optionalUint64Json(evidence.server_start_acknowledged_offset_ns)
        + ", \"input_ordinal\": " + optionalUint64Json(evidence.input_ordinal)
        + ", \"offset_origin\": \"GameBenchmark.request_accepted_at\", \"complete\": "
        + std::string(evidence.complete ? "true" : "false") + " }";
}

std::string gameBenchmarkJson(GameBenchmarkEvidence const& benchmark)
{
    std::string result = "{\n"
        "    \"workload\": " + jsonString(benchmark.workload) + ",\n"
        "    \"qualification_proof\": " + jsonString(benchmark.qualification_proof) + ",\n"
        "    \"movement_trajectory_contract\": "
            + jsonString(benchmark.movement_trajectory_contract) + ",\n"
        "    \"terrain_backend\": " + terrainBackendReceiptJson(benchmark.terrain_backend_receipt) + ",\n"
        "    \"measurement_kind\": \"full-game-render-submit-present-request\",\n"
        "    \"package_id\": " + jsonString(benchmark.package_id) + ",\n"
        "    \"hardware\": " + jsonString(benchmark.hardware) + ",\n"
        "    \"gpu\": " + (benchmark.gpu ? jsonString(*benchmark.gpu) : "null") + ",\n"
        "    \"present_mode\": " + jsonString(benchmark.present_mode) + ",\n"
        "    \"build_mode\": " + jsonString(benchmark.build_mode) + ",\n"
        "    \"qualification_settings_manifest\": "
            + (benchmark.qualification_settings_manifest.empty() ? "null" : benchmark.qualification_settings_manifest)
            + ",\n"
        "    \"terrain_identity\": " + terrainIdentityJson(benchmark.terrain_identity) + ",\n"
        "    \"session_join_epoch\": " + std::to_string(benchmark.session_join_epoch) + ",\n"
        "    \"diagnostic_overrides_present\": "
            + (benchmark.diagnostic_overrides_present ? "true" : "false") + ",\n"
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
        "    \"frame_budget\": "
            + (benchmark.frame_budget ? frameBudgetJson(*benchmark.frame_budget) : "null") + ",\n"
        "    \"terrain_readiness\": "
            + (benchmark.terrain_readiness
                ? terrainReadinessJson(*benchmark.terrain_readiness) : "null") + ",\n"
        "    \"full_radius_terrain\": "
            + (benchmark.full_radius_terrain
                ? fullRadiusTerrainJson(*benchmark.full_radius_terrain) : "null") + ",\n"
        "    \"s1_terrain_performance\": "
            + (benchmark.s1_terrain_performance
                ? s1TerrainPerformanceJson(*benchmark.s1_terrain_performance) : "null") + ",\n"
        "    \"presentation_timing\": "
            + (benchmark.presentation_timing
                ? presentationTimingJson(*benchmark.presentation_timing) : "null") + ",\n"
        "    \"controlled_start\": "
            + (benchmark.controlled_start ? startActionJson(*benchmark.controlled_start) : "null") + ",\n"
        "    \"frame_completion_source\": \"per-slot-submitted-frame-fence-success-observed-on-reuse\",\n"
        "    \"raw_gameplay_inputs\": [";
    for (uint64_t index = 0U; index < benchmark.raw_gameplay_inputs.size(); ++index) {
        if (index != 0U) {
            result += ',';
        }
        GameBenchmarkRawInputEvidence const& input = benchmark.raw_gameplay_inputs[index];
        result += "\n      { \"input_ordinal\": " + std::to_string(input.input_ordinal)
            + ", \"sequence\": " + std::to_string(input.sequence)
            + ", \"sent_offset_ns\": " + std::to_string(input.sent_offset_ns)
            + ", \"scheduled_offset_ns\": " + std::to_string(input.scheduled_offset_ns)
            + ", \"movement_intent\": { \"strafe\": "
                + std::to_string(input.movement_intent.strafe)
            + ", \"forward\": " + std::to_string(input.movement_intent.forward)
            + ", \"vertical\": " + std::to_string(input.movement_intent.vertical) + " }"
            + ", \"camera_look\": { \"yaw_degrees\": "
                + jsonNumber(input.movement_intent.yaw_degrees)
            + ", \"pitch_degrees\": " + jsonNumber(input.movement_intent.pitch_degrees) + " }"
            + ", \"direction\": { \"x\": "
                + std::to_string(static_cast<int8_t>(input.direction.x))
            + ", \"y\": " + std::to_string(static_cast<int8_t>(input.direction.y))
            + ", \"z\": " + std::to_string(static_cast<int8_t>(input.direction.z))
            + ", \"view_x\": " + std::to_string(input.direction.view_x)
            + ", \"view_y\": " + std::to_string(input.direction.view_y)
            + ", \"accelerated\": " + (input.direction.accelerated ? "true" : "false")
            + ", \"speedup\": " + std::to_string(input.direction.speedup)
            + ", \"cycle_movement_capabilities\": "
                + (input.direction.cycle_movement_capabilities ? "true" : "false") + " } }";
    }
    result += "\n    ],\n    \"raw_server_stream_samples\": [";
    for (uint64_t index = 0U; index < benchmark.raw_server_stream_samples.size(); ++index) {
        GameBenchmarkServerStreamSample const& sample = benchmark.raw_server_stream_samples[index];
        if (index != 0U) {
            result += ',';
        }
        result += "\n      { \"started_offset_ns\": " + std::to_string(sample.started_offset_ns)
            + ", \"center\": { \"x\": " + std::to_string(sample.center_x)
            + ", \"y\": " + std::to_string(sample.center_y) + " }"
            + ", \"near_window\": { \"desired\": " + std::to_string(sample.near_desired_count)
            + ", \"resident\": " + std::to_string(sample.near_resident_count)
            + ", \"queued\": " + std::to_string(sample.near_queued_count)
            + ", \"dispatched\": " + std::to_string(sample.near_dispatched_count)
            + ", \"ready\": " + std::to_string(sample.near_ready_count)
            + ", \"inflight_additions\": " + std::to_string(sample.near_inflight_count)
            + ", \"inflight_removals\": " + std::to_string(sample.near_inflight_removals_count)
            + ", \"untracked\": " + std::to_string(sample.near_untracked_count) + " }"
            + ", \"server_queued\": " + std::to_string(sample.server_queued_count)
            + ", \"server_dispatched\": " + std::to_string(sample.server_dispatched_count)
            + ", \"server_ready\": " + std::to_string(sample.server_ready_count)
            + ", \"server_inflight_additions\": " + std::to_string(sample.server_inflight_addition_count)
            + ", \"server_inflight_removals\": " + std::to_string(sample.server_inflight_removal_count)
            + ", \"server_inflight_deliveries\": " + std::to_string(sample.server_inflight_delivery_count)
            + ", \"server_pending_removals\": " + std::to_string(sample.server_pending_removal_count)
            + ", \"server_resident\": " + std::to_string(sample.server_resident_count)
            + ", \"server_admitted_additions\": " + std::to_string(sample.server_admitted_addition_count)
            + ", \"server_rejected_delivery_acks\": "
            + std::to_string(sample.server_rejected_delivery_ack_count)
            + ", \"server_delivery_credits\": " + std::to_string(sample.server_delivery_credit_count)
            + ", \"worker_queued\": " + std::to_string(sample.worker_queued_count)
            + ", \"worker_running\": " + std::to_string(sample.worker_running_count)
            + ", \"worker_completed_uncollected\": "
                + std::to_string(sample.worker_completed_uncollected_count)
            + ", \"preview_stream_metrics\": { \"coverage_refresh\": "
                + phaseMetricsJson(sample.coverage_refresh)
            + ", \"worker_result_publication\": "
                + phaseMetricsJson(sample.worker_result_publication)
            + ", \"queue_refill\": " + phaseMetricsJson(sample.queue_refill)
            + ", \"delivery\": " + phaseMetricsJson(sample.delivery)
            + ", \"worker_dispatch\": " + phaseMetricsJson(sample.worker_dispatch)
            + ", \"materialization_candidate_keys_examined\": "
                + std::to_string(sample.materialization_candidate_keys_examined)
            + ", \"materialization_resident_chunk_lookups\": "
                + std::to_string(sample.materialization_resident_chunk_lookups)
            + ", \"materialization_admission_attempts\": "
                + std::to_string(sample.materialization_admission_attempts)
            + ", \"materialization_admissions\": "
                + std::to_string(sample.materialization_admissions)
            + ", \"materialization_duplicate_requests\": "
                + std::to_string(sample.materialization_duplicate_requests)
            + ", \"chunk_delivery_candidate_keys_examined\": "
                + std::to_string(sample.chunk_delivery_candidate_keys_examined)
            + ", \"chunk_delivery_resident_chunk_lookups\": "
                + std::to_string(sample.chunk_delivery_resident_chunk_lookups)
            + ", \"chunk_delivery_batches_sent\": "
                + std::to_string(sample.chunk_delivery_batches_sent)
            + ", \"chunk_delivery_chunks_sent\": "
                + std::to_string(sample.chunk_delivery_chunks_sent)
            + ", \"chunk_delivery_credits_consumed\": "
                + std::to_string(sample.chunk_delivery_credits_consumed)
            + ", \"world_generation_jobs_dispatched\": "
                + std::to_string(sample.world_generation_jobs_dispatched)
            + ", \"height_tile_jobs_dispatched\": "
                + std::to_string(sample.height_tile_jobs_dispatched)
            + ", \"height_tile_worker_queued\": " + std::to_string(sample.worker_queued_count)
            + ", \"height_tile_worker_running\": " + std::to_string(sample.worker_running_count)
            + ", \"height_tile_worker_completed_uncollected\": "
                + std::to_string(sample.worker_completed_uncollected_count)
            + ", \"world_generation_worker_queued\": "
                + std::to_string(sample.world_generation_worker_queued_count)
            + ", \"world_generation_worker_running\": "
                + std::to_string(sample.world_generation_worker_running_count)
            + ", \"world_generation_worker_completed_uncollected\": "
                + std::to_string(sample.world_generation_worker_completed_uncollected_count)
            + ", \"global_server_world_resident_chunks\": "
                + std::to_string(sample.global_server_world_resident_chunks)
            + ", \"required_near_chunk_keys\": " + std::to_string(sample.required_near_chunk_keys)
            + ", \"required_full_chunk_keys\": " + std::to_string(sample.required_full_chunk_keys)
            + ", \"acknowledged_chunk_keys\": " + std::to_string(sample.acknowledged_chunk_keys)
            + ", \"inflight_chunk_batches\": " + std::to_string(sample.inflight_chunk_batches)
            + ", \"inflight_chunk_keys\": " + std::to_string(sample.inflight_chunk_keys)
            + ", \"chunk_delivery_credits\": " + std::to_string(sample.chunk_delivery_credits)
            + ", \"near_required_chunks_all_resident\": "
                + (sample.near_required_chunks_all_resident ? "true" : "false") + " } }";
    }
    result += "\n    ],\n    \"raw_server_tick_durations_ns\": [";
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
            + jsonString(!sample.terrain_phase.empty() ? sample.terrain_phase
                : sample.excluded ? "excluded" : sample.uncapped ? "uncapped"
                : sample.warm ? "warm" : "cold")
            + ", \"loop_ordinal\": " + std::to_string(sample.loop_ordinal)
            + ", \"presentation_succeeded\": "
            + std::string(sample.presentation_succeeded ? "true" : "false")
            + ", \"swapchain_generation\": "
                + (sample.swapchain_generation ? std::to_string(*sample.swapchain_generation) : "null")
            + ", \"application_present_id\": "
                + (sample.application_present_id ? std::to_string(*sample.application_present_id) : "null")
            + ", \"submitted_frame_sequence_id\": "
                + (sample.submitted_frame_sequence_id
                    ? std::to_string(*sample.submitted_frame_sequence_id) : "null")
            + ", \"submitted_frame_slot\": "
                + (sample.submitted_frame_slot ? std::to_string(*sample.submitted_frame_slot) : "null")
            + ", \"submitted_frame_offset_ns\": " + std::to_string(sample.submitted_frame_offset_ns)
            + ", \"submitted_frame_phase\": " + jsonString(sample.submitted_frame_phase)
            + ", \"terrain_center\": "
            + (sample.terrain_center_valid
                ? "{ \"x\": " + std::to_string(sample.terrain_center_x) + ", \"y\": "
                    + std::to_string(sample.terrain_center_y) + " }"
                : "null")
            + ", \"playable_sphere\": "
                + (sample.playable_sphere ? playableSphereJson(*sample.playable_sphere) : "null")
            + optionalVisualLodField(sample.visual_lod)
            + ", \"qualification_settings\": " + qualificationSettingsJson(sample.qualification_settings)
            + ", \"presented_player\": " + presentedPlayerJson(sample.presented_player)
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
            + ", \"terrain_near\": "
                + (sample.terrain_near ? terrainNearSampleJson(*sample.terrain_near) : "null")
            + ", \"diagnostic_terrain_radial_bands\": "
                + (sample.diagnostic_terrain_radial_bands
                    ? terrainRadialBandsJson(*sample.diagnostic_terrain_radial_bands) : "null")
            + ", \"diagnostic_mesh_episodes\": "
                + (sample.diagnostic_mesh_episodes ? meshEpisodesJson(*sample.diagnostic_mesh_episodes) : "null")
            + ", \"diagnostic_terrain_spatial_scan_duration_ns\": "
                + std::to_string(sample.diagnostic_terrain_spatial_scan_duration_ns)
            + ", \"renderer_stone_face_growth\": "
                + (sample.renderer_stone_face_growth
                    ? stoneFaceGrowthDiagnosticsJson(*sample.renderer_stone_face_growth) : "null")
            + ", \"diagnostic_render_attempt_id\": " + std::to_string(sample.diagnostic_render_attempt_id)
            + ", \"diagnostic_frame_recorded\": " + (sample.diagnostic_frame_recorded ? "true" : "false")
            + ", \"diagnostic_indexed_stone_quads\": " + (sample.diagnostic_indexed_stone_quads ? "true" : "false")
            + ", \"diagnostic_stone_indirect\": " + (sample.diagnostic_stone_indirect ? "true" : "false")
            + ", \"diagnostic_frame_slot\": "
                + (sample.diagnostic_frame_slot ? std::to_string(*sample.diagnostic_frame_slot) : "null")
            + ", \"diagnostic_submitted_stone_quad_count\": "
                + std::to_string(sample.diagnostic_submitted_stone_quad_count)
            + ", \"diagnostic_stone_draw_count\": " + std::to_string(sample.diagnostic_stone_draw_count)
            + ", \"diagnostic_camera\": " + diagnosticCameraJson(sample.diagnostic_camera)
            + ", \"diagnostic_cpu_acquire_wait_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_acquire_wait_duration_ns)
            + ", \"diagnostic_presentation_acquire\": "
                + presentationAcquireDiagnosticsJson(sample.diagnostic_presentation_acquire)
            + ", \"diagnostic_cpu_presentation_recreation_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_presentation_recreation_duration_ns)
            + ", \"diagnostic_presentation_recreation_attempted\": "
                + (sample.diagnostic_presentation_recreation_attempted ? "true" : "false")
            + ", \"diagnostic_presentation_recreation_deferred\": "
                + (sample.diagnostic_presentation_recreation_deferred ? "true" : "false")
            + ", \"diagnostic_cpu_command_record_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_command_record_duration_ns)
            + ", \"diagnostic_cpu_visible_range_collection_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_visible_range_collection_duration_ns)
            + ", \"diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_height_tile_group_and_tile_traversal_duration_ns)
            + ", \"diagnostic_cpu_height_tile_pending_range_order_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_height_tile_pending_range_order_duration_ns)
            + ", \"diagnostic_cpu_height_tile_visible_range_merge_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_height_tile_visible_range_merge_duration_ns)
            + ", \"diagnostic_cpu_indirect_prepare_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_indirect_prepare_duration_ns)
            + ", \"diagnostic_cpu_stone_scene_record_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_stone_scene_record_duration_ns)
            + ", \"diagnostic_cpu_command_record_residual_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_command_record_residual_duration_ns)
            + ", \"diagnostic_cpu_complete_present_wait_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_complete_present_wait_duration_ns)
            + ", \"diagnostic_client_window_input_duration_ns\": "
                + std::to_string(sample.diagnostic_client_window_input_duration_ns)
            + ", \"diagnostic_client_height_tile_interest_duration_ns\": "
                + std::to_string(sample.diagnostic_client_height_tile_interest_duration_ns)
            + ", \"diagnostic_client_preview_lod_refresh_duration_ns\": "
                + std::to_string(sample.diagnostic_client_preview_lod_refresh_duration_ns)
            + ", \"diagnostic_client_preview_lod_classified_bucket_count\": "
                + std::to_string(sample.diagnostic_client_preview_lod_classified_bucket_count)
            + ", \"diagnostic_client_preview_lod_evaluated_tile_count\": "
                + std::to_string(sample.diagnostic_client_preview_lod_evaluated_tile_count)
            + ", \"diagnostic_client_preview_lod_changed_tile_count\": "
                + std::to_string(sample.diagnostic_client_preview_lod_changed_tile_count)
            + ", \"diagnostic_client_preview_mesh_pump_before_changes_duration_ns\": "
                + std::to_string(sample.diagnostic_client_preview_mesh_pump_before_changes_duration_ns)
            + ", \"diagnostic_client_height_tile_change_and_removal_duration_ns\": "
                + std::to_string(sample.diagnostic_client_height_tile_change_and_removal_duration_ns)
            + ", \"diagnostic_client_height_tile_change_queue_duration_ns\": "
                + std::to_string(sample.diagnostic_client_height_tile_change_queue_duration_ns)
            + ", \"diagnostic_client_preview_mesh_remove_duration_ns\": "
                + std::to_string(sample.diagnostic_client_preview_mesh_remove_duration_ns)
            + ", \"diagnostic_client_preview_mesh_remove_renderer_duration_ns\": "
                + std::to_string(sample.diagnostic_client_preview_mesh_remove_renderer_duration_ns)
            + ", \"diagnostic_client_preview_mesh_remove_neighbor_enqueue_duration_ns\": "
                + std::to_string(sample.diagnostic_client_preview_mesh_remove_neighbor_enqueue_duration_ns)
            + ", \"diagnostic_client_preview_mesh_remove_count\": "
                + std::to_string(sample.diagnostic_client_preview_mesh_remove_count)
            + ", \"diagnostic_renderer_height_tile_remove_prefilter_duration_ns\": "
                + std::to_string(sample.diagnostic_renderer_height_tile_remove_prefilter_duration_ns)
            + ", \"diagnostic_renderer_height_tile_remove_slot_retirement_duration_ns\": "
                + std::to_string(sample.diagnostic_renderer_height_tile_remove_slot_retirement_duration_ns)
            + ", \"diagnostic_renderer_height_tile_remove_draw_index_duration_ns\": "
                + std::to_string(sample.diagnostic_renderer_height_tile_remove_draw_index_duration_ns)
            + ", \"diagnostic_renderer_height_tile_remove_input_count\": "
                + std::to_string(sample.diagnostic_renderer_height_tile_remove_input_count)
            + ", \"diagnostic_renderer_height_tile_remove_matching_slot_count\": "
                + std::to_string(sample.diagnostic_renderer_height_tile_remove_matching_slot_count)
            + ", \"diagnostic_renderer_height_tile_remove_actual_slot_count\": "
                + std::to_string(sample.diagnostic_renderer_height_tile_remove_actual_slot_count)
            + ", \"diagnostic_client_preview_mesh_pump_after_changes_duration_ns\": "
                + std::to_string(sample.diagnostic_client_preview_mesh_pump_after_changes_duration_ns)
            + ", \"diagnostic_client_camera_resolution_duration_ns\": "
                + std::to_string(sample.diagnostic_client_camera_resolution_duration_ns)
            + ", \"diagnostic_client_render_before_resize_duration_ns\": "
                + std::to_string(sample.diagnostic_client_render_before_resize_duration_ns)
            + ", \"diagnostic_renderer_resize_duration_ns\": "
                + std::to_string(sample.diagnostic_renderer_resize_duration_ns)
            + ", \"diagnostic_client_render_after_resize_duration_ns\": "
                + std::to_string(sample.diagnostic_client_render_after_resize_duration_ns)
            + ", \"diagnostic_renderer_call_duration_ns\": "
                + std::to_string(sample.diagnostic_renderer_call_duration_ns)
            + ", \"diagnostic_client_render_after_renderer_duration_ns\": "
                + std::to_string(sample.diagnostic_client_render_after_renderer_duration_ns)
            + ", \"diagnostic_gpu_timestamps_enabled\": " + (sample.diagnostic_gpu_timestamps_enabled ? "true" : "false")
            + ", \"diagnostic_gpu_timestamp_reason\": " + jsonString(sample.diagnostic_gpu_timestamp_reason)
            + ", \"diagnostic_gpu_sample_attempt_id\": " + std::to_string(sample.diagnostic_gpu_sample_attempt_id)
            + ", \"diagnostic_gpu_sample_slot\": " + std::to_string(sample.diagnostic_gpu_sample_slot)
            + ", \"diagnostic_gpu_sample_quad_count\": " + std::to_string(sample.diagnostic_gpu_sample_quad_count)
            + ", \"diagnostic_gpu_sample_draw_count\": " + std::to_string(sample.diagnostic_gpu_sample_draw_count)
            + ", \"diagnostic_gpu_terrain_duration_ns\": "
                + (sample.diagnostic_gpu_terrain_duration_ns
                    ? std::to_string(*sample.diagnostic_gpu_terrain_duration_ns) : "null")
            + ", \"latest_gpu_frame_duration_ns\": "
            + (sample.latest_gpu_frame_duration_ns
                ? std::to_string(*sample.latest_gpu_frame_duration_ns) : "null")
            + ", \"diagnostic_terrain_occlusion_enabled\": "
            + (sample.diagnostic_terrain_occlusion_enabled ? "true" : "false")
            + ", \"diagnostic_terrain_occlusion_oracle_frame\": "
            + (sample.diagnostic_terrain_occlusion_oracle_frame ? "true" : "false")
            + ", \"diagnostic_terrain_occlusion_filtered_frame\": "
            + (sample.diagnostic_terrain_occlusion_filtered_frame ? "true" : "false")
            + ", \"diagnostic_terrain_occlusion_removed_quad_count\": "
            + std::to_string(sample.diagnostic_terrain_occlusion_removed_quad_count)
            + ", \"diagnostic_terrain_occlusion_removed_range_count\": "
            + std::to_string(sample.diagnostic_terrain_occlusion_removed_range_count)
            + ", \"diagnostic_terrain_occlusion_removed_command_count\": "
            + std::to_string(sample.diagnostic_terrain_occlusion_removed_command_count)
            + " }";
    }
    result += "\n    ],\n    \"raw_fence_confirmed_frames\": [";
    for (uint64_t index = 0U; index < benchmark.raw_fence_confirmed_frames.size(); ++index) {
        GameBenchmarkFrameCompletion const& completion = benchmark.raw_fence_confirmed_frames[index];
        if (index != 0U) {
            result += ',';
        }
        result += "\n      { \"sequence_id\": " + std::to_string(completion.sequence_id)
            + ", \"swapchain_generation\": " + std::to_string(completion.swapchain_generation)
            + ", \"frame_slot\": " + std::to_string(completion.frame_slot)
            + ", \"submitted_offset_ns\": " + std::to_string(completion.submitted_offset_ns)
            + ", \"completion_observed_offset_ns\": "
                + std::to_string(completion.completion_observed_offset_ns)
            + ", \"application_present_id\": "
                + (completion.application_present_id
                    ? std::to_string(*completion.application_present_id) : "null")
            + ", \"submission_phase\": " + jsonString(completion.submission_phase) + " }";
    }
    result += "\n    ]\n  }";
    return result;
}

} // namespace

std::string stoneFaceGrowthDiagnosticsJson(client::StoneFaceGrowthDiagnostics const& diagnostics)
{
    bool const has_known_exception = diagnostics.recreation_exception_count
        > diagnostics.recreation_unknown_exception_count;
    return "{ \"pre_growth_deadline_rejection_count\": "
            + std::to_string(diagnostics.pre_growth_deadline_rejection_count)
        + ", \"growth_attempt_count\": " + std::to_string(diagnostics.growth_attempt_count)
        + ", \"before_recreate_count\": " + std::to_string(diagnostics.before_recreate_count)
        + ", \"deferred_recreation_count\": " + std::to_string(diagnostics.deferred_recreation_count)
        + ", \"recreation_exception_count\": " + std::to_string(diagnostics.recreation_exception_count)
        + ", \"recreation_unknown_exception_count\": "
            + std::to_string(diagnostics.recreation_unknown_exception_count)
        + ", \"recreation_exception_before_hook_count\": "
            + std::to_string(diagnostics.recreation_exception_before_hook_count)
        + ", \"growth_success_count\": " + std::to_string(diagnostics.growth_success_count)
        + ", \"rollback_count\": " + std::to_string(diagnostics.rollback_count)
        + ", \"rollback_exception_count\": " + std::to_string(diagnostics.rollback_exception_count)
        + ", \"post_growth_deadline_rejection_count\": "
            + std::to_string(diagnostics.post_growth_deadline_rejection_count)
        + ", \"publication_retry_count\": " + std::to_string(diagnostics.publication_retry_count)
        + ", \"last_required_capacity\": " + std::to_string(diagnostics.last_required_capacity)
        + ", \"last_target_capacity\": " + std::to_string(diagnostics.last_target_capacity)
        + ", \"required_capacity_high_water\": " + std::to_string(diagnostics.required_capacity_high_water)
        + ", \"actual_capacity\": " + std::to_string(diagnostics.actual_capacity)
        + ", \"actual_capacity_high_water\": " + std::to_string(diagnostics.actual_capacity_high_water)
        + ", \"growth_work_duration_ns\": " + std::to_string(nanosecondsCount(diagnostics.growth_work_duration))
        + ", \"first_recreation_exception_reason\": "
            + (has_known_exception ? jsonString(diagnostics.first_recreation_exception_reason.data()) : "null")
        + ", \"last_recreation_exception_reason\": "
            + (has_known_exception ? jsonString(diagnostics.last_recreation_exception_reason.data()) : "null")
        + " }";
}

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
    // Own the input snapshot: the callback may change the caller's string.
    std::string mode, // NOLINT(performance-unnecessary-value-param)
    std::function<std::expected<RuntimeEvidence, std::string>()> const& operation
)
{
    try {
        auto result = operation();
        if (result.has_value()) {
            return std::move(*result);
        }
        // Retain mode if construction of the failure evidence throws.
        return RuntimeEvidence{
            .mode = mode,
            .scenario_version = {},
            .profile = {},
            .scenario_name = {},
            .failure = std::move(result.error()),
            .replay_id = {},
            .passed = false,
        };
    } catch (std::exception const& error) {
        return RuntimeEvidence{
            .mode = mode,
            .scenario_version = {},
            .profile = {},
            .scenario_name = {},
            .failure = error.what(),
            .replay_id = {},
            .passed = false,
        };
    } catch (...) {
        return RuntimeEvidence{
            .mode = mode,
            .scenario_version = {},
            .profile = {},
            .scenario_name = {},
            .failure = "unknown renderer exception",
            .replay_id = {},
            .passed = false,
        };
    }
}

} // namespace acceptance
