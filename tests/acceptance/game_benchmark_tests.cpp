#include <client/render/StoneFaceCapacity.hpp>
#include <shared/net/Message.hpp>
#include <shared/world/World.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/GameBenchmark.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan_core.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <memory>
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
                .phase = "speed-200-movement",
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

TEST(GameBenchmark, AcceptsExpectedHistoryWithoutLastDrainTimestamp)
{
    acceptance::GameBenchmarkPresentationTimingEvidence evidence = completePresentationTimingEvidence();
    evidence.records.pop_back();
    evidence.polls.back().status = acceptance::GameBenchmarkPresentationPollStatus::Empty;
    evidence.polls.back().returned_count = 0U;
    EXPECT_TRUE(acceptance::gameBenchmarkPresentationTimingComplete(evidence));
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
    options.workload = acceptance::GameBenchmarkWorkload::Speed200Movement;
    EXPECT_EQ(acceptance::gameBenchmarkDeadline(options), options.deadline);
}

TEST(GameBenchmark, SuccessfulRequestIntervalsIncludeFailedAttemptsAndStayWithinTheirStationaryPhase)
{
    static constexpr uint64_t MILLISECOND_NS = 1'000'000U;
    std::array<acceptance::GameBenchmarkFrameSample, 6U> const frames{
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 0U,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "baseline-stationary",
            .terrain_near = std::nullopt,
            .renderer_stone_face_growth = std::nullopt,
            .diagnostic_frame_slot = std::nullopt,
            .diagnostic_gpu_timestamp_reason = {},
            .diagnostic_gpu_terrain_duration_ns = std::nullopt,
            .latest_gpu_frame_duration_ns = std::nullopt,
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = false,
            .started_offset_ns = 2U * MILLISECOND_NS,
            .loop_duration_ns = 8U * MILLISECOND_NS,
            .terrain_phase = "baseline-stationary",
            .terrain_near = std::nullopt,
            .renderer_stone_face_growth = std::nullopt,
            .diagnostic_frame_slot = std::nullopt,
            .diagnostic_gpu_timestamp_reason = {},
            .diagnostic_gpu_terrain_duration_ns = std::nullopt,
            .latest_gpu_frame_duration_ns = std::nullopt,
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 10U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "baseline-stationary",
            .terrain_near = std::nullopt,
            .renderer_stone_face_growth = std::nullopt,
            .diagnostic_frame_slot = std::nullopt,
            .diagnostic_gpu_timestamp_reason = {},
            .diagnostic_gpu_terrain_duration_ns = std::nullopt,
            .latest_gpu_frame_duration_ns = std::nullopt,
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 100U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "speed-200-movement",
            .terrain_near = std::nullopt,
            .renderer_stone_face_growth = std::nullopt,
            .diagnostic_frame_slot = std::nullopt,
            .diagnostic_gpu_timestamp_reason = {},
            .diagnostic_gpu_terrain_duration_ns = std::nullopt,
            .latest_gpu_frame_duration_ns = std::nullopt,
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 1'000U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "recovered-stationary",
            .terrain_near = std::nullopt,
            .renderer_stone_face_growth = std::nullopt,
            .diagnostic_frame_slot = std::nullopt,
            .diagnostic_gpu_timestamp_reason = {},
            .diagnostic_gpu_terrain_duration_ns = std::nullopt,
            .latest_gpu_frame_duration_ns = std::nullopt,
        },
        acceptance::GameBenchmarkFrameSample{
            .presentation_succeeded = true,
            .started_offset_ns = 1'020U * MILLISECOND_NS,
            .loop_duration_ns = 2U * MILLISECOND_NS,
            .terrain_phase = "recovered-stationary",
            .terrain_near = std::nullopt,
            .renderer_stone_face_growth = std::nullopt,
            .diagnostic_frame_slot = std::nullopt,
            .diagnostic_gpu_timestamp_reason = {},
            .diagnostic_gpu_terrain_duration_ns = std::nullopt,
            .latest_gpu_frame_duration_ns = std::nullopt,
        },
    };

    auto const baseline = acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "baseline-stationary");
    ASSERT_EQ(baseline.size(), 1U);
    EXPECT_EQ(baseline.front(), 10ms);
    auto const recovered = acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "recovered-stationary");
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front(), 20ms);
    EXPECT_TRUE(acceptance::gameBenchmarkSuccessfulPresentIntervals(frames, "speed-200-movement").empty());
}

