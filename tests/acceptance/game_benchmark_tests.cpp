#include <client/TerrainLodPresentation.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/render/StoneFaceCapacity.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/GameBenchmark.hpp>
#include <acceptance/ReplayInput.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan_core.h>

#include <array>
#include <chrono>
#include <cstddef>  // IWYU pragma: keep
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <memory>  // IWYU pragma: keep
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace {

acceptance::GameBenchmarkPresentationTimingEvidence completePresentationTimingEvidence()
{
    static constexpr uint64_t SWAPCHAIN_GENERATION = 0U;
    static constexpr uint32_t FIRST_PRESENT_ID = 1U;
    static constexpr uint32_t ENDPOINT_PRESENT_ID = 2U;
    static constexpr uint32_t DRAIN_PRESENT_ID = 3U;
    return acceptance::GameBenchmarkPresentationTimingEvidence{
        .capability_supported = true,
        .successful_requests = {
            {
                .request = {
                    .swapchain_generation = SWAPCHAIN_GENERATION,
                    .application_present_id = FIRST_PRESENT_ID,
                },
                .loop_ordinal = 1U,
                .phase = "initial-full-radius-warmup",
            },
            {
                .request = {
                    .swapchain_generation = SWAPCHAIN_GENERATION,
                    .application_present_id = ENDPOINT_PRESENT_ID,
                },
                .loop_ordinal = 2U,
                .phase = "baseline-stationary",
            },
        },
        .records = {
            {
                .request = {
                    .swapchain_generation = SWAPCHAIN_GENERATION,
                    .application_present_id = ENDPOINT_PRESENT_ID,
                },
                .actual_present_time_ns = 2'000U,
            },
            {
                .request = {
                    .swapchain_generation = SWAPCHAIN_GENERATION,
                    .application_present_id = FIRST_PRESENT_ID,
                },
                .actual_present_time_ns = 1'000U,
            },
            {
                .request = {
                    .swapchain_generation = SWAPCHAIN_GENERATION,
                    .application_present_id = DRAIN_PRESENT_ID,
                },
                .actual_present_time_ns = 3'000U,
            },
        },
        .polls = {
            {
                .scope = acceptance::GameBenchmarkPresentationPollScope::Loop,
                .status = acceptance::GameBenchmarkPresentationPollStatus::Incomplete,
                .vk_result = static_cast<int32_t>(VK_INCOMPLETE),
                .returned_count = 2U,
            },
            {
                .scope = acceptance::GameBenchmarkPresentationPollScope::Drain,
                .status = acceptance::GameBenchmarkPresentationPollStatus::Records,
                .vk_result = static_cast<int32_t>(VK_SUCCESS),
                .returned_count = 1U,
            },
        },
        .capture_endpoint = acceptance::GameBenchmarkPresentationCaptureEndpoint{
            .request = {
                .swapchain_generation = SWAPCHAIN_GENERATION,
                .application_present_id = ENDPOINT_PRESENT_ID,
            },
            .loop_ordinal = 2U,
        },
        .drain_complete = true,
        .drain_request_ids = {
            {
                .swapchain_generation = SWAPCHAIN_GENERATION,
                .application_present_id = DRAIN_PRESENT_ID,
            },
        },
    };
}

acceptance::GameBenchmarkPresentationTimingEvidence warmupGenerationPresentationTimingEvidence()
{
    static constexpr uint64_t OLD_GENERATION = 8U;
    static constexpr uint64_t MEASUREMENT_GENERATION = 9U;
    return acceptance::GameBenchmarkPresentationTimingEvidence{
        .capability_supported = true,
        .successful_requests = {
            {
                .request = { .swapchain_generation = OLD_GENERATION, .application_present_id = 1U },
                .loop_ordinal = 1U,
                .phase = "initial-full-radius-warmup",
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 1U },
                .loop_ordinal = 2U,
                .phase = "initial-full-radius-warmup",
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 2U },
                .loop_ordinal = 3U,
                .phase = "baseline-stationary",
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 3U },
                .loop_ordinal = 4U,
                .phase = "full-radius-movement",
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 4U },
                .loop_ordinal = 5U,
                .phase = "full-radius-recovery-warmup",
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 5U },
                .loop_ordinal = 6U,
                .phase = "recovered-stationary",
            },
        },
        .records = {
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 1U },
                .actual_present_time_ns = 2'000U,
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 2U },
                .actual_present_time_ns = 3'000U,
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 3U },
                .actual_present_time_ns = 4'000U,
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 4U },
                .actual_present_time_ns = 5'000U,
            },
            {
                .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 5U },
                .actual_present_time_ns = 6'000U,
            },
        },
        .polls = {
            {
                .scope = acceptance::GameBenchmarkPresentationPollScope::Loop,
                .status = acceptance::GameBenchmarkPresentationPollStatus::Incomplete,
                .vk_result = static_cast<int32_t>(VK_INCOMPLETE),
                .returned_count = 5U,
            },
            {
                .scope = acceptance::GameBenchmarkPresentationPollScope::Drain,
                .status = acceptance::GameBenchmarkPresentationPollStatus::Empty,
                .vk_result = static_cast<int32_t>(VK_SUCCESS),
                .returned_count = 0U,
            },
        },
        .capture_endpoint = acceptance::GameBenchmarkPresentationCaptureEndpoint{
            .request = { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 5U },
            .loop_ordinal = 6U,
        },
        .drain_complete = true,
        .drain_request_ids = {
            { .swapchain_generation = MEASUREMENT_GENERATION, .application_present_id = 6U },
        },
    };
}

void changeMeasuredRequestGeneration(
    acceptance::GameBenchmarkPresentationTimingEvidence& evidence,
    uint64_t const generation
)
{
    static constexpr uint64_t OLD_GENERATION = 8U;
    if (!evidence.capture_endpoint.has_value()) {
        ADD_FAILURE() << "benchmark fixture must include a capture endpoint";
        return;
    }
    for (acceptance::GameBenchmarkSuccessfulPresentationRequest& request : evidence.successful_requests) {
        if (request.phase != "initial-full-radius-warmup") {
            request.request.swapchain_generation = generation;
        }
    }
    for (acceptance::GameBenchmarkPresentationTimingRecord& record : evidence.records) {
        if (record.request.swapchain_generation != OLD_GENERATION) {
            record.request.swapchain_generation = generation;
        }
    }
    evidence.capture_endpoint->request.swapchain_generation = generation;
    for (acceptance::GameBenchmarkPresentationRequestId& request : evidence.drain_request_ids) {
        request.swapchain_generation = generation;
    }
}

TEST(GameBenchmark, AcceptsCompleteDriverHistoryWithUnorderedRecords)
{
    acceptance::GameBenchmarkPresentationTimingEvidence const evidence = completePresentationTimingEvidence();
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, AcceptsCompleteS1DriverHistoryWithMeasuredEndpointAndDrain)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.successful_requests[0U].phase = "s1-fill-ramp";
    evidence.successful_requests[1U].phase = "s1-x30-movement";
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsS1DriverHistoryWhenGenerationChangesAtMovementStart)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.successful_requests[0U].phase = "s1-fill-ramp";
    evidence.successful_requests[1U].phase = "s1-x30-movement";
    evidence.successful_requests[1U].request.swapchain_generation = 1U;
    evidence.records.front().request.swapchain_generation = 1U;
    evidence.records.back().request.swapchain_generation = 1U;
    ASSERT_TRUE(evidence.capture_endpoint.has_value());
    if (!evidence.capture_endpoint) {
        return;
    }
    evidence.capture_endpoint->request.swapchain_generation = 1U;
    evidence.drain_request_ids.front().swapchain_generation = 1U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RetiresUnresolvedFrameSubmissionAcrossSwapchainGeneration)
{
    acceptance::GameBenchmarkFrameCompletionTracker tracker;
    EXPECT_TRUE(tracker.track({
        .sequence_id = 12U,
        .swapchain_generation = 4U,
        .frame_slot = 2U,
        .submitted_offset_ns = 100U,
        .application_present_id = 8U,
        .submission_phase = "s1-x30-movement",
    }));

    EXPECT_FALSE(tracker.confirmFence(5U, 2U, 110U).has_value());
    EXPECT_TRUE(tracker.hasPendingPhase("s1-x30-movement"));
    tracker.retireOtherGenerations(5U);
    EXPECT_FALSE(tracker.hasPendingPhase("s1-x30-movement"));

    EXPECT_TRUE(tracker.track({
        .sequence_id = 13U,
        .swapchain_generation = 5U,
        .frame_slot = 2U,
        .submitted_offset_ns = 120U,
        .application_present_id = 1U,
        .submission_phase = "s1-x30-movement",
    }));
    std::optional<acceptance::GameBenchmarkFrameCompletion> const completion = tracker.confirmFence(
        5U, 2U, 130U
    );
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->sequence_id, 13U);
    EXPECT_EQ(completion->swapchain_generation, 5U);
    EXPECT_EQ(completion->application_present_id, 1U);
    EXPECT_EQ(completion->submitted_offset_ns, 120U);
    EXPECT_EQ(completion->completion_observed_offset_ns, 130U);
    EXPECT_FALSE(tracker.hasPendingPhase("s1-x30-movement"));
}

TEST(GameBenchmark, AcceptsExpectedHistoryWithoutLastDrainTimestamp)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.records.pop_back();
    evidence.polls.back().status = acceptance::GameBenchmarkPresentationPollStatus::Empty;
    evidence.polls.back().returned_count = 0U;
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsCompleteHistoryWithoutPostEndpointDrainPoll)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.polls.pop_back();
    evidence.polls.front().status = acceptance::GameBenchmarkPresentationPollStatus::Records;
    evidence.polls.front().vk_result = static_cast<int32_t>(VK_SUCCESS);
    evidence.polls.front().returned_count = static_cast<uint32_t>(evidence.records.size());
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsZeroActualPresentationTime)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.records.front().actual_present_time_ns = 0U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, AcceptsBoundedIncompletePollWhenFinalHistoryReconciles)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.polls.front().vk_result = static_cast<int32_t>(VK_SUCCESS);
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsMissingExpectedDriverHistoryRecord)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.records.erase(evidence.records.begin() + 1);
    evidence.polls.front().returned_count = 1U;
    evidence.polls.front().vk_result = static_cast<int32_t>(VK_SUCCESS);
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsUnexpectedDriverHistoryRecord)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.records.push_back({
        .request = {
            .swapchain_generation = 0U,
            .application_present_id = 4U,
        },
        .actual_present_time_ns = 4'000U,
    });
    ++evidence.polls.back().returned_count;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsHistoryError)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.polls.front().status = acceptance::GameBenchmarkPresentationPollStatus::Error;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsHistoryFromAnotherSwapchainGeneration)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.records.front().request.swapchain_generation = 8U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, AcceptsReusedPresentIdsAcrossInitialWarmupGenerations)
{
    static constexpr uint64_t OLD_GENERATION = 8U;
    static constexpr uint32_t REUSED_PRESENT_ID = 1U;
    static constexpr uint64_t OLD_GENERATION_PRESENT_TIME_NS = 1'000U;
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = warmupGenerationPresentationTimingEvidence();
    evidence.records.insert(
        evidence.records.begin(),
        acceptance::GameBenchmarkPresentationTimingRecord{
            .request = { .swapchain_generation = OLD_GENERATION, .application_present_id = REUSED_PRESENT_ID },
            .actual_present_time_ns = OLD_GENERATION_PRESENT_TIME_NS,
        }
    );
    evidence.polls.front().returned_count = 6U;
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, AllowsMissingHistoryOnlyForOldInitialWarmupGeneration)
{
    acceptance::GameBenchmarkPresentationTimingEvidence const evidence =
        warmupGenerationPresentationTimingEvidence();
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsDecreasingInitialWarmupGenerations)
{
    static constexpr uint64_t NEWER_GENERATION = 10U;
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = warmupGenerationPresentationTimingEvidence();
    evidence.successful_requests.front().request.swapchain_generation = NEWER_GENERATION;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsMissingMeasurementGenerationWarmupHistory)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = warmupGenerationPresentationTimingEvidence();
    evidence.records.erase(evidence.records.begin());
    evidence.polls.front().returned_count = 4U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsMissingMeasuredGenerationHistory)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = warmupGenerationPresentationTimingEvidence();
    evidence.records.erase(evidence.records.begin() + 1);
    evidence.polls.front().returned_count = 4U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsGenerationChangeAtBaseline)
{
    static constexpr uint64_t CHANGED_GENERATION = 10U;
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = warmupGenerationPresentationTimingEvidence();
    changeMeasuredRequestGeneration(evidence, CHANGED_GENERATION);
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsGenerationChangeDuringRecoveryWarmup)
{
    static constexpr uint64_t CHANGED_GENERATION = 10U;
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = warmupGenerationPresentationTimingEvidence();
    if (!evidence.capture_endpoint.has_value()) {
        ADD_FAILURE() << "recovery warmup fixture must include a capture endpoint";
        return;
    }
    evidence.successful_requests[4U].request.swapchain_generation = CHANGED_GENERATION;
    evidence.successful_requests[5U].request.swapchain_generation = CHANGED_GENERATION;
    evidence.records[3U].request.swapchain_generation = CHANGED_GENERATION;
    evidence.records[4U].request.swapchain_generation = CHANGED_GENERATION;
    evidence.capture_endpoint->request.swapchain_generation = CHANGED_GENERATION;
    evidence.drain_request_ids.front().swapchain_generation = CHANGED_GENERATION;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
}

