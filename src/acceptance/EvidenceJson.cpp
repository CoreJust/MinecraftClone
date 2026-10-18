#include <acceptance/EvidenceJson.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
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

std::string optionalBoolJson(std::optional<bool> const value)
{
    return value ? (*value ? "true" : "false") : "null";
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
    return "{ \"contract\": \"mc.full-radius-terrain-barriers.v1\", "
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
        + ", \"complete\": " + (evidence.complete ? "true" : "false") + " }";
}

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
        "    \"terrain_readiness\": "
            + (benchmark.terrain_readiness
                ? terrainReadinessJson(*benchmark.terrain_readiness) : "null") + ",\n"
        "    \"full_radius_terrain\": "
            + (benchmark.full_radius_terrain
                ? fullRadiusTerrainJson(*benchmark.full_radius_terrain) : "null") + ",\n"
        "    \"raw_server_stream_samples\": [";
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
                + std::to_string(sample.worker_completed_uncollected_count) + " }";
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
            + ", \"presentation_succeeded\": "
            + std::string(sample.presentation_succeeded ? "true" : "false")
            + ", \"terrain_center\": "
            + (sample.terrain_center_valid
                ? "{ \"x\": " + std::to_string(sample.terrain_center_x) + ", \"y\": "
                    + std::to_string(sample.terrain_center_y) + " }"
                : "null")
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
            + ", \"renderer_stone_face_growth\": "
                + (sample.renderer_stone_face_growth
                    ? stoneFaceGrowthDiagnosticsJson(*sample.renderer_stone_face_growth) : "null")
            + ", \"diagnostic_terrain_radial_bands\": "
                + (sample.diagnostic_terrain_radial_bands
                    ? terrainRadialBandsJson(*sample.diagnostic_terrain_radial_bands) : "null")
            + ", \"diagnostic_terrain_spatial_scan_duration_ns\": "
                + std::to_string(sample.diagnostic_terrain_spatial_scan_duration_ns)
            + ", \"diagnostic_render_attempt_id\": " + std::to_string(sample.diagnostic_render_attempt_id)
            + ", \"diagnostic_presentation_acquire\": "
                + presentationAcquireDiagnosticsJson(sample.diagnostic_presentation_acquire)
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
            + ", \"diagnostic_cpu_command_record_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_command_record_duration_ns)
            + ", \"diagnostic_cpu_visible_range_collection_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_visible_range_collection_duration_ns)
            + ", \"diagnostic_cpu_indirect_prepare_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_indirect_prepare_duration_ns)
            + ", \"diagnostic_cpu_stone_scene_record_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_stone_scene_record_duration_ns)
            + ", \"diagnostic_cpu_command_record_residual_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_command_record_residual_duration_ns)
            + ", \"diagnostic_cpu_complete_present_wait_duration_ns\": "
                + std::to_string(sample.diagnostic_cpu_complete_present_wait_duration_ns)
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
            + " }";
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