TEST(GameBenchmark, FrameBudgetLimitsOverBudgetTimeInsteadOfOverBudgetFrameCount)
{
    std::vector<acceptance::GameBenchmarkFrameSample> frames;
    frames.reserve(1'001U);
    for (uint32_t index = 0U; index < 999U; ++index) {
        frames.push_back({ .loop_duration_ns = 1'000'000U });
    }
    frames.push_back({ .loop_duration_ns = 2'000'000U });
    frames.push_back({ .excluded = true, .loop_duration_ns = 60'000'000'000U });

    acceptance::GameBenchmarkFrameBudgetEvidence const passing = acceptance::gameBenchmarkFrameBudget(
        frames, 1'000'000U
    );
    EXPECT_TRUE(passing.passed);
    EXPECT_EQ(passing.sample_count, 1'000U);
    EXPECT_EQ(passing.measured_time_ns, 1'001'000'000U);
    EXPECT_EQ(passing.in_budget_time_ns, 999'000'000U);

    frames[999U].loop_duration_ns = 3'000'000U;
    acceptance::GameBenchmarkFrameBudgetEvidence const failing = acceptance::gameBenchmarkFrameBudget(
        frames, 1'000'000U
    );
    EXPECT_FALSE(failing.passed);
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
    sequence.observe(start + 10s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 12s, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::InitialWarmup);
    sequence.observe(start + 15s, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 45s - 1ns, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(start + 45s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(start + 75s - 1ns, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    sequence.observe(start + 75s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 104s, false);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(start + 105s, true);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(start + 135s - 1ns, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(start + 135s, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Complete);
}

TEST(GameBenchmark, FullRadiusWarmupTimeoutsAreIndependentAndFailureIsTerminal)
{
    using Clock = std::chrono::steady_clock;
    using acceptance::GameBenchmarkFullRadiusPhase;
    using acceptance::GameBenchmarkFullRadiusSequence;

    Clock::time_point const start{};
    GameBenchmarkFullRadiusSequence initial_timeout{ 2s };
    initial_timeout.start(start);
    initial_timeout.observe(start + 2s, std::nullopt);
    EXPECT_EQ(initial_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);
    initial_timeout.observe(start + 3s, true);
    EXPECT_EQ(initial_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);

    GameBenchmarkFullRadiusSequence recovery_timeout{ 2s };
    recovery_timeout.start(start);
    recovery_timeout.observe(start, true);
    Clock::time_point const recovery_started = start
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    recovery_timeout.observe(start + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE, std::nullopt);
    recovery_timeout.observe(recovery_started, std::nullopt);
    ASSERT_EQ(recovery_timeout.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    recovery_timeout.observe(recovery_started + 2s, std::nullopt);
    EXPECT_EQ(recovery_timeout.phase(), GameBenchmarkFullRadiusPhase::Failed);
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
    sequence.observe(initial_scan_finished, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    EXPECT_EQ(sequence.phaseElapsed(initial_scan_finished), Clock::duration::zero());
    Clock::time_point const movement_started = initial_scan_finished
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(movement_started - SCAN_DELAY, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::BaselineStationary);
    sequence.observe(movement_started, std::nullopt);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Movement);
    Clock::time_point const recovery_started = movement_started + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    sequence.observe(recovery_started, std::nullopt);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    Clock::time_point const recovery_scan_finished = recovery_started + SCAN_DELAY;
    sequence.observe(recovery_scan_finished, true);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    EXPECT_EQ(sequence.phaseElapsed(recovery_scan_finished), Clock::duration::zero());
    Clock::time_point const stationary_finished = recovery_scan_finished
        + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(stationary_finished - SCAN_DELAY, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveredStationary);
    sequence.observe(stationary_finished, std::nullopt);
    EXPECT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::Complete);
}

TEST(GameBenchmark, FullRadiusInitialExactBarrierFinishingAfterDeadlineFails)
{
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::time_point START{};
    static constexpr std::chrono::seconds WARMUP_TIMEOUT{30};

    acceptance::GameBenchmarkFullRadiusSequence sequence{WARMUP_TIMEOUT};
    sequence.start(START);
    sequence.observe(START + WARMUP_TIMEOUT + 1ns, true);
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
    sequence.observe(START, true);
    Clock::time_point const movement_started = START + acceptance::GAME_BENCHMARK_FULL_RADIUS_STATIONARY_SAMPLE;
    sequence.observe(movement_started, std::nullopt);
    Clock::time_point const recovery_started = movement_started + acceptance::GAME_BENCHMARK_FULL_RADIUS_MOVEMENT;
    sequence.observe(recovery_started, std::nullopt);
    ASSERT_EQ(sequence.phase(), GameBenchmarkFullRadiusPhase::RecoveryWarmup);
    sequence.observe(recovery_started + WARMUP_TIMEOUT + 1ns, true);
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

    EXPECT_EQ(ordinary.x, 1U);
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
        "permission-collision-churn-v1");
    EXPECT_EQ(static_cast<int8_t>(gameBenchmarkDirection(GameBenchmarkWorkload::OrdinaryMovement, 20U).x), -1);
    for (uint64_t const ordinal : { 0U, 19U, 20U, 39U, 40U }) {
        shared::Direction const travel = gameBenchmarkDirection(GameBenchmarkWorkload::Speed200Movement, ordinal);
        EXPECT_EQ(travel.x, 127U);
        EXPECT_TRUE(travel.accelerated);
        EXPECT_EQ(travel.speedup, 200U);
    }
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