TEST(GameBenchmark, RejectsZeroAndDuplicatePresentIdsWithinGeneration)
{
    acceptance::GameBenchmarkPresentationTimingEvidence zero_id = warmupGenerationPresentationTimingEvidence();
    zero_id.successful_requests[1U].request.application_present_id = 0U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(zero_id));

    acceptance::GameBenchmarkPresentationTimingEvidence duplicate_id = warmupGenerationPresentationTimingEvidence();
    duplicate_id.successful_requests[2U].request.application_present_id = 1U;
    EXPECT_FALSE(acceptance::gameBenchmarkPresentationTimingComplete(duplicate_id));
}

TEST(GameBenchmark, RejectsQualificationWithWrongRadiusBeforeStartingRuntime)
{
    for (std::string_view const proof : std::array{ "terrain-coverage", "performance" }) {
        acceptance::GameBenchmarkOptions options{};
        options.workload = acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery;
        options.qualification_proof = proof;
        options.render_distance = 7U;
        auto const result = acceptance::runGameBenchmark(options);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().find("fixed radius"), std::string::npos);
    }
}

TEST(GameBenchmark, AcceptsOrdinaryTurningPerformanceProofAtDefaultRadiusBeforeRuntimeValidation)
{
    acceptance::GameBenchmarkOptions options{
        .cold_duration = std::chrono::seconds::zero(),
        .workload = acceptance::GameBenchmarkWorkload::OrdinaryTurningMovement,
        .qualification_proof = "performance",
    };
    EXPECT_EQ(options.render_distance, 256U);

    auto const result = acceptance::runGameBenchmark(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "game benchmark has invalid duration limits");
}

TEST(GameBenchmark, DoesNotUseSpeed200CoverageWorkloadForPerformanceProof)
{
    acceptance::GameBenchmarkOptions const options{
        .workload = acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery,
        .qualification_proof = "performance",
    };
    auto const result = acceptance::runGameBenchmark(options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("fixed radius"), std::string::npos);
}

static constexpr std::array<char const*, 8U> DIAGNOSTIC_VARIABLES{
    "MC_DIAGNOSTIC_DIRECTIONAL_STONE_CULLING",
    "MC_DIAGNOSTIC_DIRECT_STONE_DRAWS",
    "MC_DIAGNOSTIC_GPU_TIMESTAMPS",
    "MC_DIAGNOSTIC_INDEXED_STONE_QUADS",
    "MC_DIAGNOSTIC_PRESENTATION_STAGES",
    "MC_DIAGNOSTIC_SPECIALIZED_STONE_SHADER",
    "MC_DIAGNOSTIC_TERRAIN_SPATIAL_SCAN",
    "MC_DIAGNOSTIC_TERRAIN_OCCLUSION",
};

std::optional<std::string> diagnosticEnvironmentValue(char const* const variable)
{
#if defined(_MSC_VER)
    char* raw_value = nullptr;
    size_t value_size = 0U;
    auto const status = ::_dupenv_s(&raw_value, &value_size, variable);
    std::unique_ptr<char, decltype(&std::free)> const value{ raw_value, &std::free };
    if (status != 0) {
        throw std::runtime_error("cannot read benchmark diagnostic environment");
    }
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{ value.get() };
#else
    char const* const value = std::getenv(variable);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{ value };
#endif
}

struct DiagnosticEnvironmentSetting final {
    char const* variable;
    char const* value;
};

class ScopedDiagnosticSetting final {
public:
    ScopedDiagnosticSetting(DiagnosticEnvironmentSetting const setting)
        : m_variable(setting.variable)
    {
        if (std::optional<std::string> const previous = diagnosticEnvironmentValue(m_variable)) {
            m_previous = *previous;
#if defined(_WIN32)
            if (m_previous->empty()) {
                throw std::runtime_error("Windows CRT cannot restore an empty diagnostic variable");
            }
#endif
        }
        if (assign(setting.value) != 0) {
            throw std::runtime_error("cannot set benchmark diagnostic environment");
        }
    }

    ~ScopedDiagnosticSetting() noexcept
    {
        try {
            EXPECT_EQ(assign(m_previous ? m_previous->c_str() : nullptr), 0);
        } catch (...) {
            std::terminate();
        }
    }
    ScopedDiagnosticSetting(ScopedDiagnosticSetting const&) = delete;
    ScopedDiagnosticSetting& operator=(ScopedDiagnosticSetting const&) = delete;

private:
    [[nodiscard]]
    int32_t assign(char const* const value) const
    {
#if defined(_WIN32)
        return _putenv_s(m_variable, value == nullptr ? "" : value);
#else
        return value == nullptr ? unsetenv(m_variable) : setenv(m_variable, value, 1);
#endif
    }

    char const* const m_variable;
    std::optional<std::string> m_previous;
};

struct ScopedCleanDiagnosticEnvironment final {
    ScopedDiagnosticSetting directional_culling{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[0U], nullptr },
    };
    ScopedDiagnosticSetting direct_draws{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[1U], nullptr },
    };
    ScopedDiagnosticSetting gpu_timestamps{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[2U], nullptr },
    };
    ScopedDiagnosticSetting indexed_quads{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[3U], nullptr },
    };
    ScopedDiagnosticSetting presentation_stages{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[4U], nullptr },
    };
    ScopedDiagnosticSetting specialized_stone_shader{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[5U], nullptr },
    };
    ScopedDiagnosticSetting terrain_spatial_scan{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[6U], nullptr },
    };
    ScopedDiagnosticSetting terrain_occlusion{
        DiagnosticEnvironmentSetting{ DIAGNOSTIC_VARIABLES[7U], nullptr },
    };
};

struct OneNearTileFixture final {
    shared::HeightTileKey center{};
    shared::HeightTileKey key{};
    bool resident = false;
    bool current_mesh_visible = false;
    uint64_t accepted_near_admissions = 0U;
    uint64_t accepted_far_admissions = 0U;
    uint64_t far_admissions_while_near_pending = 0U;
};

acceptance::GameBenchmarkTerrainObservation oneNearTile(OneNearTileFixture const fixture)
{
    acceptance::GameBenchmarkTerrainObservation observation{};
    observation.center = fixture.center;
    observation.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = fixture.key,
        .desired = true,
        .resident = fixture.resident,
        .current_mesh_visible = fixture.current_mesh_visible,
        .pending = fixture.resident && !fixture.current_mesh_visible,
    };
    observation.desired_count = 1U;
    observation.resident_count = fixture.resident ? 1U : 0U;
    observation.nonresident_count = fixture.resident ? 0U : 1U;
    observation.current_mesh_visible_count = fixture.current_mesh_visible ? 1U : 0U;
    observation.pending_count = fixture.resident && !fixture.current_mesh_visible ? 1U : 0U;
    observation.accepted_near_admissions = fixture.accepted_near_admissions;
    observation.accepted_far_admissions = fixture.accepted_far_admissions;
    observation.far_admissions_while_near_pending = fixture.far_admissions_while_near_pending;
    return observation;
}

acceptance::GameBenchmarkTerrainObservation fullNearWindow(
    shared::HeightTileKey const center,
    uint32_t const current_mesh_visible_count
)
{
    acceptance::GameBenchmarkTerrainObservation observation{};
    observation.center = center;
    observation.desired_count = acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT;
    observation.current_mesh_visible_count = current_mesh_visible_count;
    observation.resident_count = current_mesh_visible_count;
    observation.nonresident_count = acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT
        - current_mesh_visible_count;
    uint32_t index = 0U;
    for (int32_t y = -7; y <= 7; ++y) {
        for (int32_t x = -7; x <= 7; ++x) {
            bool const visible = index < current_mesh_visible_count;
            observation.near_tiles[index] = acceptance::GameBenchmarkTerrainTileObservation{
                .key = { center.x + x, center.y + y },
                .desired = true,
                .resident = visible,
                .current_mesh_visible = visible,
            };
            ++index;
        }
    }
    return observation;
}

} // namespace

TEST(GameBenchmark, ReportsNoDiagnosticOverridesWhenKnownVariablesAreAbsent)
{
    ScopedCleanDiagnosticEnvironment const environment;
    ScopedDiagnosticSetting const unrelated{
        DiagnosticEnvironmentSetting{ "MC_DIAGNOSTIC_UNIMPLEMENTED_SETTING", "1" },
    };
    EXPECT_FALSE(acceptance::gameBenchmarkDiagnosticOverridesPresent());
    EXPECT_TRUE(acceptance::gameBenchmarkDiagnosticOverridesPresent(true));
}

TEST(GameBenchmark, ReportsEachKnownDiagnosticOverride)
{
    ScopedCleanDiagnosticEnvironment const environment;
    for (char const* const variable : DIAGNOSTIC_VARIABLES) {
        {
            ScopedDiagnosticSetting const setting{ DiagnosticEnvironmentSetting{ variable, "1" } };
            EXPECT_TRUE(acceptance::gameBenchmarkDiagnosticOverridesPresent()) << variable;
        }
        EXPECT_FALSE(acceptance::gameBenchmarkDiagnosticOverridesPresent()) << variable;
    }
}

TEST(GameBenchmark, CountsZeroValuesAsDiagnosticOverrides)
{
    ScopedCleanDiagnosticEnvironment const environment;
    for (char const* const variable : DIAGNOSTIC_VARIABLES) {
        ScopedDiagnosticSetting const setting{ DiagnosticEnvironmentSetting{ variable, "0" } };
        EXPECT_TRUE(acceptance::gameBenchmarkDiagnosticOverridesPresent()) << variable;
    }
}

