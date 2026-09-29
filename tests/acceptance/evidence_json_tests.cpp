#include <acceptance/EvidenceJson.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace {

TEST(EvidenceJson, IncludesVersionedMetadataTimingAndCounters)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "scenario",
        .scenario_version = "1",
        .profile = "flat2d-v1",
        .scenario_name = "canonical_sample.core",
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
        .failure = "unexpected \"position\"",
    };
    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"mode\": \"scenario\\nsmoke\""), std::string::npos);
    EXPECT_NE(json.find("\"failure\": \"unexpected \\\"position\\\"\""), std::string::npos);
}

TEST(EvidenceJson, LabelsBenchmarkSamplesAsCpuPresentationRequestsAndPhases)
{
    acceptance::RuntimeEvidence const evidence{
        .mode = "benchmark-render",
        .benchmark = acceptance::RendererBenchmarkEvidence{
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
        .game_benchmark = acceptance::GameBenchmarkEvidence{
            .workload = "speed-200-movement-v1",
            .client_message_payload_bytes_sent = 128U,
            .client_message_payload_bytes_received = 256U,
            .excluded_present_requests = 1U,
            .warm_present_requests_per_second = 87.5,
            .uncapped_present_requests_per_second = 150.0,
            .raw_frames = {
                acceptance::GameBenchmarkFrameSample{
                    .warm = true,
                    .presentation_succeeded = true,
                    .loop_duration_ns = 12'000U,
                    .network_event_count = 2U,
                    .latest_gpu_frame_duration_ns = 42U,
                },
                acceptance::GameBenchmarkFrameSample{
                    .uncapped = true,
                    .presentation_succeeded = false,
                    .loop_duration_ns = 4'000U,
                },
                acceptance::GameBenchmarkFrameSample{
                    .excluded = true,
                    .presentation_succeeded = true,
                    .loop_duration_ns = 5'000U,
                },
            },
            .raw_server_tick_durations_ns = { 100U, 200U },
        },
    };

    std::string const json = acceptance::evidenceJson(evidence);

    EXPECT_NE(json.find("\"measurement_kind\": \"full-game-render-submit-present-request\""), std::string::npos);
    EXPECT_NE(json.find("\"workload\": \"speed-200-movement-v1\""), std::string::npos);
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
    EXPECT_NE(json.find("\"raw_server_tick_durations_ns\": [100, 200]"), std::string::npos);
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

} // namespace
