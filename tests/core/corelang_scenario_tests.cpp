#include <shared/scenario/Scenario.hpp>

#include <acceptance/ScenarioRunner.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace {

[[nodiscard]]
shared::ScenarioLimits scenarioLimits()
{
    return {
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
    };
}

[[nodiscard]]
std::string readScenario(std::string_view const filename)
{
    std::filesystem::path const path = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path()
        / "scenarios" / std::filesystem::path{std::string{filename}};
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class Cancelled final : public shared::ScenarioCancellation {
public:
    [[nodiscard]] bool isCancellationRequested() const noexcept override { return true; }
};

TEST(CoreLangScenario, LowersComplexMultiClientFlightScriptIntoTypedNativeOperations)
{
    auto const result = shared::parseScenarioSource(
        "s5_flight_multiplayer.core",
        readScenario("s5_flight_multiplayer.core"),
        scenarioLimits()
    );

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->profile(), shared::ScenarioProfile::Flight3dV1);
    EXPECT_EQ(result->seed(), 42U);
    ASSERT_EQ(result->actors().size(), 2U);
    EXPECT_EQ(result->actors()[0].x, -4);
    EXPECT_EQ(result->actors()[0].z, 10);
    EXPECT_EQ(result->actors()[1].yaw_degrees, 90);
    ASSERT_EQ(result->operations().size(), 5U);
    EXPECT_NE(std::get_if<shared::ScenarioInputOperation>(&result->operations()[0].data), nullptr);
    EXPECT_NE(std::get_if<shared::ScenarioCameraInputOperation>(&result->operations()[1].data), nullptr);
    EXPECT_EQ(result->totalTicks(), 10U);
    EXPECT_EQ(result->evidenceCount(), 2U);
    EXPECT_FALSE(shared::scenarioReplayId(*result).empty());
}

TEST(CoreLangScenario, RunsScriptedDirectAndCameraFlightThroughTheAuthoritativeRunner)
{
    auto const multiplayer = shared::parseScenarioSource(
        "s5_flight_multiplayer.core",
        readScenario("s5_flight_multiplayer.core"),
        scenarioLimits()
    );
    auto const camera = shared::parseScenarioSource(
        "s5_flight_camera.core",
        readScenario("s5_flight_camera.core"),
        scenarioLimits()
    );
    auto const boundary = shared::parseScenarioSource(
        "s5_flight_boundary.core",
        readScenario("s5_flight_boundary.core"),
        scenarioLimits()
    );
    ASSERT_TRUE(multiplayer.has_value()) << multiplayer.error().message;
    ASSERT_TRUE(camera.has_value()) << camera.error().message;
    ASSERT_TRUE(boundary.has_value()) << boundary.error().message;

    acceptance::ScenarioRunOptions const options{
        .deadline = std::chrono::seconds{5},
        .network_poll_interval = std::chrono::milliseconds{1},
    };
    auto const multiplayer_result = acceptance::runScenario(*multiplayer, options);
    auto const camera_result = acceptance::runScenario(*camera, options);
    auto const boundary_result = acceptance::runScenario(*boundary, options);

    ASSERT_TRUE(multiplayer_result.has_value()) << multiplayer_result.error();
    ASSERT_TRUE(camera_result.has_value()) << camera_result.error();
    ASSERT_TRUE(boundary_result.has_value()) << boundary_result.error();
    EXPECT_TRUE(multiplayer_result->passed);
    EXPECT_TRUE(camera_result->passed);
    EXPECT_TRUE(boundary_result->passed);
    EXPECT_EQ(multiplayer_result->clients_accepted, 2U);
    EXPECT_EQ(multiplayer_result->expectations_passed, 2U);
    EXPECT_EQ(camera_result->clients_accepted, 1U);
    EXPECT_EQ(camera_result->expectations_passed, 1U);
    EXPECT_EQ(boundary_result->clients_accepted, 2U);
    EXPECT_EQ(boundary_result->expectations_passed, 4U);
}

TEST(CoreLangScenario, RejectsInvalidScriptAfterPrivateCandidateMutation)
{
    auto const invalid = shared::parseScenarioSource(
        "s5_flight_invalid.core",
        readScenario("s5_flight_invalid.core"),
        scenarioLimits()
    );
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);

    auto const valid = shared::parseScenarioSource(
        "s5_flight_camera.core",
        readScenario("s5_flight_camera.core"),
        scenarioLimits()
    );
    ASSERT_TRUE(valid.has_value()) << valid.error().message;
    EXPECT_EQ(valid->actors().size(), 1U);
    EXPECT_EQ(valid->operations().size(), 3U);
}

TEST(CoreLangScenario, RejectsUnknownMixedLimitedAndCancelledSourcesBeforePublishingAPlan)
{
    static constexpr std::string_view VALID_PREFIX = R"(@version("0.0.3")
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
)";
    static constexpr std::string_view LIMITED = R"(@version("0.0.3")
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
    wait(2u64)
}
)";
    static constexpr std::string_view MAX_TICK_THEN_INPUT = R"(@version("0.0.3")
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
    wait(18446744073709551615u64)
    moveXYZ("alice", 0i8, 0i8, 0i8)
}
)";

    EXPECT_EQ(
        shared::parseScenarioSource("unknown.core", "script 1;\n", scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader
    );
    EXPECT_EQ(
        shared::parseScenarioSource("unknown-version.mcscenario", "scenario 10\n", scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader
    );
    EXPECT_EQ(
        shared::parseScenarioSource(
            "mixed.core", "@version(\"0.0.3\")\nscenario 1\n", scenarioLimits()
        ).error().code,
        shared::ScenarioDiagnosticCode::CoreLangCompileFailure
    );

    auto limited = scenarioLimits();
    limited.max_total_ticks = 1U;
    EXPECT_EQ(
        shared::parseScenarioSource("limited.core", LIMITED, limited).error().code,
        shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure
    );
    limited.max_total_ticks = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(
        shared::parseScenarioSource("max-tick.core", MAX_TICK_THEN_INPUT, limited).error().code,
        shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure
    );

    Cancelled const cancelled;
    EXPECT_EQ(
        shared::parseScenarioSource("cancelled.core", VALID_PREFIX, scenarioLimits(), &cancelled).error().code,
        shared::ScenarioDiagnosticCode::Cancelled
    );
}

TEST(CoreLangScenario, PreservesLegacyScenarioFrontendAndMigratedFlatReplay)
{
    auto const legacy = shared::parseScenarioSource(
        "canonical_sample.mcscenario",
        readScenario("canonical_sample.mcscenario"),
        scenarioLimits()
    );
    auto const migrated = shared::parseScenarioSource(
        "canonical_sample.core",
        readScenario("canonical_sample.core"),
        scenarioLimits()
    );

    ASSERT_TRUE(legacy.has_value()) << legacy.error().message;
    ASSERT_TRUE(migrated.has_value()) << migrated.error().message;
    ASSERT_EQ(legacy->actors().size(), 1U);
    ASSERT_EQ(migrated->actors().size(), 1U);
    EXPECT_EQ(legacy->actors()[0].x, migrated->actors()[0].x);
    EXPECT_EQ(legacy->actors()[0].y, migrated->actors()[0].y);
    EXPECT_EQ(legacy->actors()[0].z, migrated->actors()[0].z);
    EXPECT_EQ(legacy->totalTicks(), migrated->totalTicks());
    EXPECT_EQ(legacy->evidenceCount(), migrated->evidenceCount());
}

} // namespace