#if !defined(_WIN32)
TEST(GameBenchmark, CountsEmptyValuesAsDiagnosticOverrides)
{
    ScopedCleanDiagnosticEnvironment const environment;
    for (char const* const variable : DIAGNOSTIC_VARIABLES) {
        ScopedDiagnosticSetting const setting{ DiagnosticEnvironmentSetting{ variable, "" } };
        EXPECT_TRUE(acceptance::gameBenchmarkDiagnosticOverridesPresent()) << variable;
    }
}
#endif

TEST(GameBenchmark, DefaultsTo256AndRejectsUnsupportedRenderDistancesBeforeLaunch)
{
    static constexpr std::array<uint32_t, 3> INVALID_RADII{0U, 257U, 1'024U};
    EXPECT_EQ(acceptance::GameBenchmarkOptions{}.render_distance, 256U);
    for (uint32_t const radius : INVALID_RADII) {
        auto const result = acceptance::runGameBenchmark({
            .render_distance = radius,
            .qualification_proof = {},
        });
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), "render distance must be an integer chunk radius from 1 to 256");
    }
}

TEST(GameBenchmark, ClassifiesCompletedRequestsWithinHalfOpenIntervals)
{
    acceptance::GameBenchmarkOptions const options{};
    auto const first = std::chrono::steady_clock::time_point{};
    using acceptance::GameBenchmarkPhase;
    using acceptance::gameBenchmarkPhaseAt;

    EXPECT_EQ(gameBenchmarkPhaseAt(first - 1ns, first, options), GameBenchmarkPhase::Outside);
    EXPECT_EQ(gameBenchmarkPhaseAt(first, first, options), GameBenchmarkPhase::Cold);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 2s - 1ns, first, options), GameBenchmarkPhase::Cold);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 2s, first, options), GameBenchmarkPhase::Warm);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 5s - 1ns, first, options), GameBenchmarkPhase::Warm);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 5s, first, options), GameBenchmarkPhase::Uncapped);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s - 1ns, first, options), GameBenchmarkPhase::Uncapped);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s, first, options), GameBenchmarkPhase::Outside);
    EXPECT_EQ(gameBenchmarkPhaseAt(first + 6s + 1ns, first, options), GameBenchmarkPhase::Outside);
}

TEST(GameBenchmark, KeepsDiagnosticStationaryInputsMotionlessAcrossScriptBoundaries)
{
    using acceptance::GameBenchmarkWorkload;
    for (uint64_t const ordinal : { 0U, 19U, 20U, 39U, 40U, 100'000U }) {
        shared::Direction const direction = acceptance::gameBenchmarkDirection(
            GameBenchmarkWorkload::DiagnosticStationary, ordinal
        );
        EXPECT_EQ(direction.x, 0U);
        EXPECT_EQ(direction.y, 0U);
        EXPECT_EQ(direction.z, 0U);
        EXPECT_FALSE(direction.accelerated);
        EXPECT_FALSE(direction.cycle_movement_capabilities);
    }
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::DiagnosticStationary),
        "diagnostic-stationary-v1");
}

TEST(GameBenchmark, BoundsFullRadiusWarmupTimeoutAndWatchdogBudget)
{
    EXPECT_EQ(acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE, 30s);
    EXPECT_EQ(acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT, 30s);
    EXPECT_EQ(acceptance::GAME_BENCHMARK_FULL_RADIUS_NON_WARMUP_BUDGET, 120s);
    EXPECT_EQ(*acceptance::gameBenchmarkFullRadiusWarmupTimeout(1), 1s);
    EXPECT_EQ(*acceptance::gameBenchmarkFullRadiusWarmupTimeout(180), 180s);
    EXPECT_EQ(*acceptance::gameBenchmarkFullRadiusWarmupTimeout(1'800), 1'800s);
    for (int64_t const seconds : { -1, 0, 1'801 }) {
        EXPECT_FALSE(acceptance::gameBenchmarkFullRadiusWarmupTimeout(seconds));
    }

    acceptance::GameBenchmarkOptions options{
        .workload = acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery,
        .qualification_proof = {},
    };
    EXPECT_EQ(acceptance::gameBenchmarkDeadline(options), 480s);
    options.workload = acceptance::GameBenchmarkWorkload::OrdinaryTurningMovement;
    EXPECT_EQ(acceptance::gameBenchmarkDeadline(options), options.deadline);
    options.workload = acceptance::GameBenchmarkWorkload::Speed200Movement;
    EXPECT_EQ(acceptance::gameBenchmarkDeadline(options), options.deadline);
}

TEST(GameBenchmark, SuccessfulRequestIntervalsIncludeFailedAttemptsAndStayWithinTheirStationaryPhase)
{
    static constexpr uint64_t MILLISECOND_NS = 1'000'000U;
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    frames.reserve(6U);
    acceptance::GameBenchmarkFrameSample& first_baseline_frame = frames.emplace_back();
    first_baseline_frame.presentation_succeeded = true;
    first_baseline_frame.started_offset_ns = 0U;
    first_baseline_frame.loop_duration_ns = 2U * MILLISECOND_NS;
    first_baseline_frame.terrain_phase = "baseline-stationary";

    acceptance::GameBenchmarkFrameSample& failed_baseline_frame = frames.emplace_back();
    failed_baseline_frame.presentation_succeeded = false;
    failed_baseline_frame.started_offset_ns = 2U * MILLISECOND_NS;
    failed_baseline_frame.loop_duration_ns = 8U * MILLISECOND_NS;
    failed_baseline_frame.terrain_phase = "baseline-stationary";

    acceptance::GameBenchmarkFrameSample& second_baseline_frame = frames.emplace_back();
    second_baseline_frame.presentation_succeeded = true;
    second_baseline_frame.started_offset_ns = 10U * MILLISECOND_NS;
    second_baseline_frame.loop_duration_ns = 2U * MILLISECOND_NS;
    second_baseline_frame.terrain_phase = "baseline-stationary";

    acceptance::GameBenchmarkFrameSample& movement_frame = frames.emplace_back();
    movement_frame.presentation_succeeded = true;
    movement_frame.started_offset_ns = 100U * MILLISECOND_NS;
    movement_frame.loop_duration_ns = 2U * MILLISECOND_NS;
    movement_frame.terrain_phase = "full-radius-movement";

    acceptance::GameBenchmarkFrameSample& first_recovered_frame = frames.emplace_back();
    first_recovered_frame.presentation_succeeded = true;
    first_recovered_frame.started_offset_ns = 1'000U * MILLISECOND_NS;
    first_recovered_frame.loop_duration_ns = 2U * MILLISECOND_NS;
    first_recovered_frame.terrain_phase = "recovered-stationary";

    acceptance::GameBenchmarkFrameSample& second_recovered_frame = frames.emplace_back();
    second_recovered_frame.presentation_succeeded = true;
    second_recovered_frame.started_offset_ns = 1'020U * MILLISECOND_NS;
    second_recovered_frame.loop_duration_ns = 2U * MILLISECOND_NS;
    second_recovered_frame.terrain_phase = "recovered-stationary";

    auto const baseline = acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "baseline-stationary");
    ASSERT_EQ(baseline.size(), 1U);
    EXPECT_EQ(baseline.front(), 10ms);
    auto const recovered = acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "recovered-stationary");
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front(), 20ms);
    EXPECT_TRUE(acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "full-radius-movement").empty());
}

TEST(GameBenchmark, FrameBudgetUsesNinetyNinePercentOfFreshCompletionTime)
{
    static constexpr uint64_t TARGET_FRAME_DURATION_NS = 1'000'000U;
    static constexpr uint64_t REQUIRED_WINDOW_DURATION_NS = 1'000'000'000U;
    static constexpr uint32_t TOTAL_WINDOW_MILLISECONDS = 1'000U;
    auto const measure = [](uint32_t const late_interval_count) {
        std::vector<acceptance::GameBenchmarkFrameSample> frames;
        std::vector<acceptance::GameBenchmarkFrameCompletion> completions;
        uint32_t const normal_interval_count = TOTAL_WINDOW_MILLISECONDS - late_interval_count * 10U;
        uint64_t offset_ns = 0U;
        uint32_t ordinal = 0U;
        for (uint32_t interval_index = 0U;
             interval_index < normal_interval_count + late_interval_count;
             ++interval_index) {
            uint64_t const duration_ns = interval_index < normal_interval_count
                ? TARGET_FRAME_DURATION_NS
                : 10U * TARGET_FRAME_DURATION_NS;
            uint32_t const frame_slot = ordinal % 3U;
            ++ordinal;
            offset_ns += duration_ns;
            frames.push_back({
                .loop_ordinal = ordinal,
                .presentation_succeeded = true,
                .swapchain_generation = 1U,
                .submitted_frame_sequence_id = ordinal,
                .submitted_frame_slot = frame_slot,
                .submitted_frame_offset_ns = offset_ns,
                .submitted_frame_phase = "full-radius-movement",
                .started_offset_ns = offset_ns - duration_ns,
                .loop_duration_ns = duration_ns,
                .terrain_phase = "full-radius-movement",
            });
            completions.push_back({
                .sequence_id = ordinal,
                .swapchain_generation = 1U,
                .frame_slot = frame_slot,
                .submitted_offset_ns = offset_ns,
                .completion_observed_offset_ns = offset_ns,
                .submission_phase = "full-radius-movement",
            });
        }
        acceptance::GameBenchmarkFrameBudgetWindow const window{
            .start_offset_ns = 0U,
            .end_offset_ns = REQUIRED_WINDOW_DURATION_NS,
            .required_duration_ns = REQUIRED_WINDOW_DURATION_NS,
            .phase = "full-radius-movement",
            .complete = true,
        };
        return acceptance::gameBenchmarkFrameBudget(
            frames, completions, TARGET_FRAME_DURATION_NS, window
        );
    };

    acceptance::GameBenchmarkFrameBudgetEvidence const exactly_ninety_nine_percent = measure(1U);
    EXPECT_TRUE(exactly_ninety_nine_percent.passed);
    EXPECT_EQ(exactly_ninety_nine_percent.required_in_budget_time_permille, 990U);
    EXPECT_EQ(exactly_ninety_nine_percent.in_budget_time_ns, 990'000'000U);
    EXPECT_FALSE(measure(2U).passed);
}

