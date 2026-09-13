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
