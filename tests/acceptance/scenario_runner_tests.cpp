#include <shared/scenario/Scenario.hpp>

#include <acceptance/ScenarioRunner.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]]
shared::ScenarioLimits scenarioLimits()
{
    return shared::ScenarioLimits{
        .max_source_bytes = 4'096,
        .max_statements = 64,
        .max_actors = 8,
        .max_total_ticks = 100,
        .max_operations = 32,
        .max_evidence = 8,
    };
}

[[nodiscard]]
std::expected<shared::ScenarioPlan, shared::ScenarioDiagnostic> parsePlan(std::string_view const source)
{
    return shared::parseScenario("scenario-runner-test", source, scenarioLimits());
}

TEST(ScenarioRunner, ExecutesTheAuthoritativeFiveTickSampleOverRealEnet)
{
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
input alice 1 0
wait 5
expect player alice position 9 4
end
)";
    auto parsed = parsePlan(SOURCE);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    shared::ScenarioPlan const& plan = *parsed;

    auto const result = acceptance::runScenario(plan, {
        .deadline = std::chrono::seconds{ 5 },
        .network_poll_interval = std::chrono::milliseconds{ 1 },
    });

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->passed);
    EXPECT_EQ(result->clients_requested, 1U);
    EXPECT_EQ(result->clients_accepted, 1U);
    EXPECT_EQ(result->ticks, 5U);
    EXPECT_EQ(result->last_effective_tick, 1U);
    EXPECT_EQ(result->inputs_sent, 5U);
    EXPECT_EQ(result->expectations_passed, 1U);
    EXPECT_GE(result->server_events_processed, 7U);
}

TEST(ScenarioRunner, PreservesNegativeDirectionsAtTheWireBoundary)
{
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
input alice -1 0
wait 1
expect player alice position 3 4
end
)";
    auto parsed = parsePlan(SOURCE);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    shared::ScenarioPlan const& plan = *parsed;

    auto const result = acceptance::runScenario(plan, {
        .deadline = std::chrono::seconds{ 5 },
        .network_poll_interval = std::chrono::milliseconds{ 1 },
    });

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->inputs_sent, 1U);
    EXPECT_EQ(result->last_effective_tick, 1U);
}

TEST(ScenarioRunner, AppliesTwoActorsInputsAtTheSameTickBoundary)
{
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
player bob character "#" at 10 10
begin
input alice 1 0
input bob 0 -1
wait 2
expect player alice position 6 4
expect player bob position 10 8
end
)";
    auto parsed = parsePlan(SOURCE);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto const result = acceptance::runScenario(*parsed, {
        .deadline = std::chrono::seconds{ 5 },
        .network_poll_interval = std::chrono::milliseconds{ 1 },
    });

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->passed);
    EXPECT_EQ(result->clients_requested, 2U);
    EXPECT_EQ(result->clients_accepted, 2U);
    EXPECT_EQ(result->ticks, 2U);
    EXPECT_EQ(result->inputs_sent, 4U);
    EXPECT_EQ(result->expectations_passed, 2U);
}

TEST(ScenarioRunner, RejectsInvalidRuntimeLimitsWithoutWaitingForNetwork)
{
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
end
)";
    auto parsed = parsePlan(SOURCE);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    shared::ScenarioPlan const& plan = *parsed;

    auto const result = acceptance::runScenario(plan, {
        .deadline = std::chrono::seconds::zero(),
        .network_poll_interval = std::chrono::milliseconds{ 1 },
    });

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("positive monotonic limits"), std::string::npos);
}

TEST(ScenarioRunner, CapsNetworkPollsToTheRemainingDeadline)
{
    EXPECT_EQ(
        acceptance::detail::boundedNetworkPollTimeout(
            std::chrono::hours{ 1 },
            std::chrono::milliseconds{ 250 }
        ),
        std::chrono::milliseconds{ 250 }
    );
    EXPECT_EQ(
        acceptance::detail::boundedNetworkPollTimeout(
            std::chrono::milliseconds{ 1 },
            std::chrono::milliseconds{ 250 }
        ),
        std::chrono::milliseconds{ 1 }
    );
}

TEST(ScenarioRunner, RejectsActorCountsBeyondServerCapacityBeforeConnecting)
{
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat2d-v1
seed 42
player alpha character "@" at 0 0
player bravo character "#" at 2 0
player charlie character "$" at 4 0
player delta character "%" at 6 0
player echo character "&" at 8 0
begin
end
)";
    auto parsed = parsePlan(SOURCE);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    shared::ScenarioPlan const& plan = *parsed;

    auto const result = acceptance::runScenario(plan, {
        .deadline = std::chrono::seconds{ 5 },
        .network_poll_interval = std::chrono::milliseconds{ 1 },
    });

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("at most 4 actors"), std::string::npos);
}

} // namespace