TEST(GameBenchmark, FrameBudgetLimitsOverBudgetTimeInsteadOfOverBudgetFrameCount)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    std::vector<acceptance::GameBenchmarkFrameCompletion> completions;
    frames.reserve(1'001U);
    completions.reserve(1'000U);
    uint64_t offset_ns = 0U;
    for (uint32_t index = 0U; index < 999U; ++index) {
        frames.push_back({
            .loop_ordinal = static_cast<uint64_t>(index) + 1U,
            .presentation_succeeded = true,
            .swapchain_generation = 1U,
            .application_present_id = index + 1U,
            .submitted_frame_sequence_id = static_cast<uint64_t>(index) + 1U,
            .submitted_frame_slot = index % 3U,
            .submitted_frame_offset_ns = offset_ns + 1'000'000U,
            .submitted_frame_phase = "full-radius-movement",
            .started_offset_ns = offset_ns,
            .loop_duration_ns = 1'000'000U,
            .terrain_phase = "full-radius-movement",
        });
        completions.push_back({
            .sequence_id = static_cast<uint64_t>(index) + 1U,
            .swapchain_generation = 1U,
            .frame_slot = index % 3U,
            .submitted_offset_ns = offset_ns + 1'000'000U,
            .completion_observed_offset_ns = offset_ns + 1'000'000U,
            .application_present_id = index + 1U,
            .submission_phase = "full-radius-movement",
        });
        offset_ns += 1'000'000U;
    }
    frames.push_back({
        .loop_ordinal = 1'000U,
        .presentation_succeeded = true,
        .swapchain_generation = 1U,
        .application_present_id = 1'000U,
        .submitted_frame_sequence_id = 1'000U,
        .submitted_frame_slot = 0U,
        .submitted_frame_offset_ns = offset_ns + 1'000'000U,
        .submitted_frame_phase = "full-radius-movement",
        .started_offset_ns = offset_ns,
        .loop_duration_ns = 1'000'000U,
        .terrain_phase = "full-radius-movement",
    });
    completions.push_back({
        .sequence_id = 1'000U,
        .swapchain_generation = 1U,
        .frame_slot = 0U,
        .submitted_offset_ns = offset_ns + 1'000'000U,
        .completion_observed_offset_ns = offset_ns + 2'000'000U,
        .application_present_id = 1'000U,
        .submission_phase = "full-radius-movement",
    });
    frames.push_back({
        .excluded = true,
        .submitted_frame_phase = {},
        .loop_duration_ns = 60'000'000'000U,
    });

    acceptance::GameBenchmarkFrameBudgetWindow window{
        .start_offset_ns = 0U,
        .end_offset_ns = 1'002'000'000U,
        .required_duration_ns = 1'000'000'000U,
        .phase = "full-radius-movement",
        .complete = true,
    };
    acceptance::GameBenchmarkFrameBudgetEvidence const passing = acceptance::gameBenchmarkFrameBudget(
        frames, completions, 1'000'000U, window
    );
    EXPECT_TRUE(passing.passed);
    EXPECT_EQ(passing.sample_count, 1'001U);
    EXPECT_EQ(passing.fence_confirmed_frame_count, 1'000U);
    EXPECT_EQ(passing.measured_time_ns, 1'002'000'000U);
    EXPECT_EQ(passing.in_budget_time_ns, 1'000'000'000U);

    completions[999U].completion_observed_offset_ns = offset_ns + 13'000'000U;
    window.end_offset_ns = 1'013'000'000U;
    acceptance::GameBenchmarkFrameBudgetEvidence const failing = acceptance::gameBenchmarkFrameBudget(
        frames, completions, 1'000'000U, window
    );
    EXPECT_FALSE(failing.passed);
    EXPECT_EQ(failing.measured_time_ns, 1'013'000'000U);
    EXPECT_EQ(failing.in_budget_time_ns, 1'000'000'000U);
}

TEST(GameBenchmark, FrameBudgetRejectsSparseFenceCompletionsDespiteFastPresentCadence)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    frames.reserve(5U);
    for (uint32_t index = 0U; index < 5U; ++index) {
        frames.push_back({
            .loop_ordinal = static_cast<uint64_t>(index) + 1U,
            .presentation_succeeded = true,
            .swapchain_generation = 1U,
            .application_present_id = index + 1U,
            .submitted_frame_sequence_id = static_cast<uint64_t>(index) + 1U,
            .submitted_frame_slot = index % 3U,
            .submitted_frame_offset_ns = (static_cast<uint64_t>(index) + 1U) * 8'000'000U,
            .submitted_frame_phase = "full-radius-movement",
            .started_offset_ns = static_cast<uint64_t>(index) * 8'000'000U,
            .loop_duration_ns = 8'000'000U,
            .terrain_phase = "full-radius-movement",
        });
    }
    std::vector<acceptance::GameBenchmarkFrameCompletion> completions{
        {
            .sequence_id = 1U,
            .swapchain_generation = 1U,
            .frame_slot = 0U,
            .submitted_offset_ns = 8'000'000U,
            .completion_observed_offset_ns = 24'000'000U,
            .application_present_id = 1U,
            .submission_phase = "full-radius-movement",
        },
        {
            .sequence_id = 2U,
            .swapchain_generation = 1U,
            .frame_slot = 1U,
            .submitted_offset_ns = 16'000'000U,
            .completion_observed_offset_ns = 32'000'000U,
            .application_present_id = 2U,
            .submission_phase = "full-radius-movement",
        },
    };

    acceptance::GameBenchmarkFrameBudgetWindow const window{
        .start_offset_ns = 0U,
        .end_offset_ns = 40'000'000U,
        .required_duration_ns = 40'000'000U,
        .phase = "full-radius-movement",
        .complete = true,
    };
    acceptance::GameBenchmarkFrameBudgetEvidence const evidence = acceptance::gameBenchmarkFrameBudget(
        frames, completions, 8'000'000U, window
    );

    EXPECT_FALSE(evidence.passed);
    EXPECT_EQ(evidence.measured_time_ns, 40'000'000U);
    EXPECT_EQ(evidence.fence_confirmed_frame_count, 2U);
    EXPECT_EQ(evidence.sample_count, 3U);
}

TEST(GameBenchmark, FrameBudgetAllowsFenceDrainTailWithoutExtendingMeasurementWindow)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    std::vector<acceptance::GameBenchmarkFrameCompletion> completions;
    for (uint32_t index = 0U; index < 3U; ++index) {
        uint64_t const sequence = static_cast<uint64_t>(index) + 1U;
        uint64_t const submitted_offset = index < 2U
            ? sequence * 8'000'000U - 2'000'000U
            : 23'000'000U;
        uint64_t const completion_observed_offset = index < 2U
            ? sequence * 8'000'000U
            : 28'000'000U;
        frames.push_back({
            .loop_ordinal = sequence,
            .presentation_succeeded = true,
            .swapchain_generation = 1U,
            .application_present_id = static_cast<uint32_t>(sequence),
            .submitted_frame_sequence_id = sequence,
            .submitted_frame_slot = index,
            .submitted_frame_offset_ns = submitted_offset,
            .submitted_frame_phase = "s1-x30-movement",
            .started_offset_ns = submitted_offset - 4'000'000U,
            .loop_duration_ns = 4'000'000U,
            .terrain_phase = "s1-x30-movement",
        });
        completions.push_back({
            .sequence_id = sequence,
            .swapchain_generation = 1U,
            .frame_slot = index,
            .submitted_offset_ns = submitted_offset,
            .completion_observed_offset_ns = completion_observed_offset,
            .application_present_id = static_cast<uint32_t>(sequence),
            .submission_phase = "s1-x30-movement",
        });
    }
    acceptance::GameBenchmarkFrameBudgetWindow const window{
        .start_offset_ns = 0U,
        .end_offset_ns = 24'000'000U,
        .required_duration_ns = 24'000'000U,
        .phase = "s1-x30-movement",
        .complete = true,
        .completion_observation_tail_ns = 10'000'000U,
    };

    acceptance::GameBenchmarkFrameBudgetEvidence const within_tail = acceptance::gameBenchmarkFrameBudget(
        frames, completions, 8'000'000U, window
    );
    EXPECT_TRUE(within_tail.passed);
    EXPECT_TRUE(within_tail.measurement_window_complete);
    EXPECT_EQ(within_tail.measured_time_ns, 24'000'000U);
    EXPECT_EQ(within_tail.in_budget_time_ns, 24'000'000U);
    EXPECT_EQ(within_tail.fence_confirmed_frame_count, 3U);

    completions.back().completion_observed_offset_ns = 35'000'000U;
    acceptance::GameBenchmarkFrameBudgetEvidence const beyond_tail = acceptance::gameBenchmarkFrameBudget(
        frames, completions, 8'000'000U, window
    );
    EXPECT_FALSE(beyond_tail.passed);
    EXPECT_FALSE(beyond_tail.measurement_window_complete);
    EXPECT_EQ(beyond_tail.measured_time_ns, 24'000'000U);
    EXPECT_EQ(beyond_tail.fence_confirmed_frame_count, 2U);
}

TEST(GameBenchmark, FrameBudgetRejectsMissingOrDuplicateFenceConfirmedCompletionIds)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames{
        {
            .loop_ordinal = 1U,
            .presentation_succeeded = true,
            .swapchain_generation = 1U,
            .application_present_id = 1U,
            .submitted_frame_sequence_id = 1U,
            .submitted_frame_slot = 0U,
            .submitted_frame_offset_ns = 8'000'000U,
            .submitted_frame_phase = "full-radius-movement",
            .started_offset_ns = 0U,
            .loop_duration_ns = 8'000'000U,
            .terrain_phase = "full-radius-movement",
        },
        {
            .loop_ordinal = 2U,
            .presentation_succeeded = true,
            .swapchain_generation = 1U,
            .application_present_id = 2U,
            .submitted_frame_sequence_id = 2U,
            .submitted_frame_slot = 1U,
            .submitted_frame_offset_ns = 16'000'000U,
            .submitted_frame_phase = "full-radius-movement",
            .started_offset_ns = 8'000'000U,
            .loop_duration_ns = 8'000'000U,
            .terrain_phase = "full-radius-movement",
        },
    };
    std::vector<acceptance::GameBenchmarkFrameCompletion> completions{
        {
            .sequence_id = 1U,
            .swapchain_generation = 1U,
            .frame_slot = 0U,
            .submitted_offset_ns = 8'000'000U,
            .completion_observed_offset_ns = 8'000'000U,
            .application_present_id = 1U,
            .submission_phase = "full-radius-movement",
        },
        {
            .sequence_id = 2U,
            .swapchain_generation = 1U,
            .frame_slot = 1U,
            .submitted_offset_ns = 16'000'000U,
            .completion_observed_offset_ns = 16'000'000U,
            .application_present_id = 2U,
            .submission_phase = "full-radius-movement",
        },
    };
    acceptance::GameBenchmarkFrameBudgetWindow const window{
        .start_offset_ns = 0U,
        .end_offset_ns = 16'000'000U,
        .required_duration_ns = 16'000'000U,
        .phase = "full-radius-movement",
        .complete = true,
    };

    frames.front().application_present_id = std::nullopt;
    frames.front().submitted_frame_sequence_id = std::nullopt;
    EXPECT_FALSE(acceptance::gameBenchmarkFrameBudget(frames, completions, 8'000'000U, window).passed);
    frames.front().application_present_id = 1U;
    frames.front().submitted_frame_sequence_id = 1U;
    completions.back().sequence_id = 1U;
    EXPECT_FALSE(acceptance::gameBenchmarkFrameBudget(frames, completions, 8'000'000U, window).passed);
}

TEST(GameBenchmark, RejectsInvalidRenderDistanceBeforeRuntimeStarts)
{
    acceptance::GameBenchmarkOptions const options{
        .workload = acceptance::GameBenchmarkWorkload::Speed200FullRadiusRecovery,
        .render_distance = 257U,
        .qualification_proof = {},
    };
    auto const result = acceptance::runGameBenchmark(options);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "render distance must be an integer chunk radius from 1 to 256");
}

TEST(GameBenchmark, FullRadiusSequenceRequiresBothExactBarriersAndStationarySamples)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    using acceptance::GameBenchmarkFullRadiusSequence;

    Clock::time_point const start{};
    GameBenchmarkFullRadiusSequence sequence{ 30s };
    sequence.start(start);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 10s, std::nullopt, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 12s, false, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 14s, true, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 15s, true, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 45s - 1ns, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 45s, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(start + 75s - 1ns, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(start + 75s, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 103s, false, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 104s, true, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 104s + 500ms, true, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(start + 134s + 500ms - 1ns, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(start + 134s + 500ms, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Complete);
}

TEST(GameBenchmark, X30MovementWindowStartsWithFirstSuccessfulInput)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    using acceptance::GameBenchmarkFullRadiusSequence;

    Clock::time_point const start{};
    Clock::time_point const first_input_at = start + 42s;
    GameBenchmarkFullRadiusSequence sequence{ 30s, true };
    sequence.start(start);
    sequence.observe(start, true, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 30s, std::nullopt, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);

    sequence.observe(first_input_at - 1ns, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    EXPECT_EQ(sequence.phaseElapsed(first_input_at - 1ns), Clock::duration::zero());
    sequence.beginMovementWindow(first_input_at);
    EXPECT_EQ(sequence.phaseElapsed(first_input_at), Clock::duration::zero());
    sequence.observe(first_input_at + 30s - 1ns, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(first_input_at + 30s, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
}

TEST(GameBenchmark, S1MeasurementWindowStartsOnlyAfterReadySuccessfulInputAndStaysFixed)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point const request_accepted_at{};
    acceptance::GameBenchmarkS1MovementWindow window;

    EXPECT_FALSE(window.observeSuccessfulInput(request_accepted_at + 10s, false));
    EXPECT_FALSE(window.startedAt().has_value());
    EXPECT_TRUE(window.observeSuccessfulInput(request_accepted_at + 20s, true));
    EXPECT_EQ(window.startedAt(), request_accepted_at + 20s);
    EXPECT_FALSE(window.observeSuccessfulInput(request_accepted_at + 25s, true));
    EXPECT_TRUE(window.contains(request_accepted_at + 20s));
    EXPECT_TRUE(window.contains(request_accepted_at + 50s - 1ns));
    EXPECT_FALSE(window.contains(request_accepted_at + 50s));
    EXPECT_FALSE(window.contains(request_accepted_at + 19s));
}

TEST(GameBenchmark, S1MovementReceiptsRetainSchedulerBoundaryInputAndAcknowledgementTail)
{
    static constexpr uint64_t START_NS = 91'316'542U;
    static constexpr int64_t SCHEDULED_START_NS = 90'931'417;
    static constexpr uint64_t TICK_NS = 100'000'000U;
    static constexpr uint32_t INPUT_COUNT = 301U;
    acceptance::GameBenchmarkEvidence benchmark;
    benchmark.s1_terrain_performance.emplace();
    benchmark.frame_budget.emplace();
    benchmark.frame_budget->measurement_window_start_offset_ns = START_NS;
    benchmark.frame_budget->measurement_window_end_offset_ns = START_NS + 30'000'000'000U;
    benchmark.frame_budget->required_window_duration_ns = 30'000'000'000U;
    auto& evidence = *benchmark.s1_terrain_performance;
    for (uint32_t index = 0U; index < INPUT_COUNT; ++index) {
        int64_t const scheduled = SCHEDULED_START_NS + static_cast<int64_t>(index * TICK_NS);
        uint64_t const sent = index == 0U ? START_NS : static_cast<uint64_t>(scheduled);
        benchmark.raw_gameplay_inputs.push_back({
            .sequence = index + 1U,
            .sent_offset_ns = sent,
            .scheduled_offset_ns = scheduled,
        });
        evidence.movement_authoritative_sphere_updates.push_back({
            .offset_ns = sent + 10'000'000U,
            .acknowledged_input_sequence = index + 1U,
            .playable_sphere = {},
        });
    }
    evidence.movement_authoritative_update_count = INPUT_COUNT;
    EXPECT_TRUE(acceptance::gameBenchmarkS1MovementReceiptsComplete(benchmark));
    EXPECT_GT(evidence.movement_authoritative_sphere_updates.back().offset_ns,
        benchmark.frame_budget->measurement_window_end_offset_ns);
    evidence.movement_authoritative_sphere_updates.erase(
        evidence.movement_authoritative_sphere_updates.begin() + 150
    );
    --evidence.movement_authoritative_update_count;
    EXPECT_FALSE(acceptance::gameBenchmarkS1MovementReceiptsComplete(benchmark));
}

TEST(GameBenchmark, GenericSphereCountsExactAuthoritativeVolumeWithoutRelabelingFullProfiles)
{
    shared::PlayerPosition const center{ .x = 128, .y = 128, .z = 128 };
    shared::SparseWorld world;
    acceptance::GameBenchmarkFrameSample::PlayableSphere sample{
        .radius_blocks = 16U,
        .active_coverage_epoch = std::nullopt,
        .active_coverage_center = std::nullopt,
        .world_loading_center = center,
        .coverage_scope = {},
        .authoritative_columns_center = std::nullopt,
    };
    auto const keys = shared::requiredChunkKeys(center, sample.radius_blocks);
    ASSERT_FALSE(keys.empty());
    acceptance::gameBenchmarkObserveGeometricSphere(sample, world);
    EXPECT_EQ(sample.required_chunks, keys.size());
    EXPECT_EQ(sample.ready_chunks, 0U);
    EXPECT_FALSE(sample.ready);
    for (auto const key : keys) {
        ASSERT_NE(world.ensureChunk(key), nullptr);
    }
    acceptance::gameBenchmarkObserveGeometricSphere(sample, world);
    EXPECT_EQ(sample.ready_chunks, sample.required_chunks);
    EXPECT_TRUE(sample.ready);
    ASSERT_TRUE(world.evict(keys.back()));
    acceptance::gameBenchmarkObserveGeometricSphere(sample, world);
    EXPECT_FALSE(sample.ready);
}

TEST(GameBenchmark, S1CurrentTerrainReadyRequiresHorizontalFullVolumeProfiles)
{
    shared::PlayerPosition const center{ .x = 12, .y = 13, .z = 14 };
    acceptance::GameBenchmarkFrameSample::PlayableSphere sample{
        .radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
        .active_coverage_epoch = 9U,
        .active_coverage_center = center,
        .world_loading_center = center,
        .ready_chunks = 419'904U,
        .required_chunks = 419'904U,
        .ready = true,
        .coverage_scope = "horizontal_full_volume",
        .required_authoritative_columns = 6'561U,
        .resident_authoritative_columns = 6'561U,
        .vertical_chunk_count = 64U,
        .authoritative_columns_center = center,
    };

    EXPECT_TRUE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    sample.radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS;
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    sample.radius_blocks = 40U;
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    sample.radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    sample.coverage_scope = "materialized_sphere";
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    sample.coverage_scope = "horizontal_full_volume";
    --sample.resident_authoritative_columns;
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    ++sample.resident_authoritative_columns;
    sample.active_coverage_epoch = 0U;
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    sample.active_coverage_epoch = 9U;
    sample.authoritative_columns_center->x += 16;
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
    sample.authoritative_columns_center = center;
    sample.ready_chunks -= 1U;
    EXPECT_FALSE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(sample));
}

TEST(GameBenchmark, S1PreWindowRoutePausesAfterReadyUntilCurrentSphereIsReady)
{
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::time_point READY_INPUT_AT{};

    acceptance::GameBenchmarkS1MovementWindow window;

    EXPECT_FALSE(window.shouldPauseBeforeStart(false, false, false));
    EXPECT_TRUE(window.shouldPauseBeforeStart(true, false, true));
    EXPECT_FALSE(window.shouldPauseBeforeStart(true, true, true));
    EXPECT_TRUE(window.observeSuccessfulInput(READY_INPUT_AT, true));
    EXPECT_FALSE(window.shouldPauseBeforeStart(true, false, true));
}

TEST(GameBenchmark, S1FillMovementUsesNormalProfileUntilReadyThenStartsX30)
{
    acceptance::GameBenchmarkMovementRouteSample const route =
        acceptance::gameBenchmarkRepeatingMovementRouteAt(std::chrono::nanoseconds::zero());
    shared::Direction const fill_direction = acceptance::gameBenchmarkS1MovementDirection(route, false);
    shared::Direction const ready_direction = acceptance::gameBenchmarkS1MovementDirection(route, true);

    EXPECT_FALSE(fill_direction.accelerated);
    EXPECT_EQ(fill_direction.speedup, 30U);
    EXPECT_EQ(fill_direction.x, ready_direction.x);
    EXPECT_EQ(fill_direction.y, ready_direction.y);
    EXPECT_EQ(fill_direction.z, ready_direction.z);
    EXPECT_EQ(fill_direction.view_x, ready_direction.view_x);
    EXPECT_EQ(fill_direction.view_y, ready_direction.view_y);
    EXPECT_TRUE(ready_direction.accelerated);
    EXPECT_EQ(ready_direction.speedup, 30U);

    using Clock = std::chrono::steady_clock;
    Clock::time_point const start{};
    acceptance::GameBenchmarkS1MovementWindow window;
    EXPECT_EQ(window.routeElapsedAt(start + 5s, start, false), 5s);
    EXPECT_EQ(window.routeElapsedAt(start + 6s, start, true), 0ns);
    EXPECT_FALSE(window.observeSuccessfulInput(start + 5s, false));
    EXPECT_FALSE(window.startedAt().has_value());
    EXPECT_TRUE(window.observeSuccessfulInput(start + 6s, true));
    EXPECT_EQ(window.startedAt(), start + 6s);
    EXPECT_EQ(window.routeElapsedAt(start + 7s, start, false), 1s);
}

TEST(GameBenchmark, FullRadiusWarmupTimeoutsAreIndependentAndFailureIsTerminal)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    using acceptance::GameBenchmarkFullRadiusSequence;

    Clock::time_point const start{};
    GameBenchmarkFullRadiusSequence initial_timeout{ 2s };
    initial_timeout.start(start);
    initial_timeout.observe(start + 2s, std::nullopt, true);
    EXPECT_EQ(initial_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);
    initial_timeout.observe(start + 3s, true, true);
    EXPECT_EQ(initial_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);

    GameBenchmarkFullRadiusSequence recovery_timeout{ 2s };
    recovery_timeout.start(start);
    recovery_timeout.observe(start, true, true);
    Clock::time_point const recovery_started = start
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    recovery_timeout.observe(start + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE, std::nullopt, true);
    recovery_timeout.observe(recovery_started, std::nullopt, true);
    ASSERT_EQ(recovery_timeout.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    recovery_timeout.observe(recovery_started + 2s, std::nullopt, true);
    EXPECT_EQ(recovery_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);
}

TEST(GameBenchmark, ControlledStartRequiresSyntheticInputAndOrderedProductionGateReceipts)
{
    acceptance::GameBenchmarkStartActionEvidence const complete{
        .input_source = "synthetic-controlled-start-key",
        .production_path = "GameClientBenchmarkHooks.start_input_override->GameClient.acceptLoadingStartInput",
        .synthetic_input_override_invoked = true,
        .eligible_prompt_rendered_offset_ns = 100U,
        .synthetic_input_requested_offset_ns = 110U,
        .key_accepted_offset_ns = 120U,
        .server_start_acknowledged_offset_ns = 130U,
        .input_ordinal = 7U,
        .complete = true,
    };
    EXPECT_TRUE(acceptance::gameBenchmarkStartActionComplete(complete));

    auto const reject = [&](acceptance::GameBenchmarkStartActionEvidence invalid) {
        invalid.complete = true;
        EXPECT_FALSE(acceptance::gameBenchmarkStartActionComplete(invalid));
    };
    auto no_synthetic_input = complete;
    no_synthetic_input.synthetic_input_override_invoked = false;
    reject(no_synthetic_input);
    auto hands_on_only = complete;
    hands_on_only.input_source = "ordinary-hands-on";
    reject(hands_on_only);
    auto ack_before_key = complete;
    ack_before_key.server_start_acknowledged_offset_ns = 119U;
    reject(ack_before_key);
    auto missing_prompt = complete;
    missing_prompt.eligible_prompt_rendered_offset_ns.reset();
    reject(missing_prompt);
}

TEST(GameBenchmark, FullRadiusStationaryEpochsStartAfterDelayedBarrierScans)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};
    static constexpr std::chrono::seconds INITIAL_FRAME_FINISHED{4};
    static constexpr std::chrono::seconds SCAN_DELAY{1};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    Clock::time_point const initial_scan_finished = START + INITIAL_FRAME_FINISHED + SCAN_DELAY;
    sequence.observe(initial_scan_finished, true, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    EXPECT_EQ(sequence.phaseElapsed(initial_scan_finished), Clock::duration::zero());
    Clock::time_point const movement_started = initial_scan_finished
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(movement_started - SCAN_DELAY, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(movement_started, std::nullopt, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    Clock::time_point const recovery_started = movement_started + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    sequence.observe(recovery_started, std::nullopt, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    Clock::time_point const recovery_scan_finished = recovery_started + SCAN_DELAY;
    sequence.observe(recovery_scan_finished, true, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    EXPECT_EQ(sequence.phaseElapsed(recovery_scan_finished), Clock::duration::zero());
    Clock::time_point const stationary_finished = recovery_scan_finished
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(stationary_finished - SCAN_DELAY, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(stationary_finished, std::nullopt, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Complete);
}

TEST(GameBenchmark, FullRadiusInitialExactBarrierFinishingAfterDeadlineFails)
{
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    sequence.observe(START + WARMUP_TIMEOUT + 1ns, true, true);
    EXPECT_EQ(sequence.phase(), acceptance::GameBenchmarkFullRadiusPhase::Failed);
}

TEST(GameBenchmark, FullRadiusRecoveryExactBarrierFinishingAfterDeadlineFails)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    sequence.observe(START, true, true);
    Clock::time_point const movement_started = START + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(movement_started, std::nullopt, true);
    Clock::time_point const recovery_started = movement_started + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    sequence.observe(recovery_started, std::nullopt, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(recovery_started + WARMUP_TIMEOUT + 1ns, true, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Failed);
}

TEST(GameBenchmark, RecoveryBarrierMustRemainCloserToTravelEndThanTravelStart)
{
    static constexpr shared::HeightTileKey MOVEMENT_START{ .x = 2'056, .y = 2'048 };
    static constexpr shared::HeightTileKey MOVEMENT_END{ .x = 2'441, .y = 2'048 };
    static constexpr shared::HeightTileKey RECOVERED_AT_DESTINATION{ .x = 2'440, .y = 2'048 };
    static constexpr shared::HeightTileKey RECOVERED_AT_ORIGIN{ .x = 2'056, .y = 2'048 };
    static constexpr shared::HeightTileKey WRAPPED_START{ .x = 4'095, .y = 0 };
    static constexpr shared::HeightTileKey WRAPPED_END{ .x = 1, .y = 0 };
    static constexpr shared::HeightTileKey WRAPPED_DESTINATION{ .x = 1, .y = 0 };

    EXPECT_TRUE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        MOVEMENT_START, MOVEMENT_END, RECOVERED_AT_DESTINATION
    ));
    EXPECT_FALSE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        MOVEMENT_START, MOVEMENT_END, RECOVERED_AT_ORIGIN
    ));
    EXPECT_TRUE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        WRAPPED_START, WRAPPED_END, WRAPPED_DESTINATION
    ));
    EXPECT_FALSE(acceptance::gameBenchmarkRecoveryIsCloserToMovementEnd(
        MOVEMENT_START, MOVEMENT_START, MOVEMENT_START
    ));
}

TEST(GameBenchmark, ScriptsDistinctMovementAndCapabilityWorkloads)
{
    using acceptance::GameBenchmarkWorkload;
    using acceptance::gameBenchmarkDirection;

    shared::Direction const ordinary = gameBenchmarkDirection(GameBenchmarkWorkload::OrdinaryMovement, 0U);
    shared::Direction const speed = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200Movement, 0U);
    shared::Direction const border = gameBenchmarkDirection(GameBenchmarkWorkload::WrappedBorder, 0U);
    shared::Direction const churn = gameBenchmarkDirection(GameBenchmarkWorkload::PermissionCollisionChurn, 0U);

    EXPECT_EQ(ordinary.x, 127U);
    EXPECT_FALSE(ordinary.accelerated);
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::Speed200Movement),
        "speed-200-movement-v2");
    EXPECT_EQ(speed.x, 127U);
    EXPECT_TRUE(speed.accelerated);
    EXPECT_EQ(speed.speedup, 200U);
    EXPECT_EQ(border.speedup, 200U);
    EXPECT_EQ(gameBenchmarkDirection(GameBenchmarkWorkload::WrappedBorder, 25U).x, 127U);
    EXPECT_FALSE(churn.cycle_movement_capabilities);
    EXPECT_FALSE(gameBenchmarkDirection(GameBenchmarkWorkload::PermissionCollisionChurn, 1U)
        .cycle_movement_capabilities);
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::PermissionCollisionChurn),
        "permission-collision-churn-v2");
    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::OrdinaryMovement),
        "ordinary-movement-v2");
    EXPECT_EQ(static_cast<int8_t>(gameBenchmarkDirection(GameBenchmarkWorkload::OrdinaryMovement, 20U).x), -127);
    for (uint64_t const ordinal : { 0U, 19U, 20U, 39U, 40U }) {
        shared::Direction const travel = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200Movement, ordinal);
        EXPECT_EQ(travel.x, 127U);
        EXPECT_TRUE(travel.accelerated);
        EXPECT_EQ(travel.speedup, 200U);
    }
}

