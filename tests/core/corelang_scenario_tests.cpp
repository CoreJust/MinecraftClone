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

[[nodiscard]] shared::ScenarioLimits scenarioLimits()
{
    return {
        .max_source_bytes = 4'096, .max_statements = 64, .max_actors = 4, .max_total_ticks = 100,
        .max_operations = 32, .max_evidence = 8,
    };
}

[[nodiscard]] std::string readScenario(std::string_view const filename)
{
    auto const path = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path()
        / "scenarios" / std::filesystem::path{std::string{filename}};
    auto input = std::ifstream{path,
        std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class Cancelled final : public shared::ScenarioCancellation {
public:
    [[nodiscard]] bool isCancellationRequested() const noexcept override { return true; }
};

void expectEquivalentPlan(shared::ScenarioPlan const& legacy, shared::ScenarioPlan const& core)
{
    EXPECT_EQ(core.version(), legacy.version());
    EXPECT_EQ(core.profile(), legacy.profile());
    EXPECT_EQ(core.seed(), legacy.seed());
    EXPECT_EQ(core.actors().size(), legacy.actors().size());
    EXPECT_EQ(core.operations().size(), legacy.operations().size());
    EXPECT_EQ(core.totalTicks(), legacy.totalTicks());
    EXPECT_EQ(core.evidenceCount(), legacy.evidenceCount());
    for (uint64_t index = 0U; index < core.actors().size(); ++index) {
        EXPECT_EQ(core.actors()[index].id, legacy.actors()[index].id);
        EXPECT_EQ(core.actors()[index].name, legacy.actors()[index].name);
        EXPECT_EQ(core.actors()[index].character, legacy.actors()[index].character);
        EXPECT_EQ(core.actors()[index].x, legacy.actors()[index].x);
        EXPECT_EQ(core.actors()[index].y, legacy.actors()[index].y);
    }
    for (uint64_t index = 0U; index < core.operations().size(); ++index) {
        auto const& first = legacy.operations()[index];
        auto const& second = core.operations()[index];
        EXPECT_EQ(second.boundary, first.boundary);
        EXPECT_EQ(second.data.index(), first.data.index());
        if (auto const* input = std::get_if<shared::ScenarioInputOperation>(&first.data)) {
            auto const* const core_input = std::get_if<shared::ScenarioInputOperation>(&second.data);
            ASSERT_NE(core_input, nullptr);
            EXPECT_EQ(core_input->actor, input->actor);
            EXPECT_EQ(core_input->x, input->x);
            EXPECT_EQ(core_input->y, input->y);
            EXPECT_EQ(core_input->effective_boundary, input->effective_boundary);
        } else if (auto const* wait = std::get_if<shared::ScenarioWaitOperation>(&first.data)) {
            auto const* const core_wait = std::get_if<shared::ScenarioWaitOperation>(&second.data);
            ASSERT_NE(core_wait, nullptr);
            EXPECT_EQ(core_wait->ticks, wait->ticks);
        } else {
            auto const* const expected = std::get_if<shared::ScenarioExpectPositionOperation>(&first.data);
            auto const* const core_expected = std::get_if<shared::ScenarioExpectPositionOperation>(&second.data);
            ASSERT_NE(expected, nullptr);
            ASSERT_NE(core_expected, nullptr);
            EXPECT_EQ(core_expected->actor, expected->actor);
            EXPECT_EQ(core_expected->x, expected->x);
            EXPECT_EQ(core_expected->y, expected->y);
        }
    }
}

TEST(CoreLangScenario, LowersCanonicalCoreSourceToTheLegacyPlanAndRunnerCounters)
{
    auto const legacy = shared::parseScenarioSource(
        "canonical_sample.mcscenario", readScenario("canonical_sample.mcscenario"), scenarioLimits()
    );
    auto const core = shared::parseScenarioSource("canonical_sample.core", readScenario("canonical_sample.core"), scenarioLimits());
    ASSERT_TRUE(legacy.has_value()) << legacy.error().message;
    ASSERT_TRUE(core.has_value()) << core.error().message;
    expectEquivalentPlan(*legacy, *core);

    auto const legacy_result = acceptance::runScenario(*legacy, {
        .deadline = std::chrono::seconds{5}, .network_poll_interval = std::chrono::milliseconds{1},
    });
    auto const core_result = acceptance::runScenario(*core, {
        .deadline = std::chrono::seconds{5}, .network_poll_interval = std::chrono::milliseconds{1},
    });
    ASSERT_TRUE(legacy_result.has_value()) << legacy_result.error();
    ASSERT_TRUE(core_result.has_value()) << core_result.error();
    EXPECT_EQ(core_result->passed, legacy_result->passed);
    EXPECT_EQ(core_result->ticks, legacy_result->ticks);
    EXPECT_EQ(core_result->clients_accepted, legacy_result->clients_accepted);
    EXPECT_EQ(core_result->inputs_sent, legacy_result->inputs_sent);
    EXPECT_EQ(core_result->expectations_passed, legacy_result->expectations_passed);
}

TEST(CoreLangScenario, RejectsUnknownMixedLimitedAndCancelledSourcesBeforeAPlanExists)
{
    static constexpr std::string_view UNKNOWN = "script 1;\n";
    static constexpr std::string_view UNKNOWN_LEGACY_VERSION = "scenario 10\n";
    static constexpr std::string_view NON_ASCII_CHARACTER = R"(@version("0.0.1")
@use MinecraftScenario
profile("flat2d-v1")
seed(1u64)
player("alice", 'ŀ', 4u8, 4u8)
)";
    static constexpr std::string_view MAX_TICK_THEN_INPUT = R"(@version("0.0.1")
@use MinecraftScenario
profile("flat2d-v1")
seed(1u64)
player("alice", '@', 4u8, 4u8)
wait(18446744073709551615u64)
input("alice", 0i8, 0i8)
)";
    static constexpr std::string_view MIXED = "@version(\"0.0.1\")\nscenario 1\n";
    static constexpr std::string_view LIMITED = R"(@version("0.0.1")
@use MinecraftScenario
profile("flat2d-v1")
seed(1u64)
player("alice", '@', 4u8, 4u8)
wait(2u64)
)";
    EXPECT_EQ(shared::parseScenarioSource("unknown.core", UNKNOWN, scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader);
    EXPECT_EQ(shared::parseScenarioSource("unknown-version.mcscenario", UNKNOWN_LEGACY_VERSION, scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader);
    EXPECT_EQ(shared::parseScenarioSource("non-ascii.core", NON_ASCII_CHARACTER, scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
    EXPECT_EQ(shared::parseScenarioSource("mixed.core", MIXED, scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::CoreLangCompileFailure);
    auto limits = scenarioLimits();
    limits.max_total_ticks = 1;
    EXPECT_EQ(shared::parseScenarioSource("limited.core", LIMITED, limits).error().code,
        shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
    limits.max_total_ticks = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(shared::parseScenarioSource("max-tick.core", MAX_TICK_THEN_INPUT, limits).error().code,
        shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
    auto const cancelled = Cancelled{};
    EXPECT_EQ(shared::parseScenarioSource("cancelled.core", LIMITED, scenarioLimits(), &cancelled).error().code,
        shared::ScenarioDiagnosticCode::Cancelled);

    auto directives = std::string{"@version(\"0.0.1\")\n"};
    for (uint32_t index = 0U; index < 300U; ++index) directives += "@use MinecraftScenario\n";
    auto directive_limits = scenarioLimits();
    directive_limits.max_source_bytes = 16'384U;
    directive_limits.max_statements = 64U;
    EXPECT_EQ(shared::parseScenarioSource("too-many-uses.core", directives, directive_limits).error().code,
        shared::ScenarioDiagnosticCode::CoreLangCompileFailure);
}

} // namespace