TEST(GameBenchmark, OrdinaryTurningWorkloadFollowsTheElapsedCameraRoute)
{
    using acceptance::GameBenchmarkWorkload;
    using acceptance::gameBenchmarkDirection;
    static constexpr uint64_t TICKS_PER_SECOND = 10U;

    EXPECT_EQ(acceptance::gameBenchmarkWorkloadName(GameBenchmarkWorkload::OrdinaryTurningMovement),
        "turning-x30-movement-v3");
    shared::Direction const forward = gameBenchmarkDirection(GameBenchmarkWorkload::OrdinaryTurningMovement, 0U);
    shared::Direction const looking_left = gameBenchmarkDirection(
        GameBenchmarkWorkload::OrdinaryTurningMovement, TICKS_PER_SECOND * 11U
    );
    shared::Direction const strafing_back = gameBenchmarkDirection(
        GameBenchmarkWorkload::OrdinaryTurningMovement, TICKS_PER_SECOND * 19U
    );
    shared::Direction const moving_back = gameBenchmarkDirection(
        GameBenchmarkWorkload::OrdinaryTurningMovement, TICKS_PER_SECOND * 26U
    );
    shared::Direction const final_tick = gameBenchmarkDirection(
        GameBenchmarkWorkload::OrdinaryTurningMovement, TICKS_PER_SECOND * 30U
    );
    shared::Direction const after_route = gameBenchmarkDirection(
        GameBenchmarkWorkload::OrdinaryTurningMovement, TICKS_PER_SECOND * 35U
    );

    EXPECT_EQ(forward.y, 127U);
    EXPECT_EQ(forward.z, 127U);
    EXPECT_TRUE(forward.accelerated);
    EXPECT_EQ(forward.speedup, 30U);
    EXPECT_FALSE(forward.cycle_movement_capabilities);
    EXPECT_LT(static_cast<int8_t>(looking_left.x), 0);
    EXPECT_GT(looking_left.y, 0U);
    EXPECT_EQ(static_cast<int8_t>(strafing_back.x), -79);
    EXPECT_EQ(strafing_back.y, 99U);
    EXPECT_EQ(static_cast<int8_t>(moving_back.z), 0);
    EXPECT_NE(forward.x, looking_left.x);
    EXPECT_EQ(final_tick.x, after_route.x);
    EXPECT_EQ(final_tick.y, after_route.y);
    EXPECT_EQ(final_tick.z, after_route.z);
    EXPECT_EQ(final_tick.view_x, after_route.view_x);
    EXPECT_EQ(final_tick.view_y, after_route.view_y);
    EXPECT_EQ(static_cast<int8_t>(final_tick.y), -127);
    EXPECT_EQ(final_tick.z, 0U);
}

TEST(GameBenchmark, OrdinaryTurningRouteMovesAndLooksIndependentlyAcrossTheWindow)
{
    static constexpr std::chrono::seconds FIRST_LOOK_TURN{ 6 };
    static constexpr std::chrono::seconds SECOND_LOOK_TURN{ 16 };
    static constexpr std::chrono::seconds THIRD_LOOK_TURN{ 23 };
    static constexpr std::chrono::seconds ROUTE_END{ 30 };

    acceptance::GameBenchmarkMovementRouteSample const initial =
        acceptance::gameBenchmarkMovementRouteAt(std::chrono::seconds::zero());
    acceptance::GameBenchmarkMovementRouteSample const movingForwardLookingLeft =
        acceptance::gameBenchmarkMovementRouteAt(FIRST_LOOK_TURN + std::chrono::seconds{ 5 });
    acceptance::GameBenchmarkMovementRouteSample const strafingLookingBack =
        acceptance::gameBenchmarkMovementRouteAt(SECOND_LOOK_TURN + std::chrono::seconds{ 3 });
    acceptance::GameBenchmarkMovementRouteSample const movingBackTurningRight =
        acceptance::gameBenchmarkMovementRouteAt(THIRD_LOOK_TURN + std::chrono::seconds{ 3 });
    acceptance::GameBenchmarkMovementRouteSample const ended =
        acceptance::gameBenchmarkMovementRouteAt(ROUTE_END);
    shared::Direction const initial_direction = acceptance::gameBenchmarkDirection(initial);
    shared::Direction const forward_left_direction = acceptance::gameBenchmarkDirection(movingForwardLookingLeft);
    shared::Direction const strafe_back_direction = acceptance::gameBenchmarkDirection(strafingLookingBack);
    shared::Direction const reverse_turn_direction = acceptance::gameBenchmarkDirection(movingBackTurningRight);

    EXPECT_EQ(initial.forward, 1);
    EXPECT_EQ(initial.strafe, 0);
    EXPECT_EQ(initial.vertical, 1);
    EXPECT_EQ(initial.yaw_degrees, 0.0);
    EXPECT_EQ(initial_direction.y, 127U);
    EXPECT_EQ(initial_direction.z, 127U);
    EXPECT_EQ(initial_direction.speedup, 30U);
    EXPECT_EQ(movingForwardLookingLeft.forward, 1);
    EXPECT_LT(movingForwardLookingLeft.yaw_degrees, -40.0);
    EXPECT_GT(movingForwardLookingLeft.yaw_degrees, -90.0);
    EXPECT_NE(forward_left_direction.x, initial_direction.x);
    EXPECT_EQ(strafingLookingBack.strafe, 1);
    EXPECT_LT(strafingLookingBack.yaw_degrees, -120.0);
    EXPECT_NE(strafe_back_direction.x, forward_left_direction.x);
    EXPECT_EQ(movingBackTurningRight.forward, -1);
    EXPECT_EQ(movingBackTurningRight.vertical, -1);
    EXPECT_EQ(static_cast<int8_t>(reverse_turn_direction.z), -127);
    EXPECT_GT(movingBackTurningRight.yaw_degrees, -180.0);
    EXPECT_LT(movingBackTurningRight.yaw_degrees, 0.0);
    EXPECT_NE(reverse_turn_direction.x, strafe_back_direction.x);
    EXPECT_NE(movingBackTurningRight.pitch_degrees, strafingLookingBack.pitch_degrees);
    EXPECT_EQ(ended.yaw_degrees, 0.0);
    EXPECT_EQ(ended.pitch_degrees, 0.0);
    EXPECT_EQ(
        acceptance::gameBenchmarkMovementRouteAt(ROUTE_END + std::chrono::seconds{ 10 }),
        ended
    );
}

TEST(GameBenchmark, DiagnosticRouteRepeatsMovementAndLookWithoutChangingTheQualificationSampler)
{
    using namespace std::chrono_literals;
    auto const first_cycle_forward = acceptance::gameBenchmarkMovementRouteAt(1s);
    auto const second_cycle_forward = acceptance::gameBenchmarkRepeatingMovementRouteAt(31s);
    auto const first_cycle_look = acceptance::gameBenchmarkMovementRouteAt(11s);
    auto const second_cycle_look = acceptance::gameBenchmarkRepeatingMovementRouteAt(41s);
    auto const first_cycle_strafe = acceptance::gameBenchmarkMovementRouteAt(19s);
    auto const second_cycle_strafe = acceptance::gameBenchmarkRepeatingMovementRouteAt(49s);
    auto const final_cycle_reverse = acceptance::gameBenchmarkRepeatingMovementRouteAt(83s);

    EXPECT_EQ(second_cycle_forward, first_cycle_forward);
    EXPECT_EQ(second_cycle_look, first_cycle_look);
    EXPECT_EQ(second_cycle_strafe, first_cycle_strafe);
    EXPECT_EQ(final_cycle_reverse, acceptance::gameBenchmarkMovementRouteAt(23s));
    EXPECT_EQ(acceptance::gameBenchmarkMovementRouteCycleAt(29s), 0U);
    EXPECT_EQ(acceptance::gameBenchmarkMovementRouteCycleAt(30s), 1U);
    EXPECT_EQ(acceptance::gameBenchmarkMovementRouteCycleAt(89s), 2U);
    EXPECT_EQ(acceptance::gameBenchmarkMovementRouteAt(83s), acceptance::gameBenchmarkMovementRouteAt(30s));
}

TEST(GameBenchmark, Speed200MovementUsesFullWrappedFlightDisplacementPerTick)
{
    static constexpr int32_t START_X = shared::World::FLIGHT_MAX_CELL - 50;
    static constexpr int32_t START_Y = 100;
    static constexpr int32_t START_Z = 800;
    static constexpr uint64_t WORLD_EXTENT_SUBCELLS =
        (static_cast<uint64_t>(shared::World::FLIGHT_MAX_CELL) + 1U) * shared::SUBCELLS_PER_CELL;
    static constexpr uint64_t EXPECTED_DELTA_SUBCELLS =
        static_cast<uint64_t>(shared::MOVEMENT_SUBCELLS_PER_TICK) * 200U;

    shared::World world{ shared::WorldMode::Flight };
    world.spawnPlayer(1U, '@', { .x = START_X, .y = START_Y, .z = START_Z });
    std::optional<shared::Player> const before = world.player(1U);
    ASSERT_TRUE(before.has_value());

    shared::Direction const direction = acceptance::gameBenchmarkDirection(
        acceptance::GameBenchmarkWorkload::Speed200Movement, 0U
    );
    ASSERT_EQ(direction.x, 127U);
    ASSERT_TRUE(direction.accelerated);
    ASSERT_EQ(direction.speedup, 200U);
    ASSERT_TRUE(world.movePlayer(1U, direction, shared::TICK));

    std::optional<shared::Player> const after = world.player(1U);
    ASSERT_TRUE(after.has_value());
    auto const horizontalSubcells = [](shared::Player const& player) {
        return static_cast<uint64_t>(player.x) * shared::SUBCELLS_PER_CELL + player.x_subcell;
    };
    uint64_t const expected_position =
        (horizontalSubcells(before.value()) + EXPECTED_DELTA_SUBCELLS) % WORLD_EXTENT_SUBCELLS;
    EXPECT_EQ(horizontalSubcells(after.value()), expected_position);
    EXPECT_LT(after.value().x, before.value().x);
}

TEST(GameBenchmark, CompletesResidentToVisibleReadinessAcrossMovingCenters)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = false,
        .current_mesh_visible = false,
        .accepted_near_admissions = 0U,
    }), start);
    EXPECT_EQ(tracker.evidence().eligible_mesh_episode_count, 0U);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start + 1s);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 4s);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 4s + 100ms);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.center_change_count, 2U);
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.deadline_miss_count, 0U);
    EXPECT_EQ(evidence.open_mesh_episode_count, 0U);
    EXPECT_EQ(evidence.max_ready_latency_ns, 3'000'000'000U);
    EXPECT_EQ(evidence.far_admissions_while_near_pending, 0U);
    EXPECT_EQ(evidence.final_desired_tile_count, 1U);
    EXPECT_EQ(evidence.final_current_mesh_visible_count, 1U);
    EXPECT_TRUE(evidence.complete);
}

TEST(GameBenchmark, RetainsCurrentNearCoverageLossAfterLaterRecovery)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(fullNearWindow({ 0, 0 }, acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT), start);
    tracker.observe(fullNearWindow({ 1, 0 }, 37U), start + 1s);
    tracker.observe(fullNearWindow({ 2, 0 }, acceptance::GAME_BENCHMARK_TERRAIN_NEAR_TILE_COUNT), start + 2s);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.current_near_sample_count, 3U);
    EXPECT_EQ(evidence.incomplete_current_near_sample_count, 1U);
    EXPECT_EQ(evidence.max_missing_current_mesh_count, 188U);
    EXPECT_FALSE(evidence.current_near_coverage_complete);
}

TEST(GameBenchmark, RejectsFarAdmissionWhileCurrentNearWorkIsPending)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
        .accepted_far_admissions = 1U,
        .far_admissions_while_near_pending = 1U,
    }), start);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
        .accepted_far_admissions = 1U,
        .far_admissions_while_near_pending = 1U,
    }), start + 1s);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
        .accepted_far_admissions = 1U,
        .far_admissions_while_near_pending = 1U,
    }), start + 1100ms);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.center_change_count, 2U);
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.final_desired_tile_count, 1U);
    EXPECT_EQ(evidence.final_current_mesh_visible_count, 1U);
    EXPECT_EQ(evidence.accepted_near_admissions, 1U);
    EXPECT_EQ(evidence.far_admissions_while_near_pending, 1U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, RecordsDeadlineMissEvenWhenMeshBecomesVisibleLater)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start + 4s + 1ms);
    tracker.observe(oneNearTile({
        .center = { 2, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 5s);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.deadline_miss_count, 1U);
    EXPECT_EQ(evidence.open_mesh_episode_count, 0U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, DoesNotCompleteWhenCurrentNearIncludesNonresidentTiles)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    tracker.observe(oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    }), start);
    tracker.observe(oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = true,
        .accepted_near_admissions = 1U,
    }), start + 1s);

    acceptance::GameBenchmarkTerrainObservation final_sample{};
    final_sample.center = shared::HeightTileKey{ 2, 0 };
    final_sample.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 1, 0 },
        .desired = true,
        .resident = true,
        .current_mesh_visible = true,
    };
    final_sample.near_tiles[113U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 2, 0 },
        .desired = true,
    };
    final_sample.desired_count = 2U;
    final_sample.resident_count = 1U;
    final_sample.nonresident_count = 1U;
    final_sample.current_mesh_visible_count = 1U;
    final_sample.accepted_near_admissions = 1U;
    tracker.observe(final_sample, start + 2s);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.final_desired_tile_count, 2U);
    EXPECT_EQ(evidence.final_current_mesh_visible_count, 1U);
    EXPECT_EQ(evidence.final_nonresident_tile_count, 1U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, CensorsDepartedKeyAndKeepsCurrentMissingEpisodeOpen)
{
    using Clock = std::chrono::steady_clock;

    acceptance::GameBenchmarkTerrainReadinessTracker tracker;
    Clock::time_point const start{};
    acceptance::GameBenchmarkTerrainObservation first = oneNearTile({
        .center = { 0, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 1U,
    });
    tracker.observe(first, start);

    acceptance::GameBenchmarkTerrainObservation second = oneNearTile({
        .center = { 1, 0 },
        .key = { 0, 0 },
        .resident = true,
        .current_mesh_visible = false,
        .accepted_near_admissions = 2U,
    });
    second.near_tiles[113U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 1, 0 },
        .desired = true,
        .resident = true,
        .pending = true,
    };
    ++second.desired_count;
    ++second.resident_count;
    ++second.pending_count;
    tracker.observe(second, start + 1s);

    acceptance::GameBenchmarkTerrainObservation final_sample{};
    final_sample.center = shared::HeightTileKey{ 2, 0 };
    final_sample.near_tiles[112U] = acceptance::GameBenchmarkTerrainTileObservation{
        .key = { 1, 0 },
        .desired = true,
        .resident = true,
        .pending = true,
    };
    final_sample.desired_count = 1U;
    final_sample.resident_count = 1U;
    final_sample.pending_count = 1U;
    final_sample.accepted_near_admissions = 2U;
    tracker.observe(final_sample, start + 1500ms);

    acceptance::GameBenchmarkTerrainReadinessEvidence const evidence = tracker.evidence();
    EXPECT_EQ(evidence.eligible_mesh_episode_count, 2U);
    EXPECT_EQ(evidence.ready_mesh_episode_count, 0U);
    EXPECT_EQ(evidence.left_near_unready_episode_count, 1U);
    EXPECT_EQ(evidence.open_mesh_episode_count, 1U);
    EXPECT_EQ(evidence.deadline_miss_count, 0U);
    EXPECT_FALSE(evidence.complete);
}

TEST(GameBenchmark, S1ReadinessDeadlinesAreIndependentOfPromptOrdering)
{
    EXPECT_TRUE(acceptance::gameBenchmarkS1ReadinessDeadlinesMet(3'335'092'166U, 3'308'200'250U));
#if defined(_WIN32)
    constexpr uint64_t PLAYABLE_DEADLINE_NS = 15'000'000'000U;
#else
    constexpr uint64_t PLAYABLE_DEADLINE_NS = 20'000'000'000U;
#endif
    EXPECT_TRUE(acceptance::gameBenchmarkS1ReadinessDeadlinesMet(4'000'000'000U, PLAYABLE_DEADLINE_NS));
    EXPECT_FALSE(acceptance::gameBenchmarkS1ReadinessDeadlinesMet(4'000'000'001U, 3'308'200'250U));
    EXPECT_FALSE(acceptance::gameBenchmarkS1ReadinessDeadlinesMet(3'335'092'166U, PLAYABLE_DEADLINE_NS + 1U));
    EXPECT_FALSE(acceptance::gameBenchmarkS1ReadinessDeadlinesMet(std::nullopt, 3'308'200'250U));
    EXPECT_FALSE(acceptance::gameBenchmarkS1ReadinessDeadlinesMet(3'335'092'166U, std::nullopt));
}

TEST(GameBenchmark, S1BoundarySampleRetainsGenuineAuthorityWithFreshReadySphere)
{
    shared::PlayerPosition const center{ .x = 12, .y = 13, .z = 14 };
    acceptance::GameBenchmarkFullRadiusEvidence::AuthoritativePlayableSphereUpdate const authority{
        .update_ordinal = 1U,
        .offset_ns = 0U,
        .state_revision = 8U,
        .acknowledged_input_sequence = 7U,
        .authoritative_position = center,
        .playable_sphere = {},
    };
    acceptance::GameBenchmarkFrameSample::PlayableSphere sphere{
        .radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
        .active_coverage_epoch = 9U,
        .active_coverage_center = center,
        .world_loading_center = center,
        .ready_chunks = 419'904U,
        .required_chunks = 419'904U,
        .ready = true,
        .coverage_scope = "horizontal_full_volume",
        .required_authoritative_columns = 6'561U,
        .resident_authoritative_columns = 6'561U,
        .vertical_chunk_count = 64U,
        .authoritative_columns_center = center,
    };
    acceptance::GameBenchmarkFrameSample::VisualLod const lod{
        .radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        .terrain_identity = std::nullopt,
        .coverage_center = std::nullopt,
    };
    auto const anchor = acceptance::gameBenchmarkS1TrajectoryAnchorSample(authority, sphere, lod, 91U);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(anchor->update_ordinal, authority.update_ordinal);
    EXPECT_EQ(anchor->state_revision, authority.state_revision);
    EXPECT_EQ(anchor->acknowledged_input_sequence, authority.acknowledged_input_sequence);
    EXPECT_EQ(anchor->offset_ns, 91U);
    EXPECT_TRUE(acceptance::gameBenchmarkS1CurrentTerrainSampleReady(anchor->playable_sphere));
    ASSERT_TRUE(anchor->visual_lod.has_value());
    EXPECT_EQ(anchor->visual_lod->radius_blocks, shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS);
    sphere.ready_chunks -= 1U;
    EXPECT_FALSE(acceptance::gameBenchmarkS1TrajectoryAnchorSample(authority, sphere, lod, 92U));
    sphere.ready_chunks += 1U;
    sphere.world_loading_center->x += 1U;
    EXPECT_FALSE(acceptance::gameBenchmarkS1TrajectoryAnchorSample(authority, sphere, lod, 93U));
}

TEST(GameBenchmark, BatchedFenceObservationsPreserveCompletenessWithoutCreditingMissingTime)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    std::vector<acceptance::GameBenchmarkFrameCompletion> completions;
    for (uint32_t index = 0U; index < 2U; ++index) {
        uint64_t const sequence = index + 1U;
        uint64_t const submitted = sequence * 8'000'000U;
        frames.push_back({
            .loop_ordinal = sequence,
            .presentation_succeeded = true,
            .swapchain_generation = 1U,
            .application_present_id = index + 1U,
            .submitted_frame_sequence_id = sequence,
            .submitted_frame_slot = index,
            .submitted_frame_offset_ns = submitted,
            .submitted_frame_phase = "s1-x30-movement",
            .started_offset_ns = submitted - 8'000'000U,
            .loop_duration_ns = 8'000'000U,
            .terrain_phase = "s1-x30-movement",
        });
        completions.push_back({
            .sequence_id = sequence,
            .swapchain_generation = 1U,
            .frame_slot = index,
            .submitted_offset_ns = submitted,
            .completion_observed_offset_ns = 16'000'000U,
            .application_present_id = index + 1U,
            .submission_phase = "s1-x30-movement",
        });
    }
    acceptance::GameBenchmarkFrameBudgetWindow const window{
        .start_offset_ns = 0U,
        .end_offset_ns = 16'000'000U,
        .required_duration_ns = 16'000'000U,
        .phase = "s1-x30-movement",
        .complete = true,
    };
    auto const evidence = acceptance::gameBenchmarkFrameBudget(frames, completions, 8'333'333U, window);
    EXPECT_TRUE(evidence.measurement_window_complete);
    EXPECT_EQ(evidence.fence_confirmed_frame_count, 2U);
    EXPECT_EQ(evidence.measured_time_ns, 16'000'000U);
    EXPECT_EQ(evidence.in_budget_time_ns, 0U);
    EXPECT_FALSE(evidence.passed);
}

TEST(GameBenchmark, S1VisualReadinessRejectsInactiveSceneWithCorrectRadiusAndNonemptyLeaves)
{
    EXPECT_FALSE(acceptance::gameBenchmarkS1VisualLodSampleReady({{
        .radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        .scene_active = false,
        .leaf_count = 128U,
        .terrain_identity = std::nullopt,
        .coverage_center = std::nullopt,
    }}));
}

TEST(GameBenchmark, S1VisualReadinessRejectsEmptyActiveSceneWithCorrectRadius)
{
    EXPECT_FALSE(acceptance::gameBenchmarkS1VisualLodSampleReady({{
        .radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        .scene_active = true,
        .leaf_count = 0U,
        .terrain_identity = std::nullopt,
        .coverage_center = std::nullopt,
    }}));
}

TEST(GameBenchmark, S1VisualReadinessRequiresCurrentActualCoverageFromTheSubmittedScene)
{
    acceptance::GameBenchmarkFrameSample::VisualLod complete{
        .radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
        .scene_active = true,
        .leaf_count = 6'561U,
        .actual_column_count = 6'561U,
        .terrain_identity = shared::TerrainIdentity{.configuration_identity = "current-configuration"},
        .session_join_epoch = 1U,
        .coverage_epoch = 2U,
        .submitted_frame_count = 3U,
        .actual_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
        .required_actual_columns = 6'561U,
        .presented_current_actual_columns = 6'561U,
        .coverage_center = shared::PlayerPosition{},
        .sources_current = true,
    };
    EXPECT_TRUE(acceptance::gameBenchmarkS1VisualLodSampleReady(complete));
    EXPECT_FALSE(acceptance::gameBenchmarkS1VisualLodSampleReady(std::nullopt));
    for (auto const missing : {0U, 1U, 6'560U}) {
        auto incomplete = complete;
        incomplete.presented_current_actual_columns = missing;
        EXPECT_FALSE(acceptance::gameBenchmarkS1VisualLodSampleReady(incomplete));
    }
    complete.sources_current = false;
    EXPECT_FALSE(acceptance::gameBenchmarkS1VisualLodSampleReady(complete));
    complete.sources_current = true;
    complete.session_join_epoch = 0U;
    EXPECT_FALSE(acceptance::gameBenchmarkS1VisualLodSampleReady(complete));
}

TEST(GameBenchmark, S1FirstX30InputWaitsForAuthenticatedAnchorWithoutSuppressingFillMovement)
{
    acceptance::GameBenchmarkS1MovementWindow window;
    EXPECT_FALSE(window.shouldPauseBeforeStart(false, false, false));
    EXPECT_TRUE(window.shouldPauseBeforeStart(true, true, false));
    EXPECT_FALSE(window.startedAt().has_value());
    EXPECT_FALSE(window.shouldPauseBeforeStart(true, true, true));
    auto const sent_at = std::chrono::steady_clock::time_point{};
    EXPECT_TRUE(window.observeSuccessfulInput(sent_at, true));
    EXPECT_FALSE(window.shouldPauseBeforeStart(true, true, false));
    EXPECT_FALSE(window.shouldPauseBeforeStart(true, false, false));
}

TEST(GameBenchmark, S1RenderedCameraTracksBodyEyeAndWrappedInterpolatedMovement)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    for (uint32_t index = 0U; index < 30U; ++index) {
        uint32_t const x = (65'520U + index * 16U) % 65'536U;
        frames.push_back({
            .presentation_succeeded = true,
            .submitted_frame_phase = {},
            .loop_duration_ns = 1U,
            .terrain_phase = "s1-x30-movement",
            .playable_sphere = acceptance::GameBenchmarkFrameSample::PlayableSphere{
                .active_coverage_epoch = std::nullopt,
                .active_coverage_center = std::nullopt,
                .world_loading_center = shared::PlayerPosition{
                    .x = static_cast<int32_t>(x), .y = 32, .z = 96,
                },
                .coverage_scope = {},
                .authoritative_columns_center = std::nullopt,
            },
            .visual_lod = acceptance::GameBenchmarkFrameSample::VisualLod{
                .terrain_identity = std::nullopt,
                .submitted_frame_count = index + 1U,
                .coverage_center = std::nullopt,
            },
            .presented_player = acceptance::GameBenchmarkFrameSample::PresentedPlayer{
                .body = {static_cast<double>((x + 65'528U) % 65'536U), 32.0, 96.0},
                .submitted_frame_count = index + 1U,
            },
            .diagnostic_camera = {
                static_cast<double>((x + 65'528U) % 65'536U) + 0.3125,
                32.3125, 97.625, 0.0, 0.0, 0.0, 70.0,
            },
        });
    }
    EXPECT_TRUE(acceptance::gameBenchmarkS1RenderedCameraTrajectoryComplete(frames));
    for (auto& frame : frames) {
        frame.diagnostic_camera[0] = 1e9;
    }
    EXPECT_FALSE(acceptance::gameBenchmarkS1RenderedCameraTrajectoryComplete(frames));
}

TEST(GameBenchmark, S1RenderedCameraRejectsStationaryViewWhileAuthorityMoves)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    frames.reserve(30U);
    for (uint32_t index = 0U; index < 30U; ++index) {
        frames.push_back({
            .presentation_succeeded = true,
            .submitted_frame_phase = {},
            .loop_duration_ns = 1U,
            .terrain_phase = "s1-x30-movement",
            .playable_sphere = acceptance::GameBenchmarkFrameSample::PlayableSphere{
                .active_coverage_epoch = std::nullopt,
                .active_coverage_center = std::nullopt,
                .world_loading_center = shared::PlayerPosition{
                    .x = static_cast<int32_t>(index), .y = 32, .z = 96,
                },
                .coverage_scope = {},
                .authoritative_columns_center = std::nullopt,
            },
            .visual_lod = acceptance::GameBenchmarkFrameSample::VisualLod{
                .terrain_identity = std::nullopt,
                .submitted_frame_count = index + 1U,
                .coverage_center = std::nullopt,
            },
            .presented_player = acceptance::GameBenchmarkFrameSample::PresentedPlayer{
                .body = {0.0, 32.0, 96.0}, .submitted_frame_count = index + 1U,
            },
            .diagnostic_camera = {0.3125, 32.3125, 97.625, 0.0, 0.0, 0.0, 70.0},
        });
    }
    EXPECT_FALSE(acceptance::gameBenchmarkS1RenderedCameraTrajectoryComplete(frames));
}

TEST(GameBenchmark, S1PresentedSceneCoverageUsesExactWrappedActualKeys)
{
    client::TerrainLodScene scene;
    for (int32_t y = -40; y <= 40; ++y) {
        for (int32_t x = -40; x <= 40; ++x) {
            scene.authority_sources.push_back({
                .coordinate = {
                    .x = (x + 4'096) % 4'096, .y = (y + 4'096) % 4'096, .z = 0,
                },
                .identity = scene.identity,
            });
            scene.leaves.push_back({
                .level = shared::TerrainLodLevel::L3,
                .x = static_cast<uint32_t>((x + 4'096) % 4'096),
                .y = static_cast<uint32_t>((y + 4'096) % 4'096),
            });
        }
    }
    EXPECT_EQ(acceptance::gameBenchmarkS1PresentedCurrentColumnCount(scene, {}), 6'561U);
    scene.authority_sources.back().identity.seed += 1U;
    EXPECT_EQ(acceptance::gameBenchmarkS1PresentedCurrentColumnCount(scene, {}), 6'560U);
    scene.authority_sources.back().identity = scene.identity;
    scene.leaves.back().x = 41U;
    EXPECT_EQ(acceptance::gameBenchmarkS1PresentedCurrentColumnCount(scene, {}), 6'560U);
    scene.leaves.push_back(scene.leaves.front());
    EXPECT_EQ(acceptance::gameBenchmarkS1PresentedCurrentColumnCount(scene, {}), 6'560U);
    scene.leaves.back().level = shared::TerrainLodLevel::L0;
    EXPECT_EQ(acceptance::gameBenchmarkS1PresentedCurrentColumnCount(scene, {}), 6'560U);
}
