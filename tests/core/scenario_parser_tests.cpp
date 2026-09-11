#include <shared/scenario/Scenario.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

namespace {

[[nodiscard]]
shared::ScenarioLimits scenarioLimits() {
    return shared::ScenarioLimits{
        .max_source_bytes = 4'096,
        .max_statements = 64,
        .max_actors = 8,
        .max_total_ticks = 100,
        .max_operations = 32,
        .max_evidence = 8,
    };
}

void expectDiagnostic(
    std::expected<shared::ScenarioPlan, shared::ScenarioDiagnostic> const& result,
    shared::ScenarioDiagnosticCode const code,
    uint32_t const line,
    uint32_t const column
) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, code);
    EXPECT_EQ(result.error().location.line, line);
    EXPECT_EQ(result.error().location.column, column);
    EXPECT_EQ(result.error().filename, "sample.scenario");
    EXPECT_EQ(result.error().message.empty(), false);
}

[[nodiscard]]
std::filesystem::path scenarioRepositoryRoot() {
    return std::filesystem::path{ __FILE__ }.parent_path().parent_path().parent_path();
}

struct DiagnosticNameCase final {
    shared::ScenarioDiagnosticCode code;
    std::string_view name;
};

} // namespace

TEST(ScenarioParserTest, ParsesTypedPlanWithSourceOrderAndAuthoritativeBoundaries) {
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
input alice 1 0
wait 5
input alice 0 0
expect player alice position 9 4
end
)";
    auto const result = shared::parseScenario("sample.scenario", SOURCE, scenarioLimits());
    ASSERT_TRUE(result.has_value());
    shared::ScenarioPlan const& plan = *result;
    EXPECT_EQ(plan.version(), 1u);
    EXPECT_EQ(plan.profile(), shared::ScenarioProfile::Flat2dV1);
    EXPECT_EQ(shared::scenarioProfileName(plan.profile()), "flat2d-v1");
    EXPECT_EQ(plan.seed(), 42u);
    ASSERT_EQ(plan.actors().size(), 1u);
    EXPECT_EQ(plan.actors()[0].id, 0u);
    EXPECT_EQ(plan.actors()[0].name, "alice");
    EXPECT_EQ(plan.actors()[0].character, '@');
    EXPECT_EQ(plan.actors()[0].x, 4u);
    EXPECT_EQ(plan.actors()[0].y, 4u);
    EXPECT_EQ(plan.actors()[0].location.line, 4u);
    EXPECT_EQ(plan.actors()[0].location.column, 1u);
    ASSERT_EQ(plan.operations().size(), 4u);
    EXPECT_EQ(plan.operations()[0].location.line, 6u);
    EXPECT_EQ(plan.operations()[0].boundary, 0u);
    auto const* const first_input = std::get_if<shared::ScenarioInputOperation>(&plan.operations()[0].data);
    ASSERT_NE(first_input, nullptr);
    EXPECT_EQ(first_input->actor, 0u);
    EXPECT_EQ(first_input->x, 1);
    EXPECT_EQ(first_input->y, 0);
    EXPECT_EQ(first_input->effective_boundary, 1u);
    EXPECT_EQ(plan.operations()[1].boundary, 0u);
    auto const* const wait = std::get_if<shared::ScenarioWaitOperation>(&plan.operations()[1].data);
    ASSERT_NE(wait, nullptr);
    EXPECT_EQ(wait->ticks, 5u);
    EXPECT_EQ(plan.operations()[2].boundary, 5u);
    auto const* const second_input = std::get_if<shared::ScenarioInputOperation>(&plan.operations()[2].data);
    ASSERT_NE(second_input, nullptr);
    EXPECT_EQ(second_input->actor, 0u);
    EXPECT_EQ(second_input->effective_boundary, 6u);
    EXPECT_EQ(plan.operations()[3].boundary, 5u);
    auto const* const expectation = std::get_if<shared::ScenarioExpectPositionOperation>(&plan.operations()[3].data);
    ASSERT_NE(expectation, nullptr);
    EXPECT_EQ(expectation->actor, 0u);
    EXPECT_EQ(expectation->x, 9u);
    EXPECT_EQ(expectation->y, 4u);
    EXPECT_EQ(plan.totalTicks(), 5u);
    EXPECT_EQ(plan.evidenceCount(), 1u);
}

TEST(ScenarioParserTest, PreservesFlat3dCameraPoseAndCameraRelativeReplayInput)
{
    static constexpr std::string_view SOURCE = R"(scenario 1
profile flat3d-v1
seed 7
player alice character "@" at 4 4 0 orientation 0 12 -30
player bob character "#" at 10 10 0 orientation 90 0 0
begin
input alice camera 0 1
input bob camera 0 1
wait 2
expect player alice position 4 6 0
expect player bob position 12 10 0
end
)";
    auto const result = shared::parseScenario("sample.scenario", SOURCE, scenarioLimits());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->profile(), shared::ScenarioProfile::Flat3dV1);
    EXPECT_EQ(shared::scenarioProfileName(result->profile()), "flat3d-v1");
    ASSERT_EQ(result->actors().size(), 2U);
    EXPECT_EQ(result->actors()[0].z, 0U);
    EXPECT_EQ(result->actors()[0].yaw_degrees, 0);
    EXPECT_EQ(result->actors()[0].pitch_degrees, 12);
    EXPECT_EQ(result->actors()[0].roll_degrees, -30);
    EXPECT_EQ(result->actors()[1].yaw_degrees, 90);
    ASSERT_EQ(result->operations().size(), 5U);
    auto const* const alice_input = std::get_if<shared::ScenarioCameraInputOperation>(&result->operations()[0].data);
    ASSERT_NE(alice_input, nullptr);
    EXPECT_EQ(alice_input->strafe, 0);
    EXPECT_EQ(alice_input->forward, 1);
    shared::Direction const forward = shared::scenarioCameraRelativeDirection(0, 0, 1);
    EXPECT_EQ(forward.x, 0U);
    EXPECT_EQ(forward.y, 1U);
    shared::Direction const right_facing_forward = shared::scenarioCameraRelativeDirection(90, 0, 1);
    EXPECT_EQ(right_facing_forward.x, 1U);
    EXPECT_EQ(right_facing_forward.y, 0U);
    shared::Direction const diagonal_heading_forward = shared::scenarioCameraRelativeDirection(135, 0, 1);
    EXPECT_EQ(diagonal_heading_forward.x, 1U);
    EXPECT_EQ(diagonal_heading_forward.y, 0U);
    shared::Direction const diagonal_tie = shared::scenarioCameraRelativeDirection(0, 1, 1);
    EXPECT_EQ(diagonal_tie.x, 1U);
    EXPECT_EQ(diagonal_tie.y, 0U);
    EXPECT_EQ(result->operations()[4].boundary, 2U);
    auto const* const expectation = std::get_if<shared::ScenarioExpectPositionOperation>(&result->operations()[4].data);
    ASSERT_NE(expectation, nullptr);
    EXPECT_EQ(expectation->z, 0U);
    std::string alternate_source{ SOURCE };
    alternate_source.replace(alternate_source.find("orientation 0 12 -30"), 20U, "orientation 1 12 -30");
    auto const alternate = shared::parseScenario("alternate.scenario", alternate_source, scenarioLimits());
    ASSERT_TRUE(alternate.has_value()) << alternate.error().message;
    EXPECT_NE(shared::scenarioReplayId(*result), shared::scenarioReplayId(*alternate));
}

TEST(ScenarioParserTest, RejectsNonFlat3dVerticalCoordinatesAndInvalidCameraAngles)
{
    static constexpr std::string_view VERTICAL_PLAYER = R"(scenario 1
profile flat3d-v1
seed 7
player alice character "@" at 4 4 1 orientation 0 0 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", VERTICAL_PLAYER, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidRange,
        4,
        35
    );
    static constexpr std::string_view INVALID_PITCH = R"(scenario 1
profile flat3d-v1
seed 7
player alice character "@" at 4 4 0 orientation 0 90 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", INVALID_PITCH, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidRange,
        4,
        51
    );
}

TEST(ScenarioParserTest, SupportsCommentsQuotedCharactersAndAsciiIdentifiersWithoutMutatingSource) {
    std::string const source = R"(# setup
scenario 1 # current schema
profile flat2d-v1
seed 7
player player_id-1 character "#" at 4 4 # literal character is not a comment
begin
input player_id-1 -1 0
wait 1
expect player player_id-1 position 3 4
end # complete
)";
    std::string const original = source;
    auto const result = shared::parseScenario("sample.scenario", source, scenarioLimits());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(source, original);
    ASSERT_EQ(result->actors().size(), 1u);
    EXPECT_EQ(result->actors()[0].name, "player_id-1");
    EXPECT_EQ(result->actors()[0].character, '#');
}

TEST(ScenarioParserTest, OwnsPlanAndDiagnosticTextAfterCallerStorageChanges) {
    std::string filename = "temporary.scenario";
    std::string source = R"(scenario 1
profile flat2d-v1
seed 7
player alice character "@" at 4 4
begin
input alice 1 0
end
)";
    auto const plan = shared::parseScenario(filename, source, scenarioLimits());
    ASSERT_TRUE(plan.has_value());
    filename.assign("changed");
    source.assign("changed");
    ASSERT_EQ(plan->actors().size(), 1u);
    EXPECT_EQ(plan->actors()[0].name, "alice");
    ASSERT_EQ(plan->operations().size(), 1u);
    EXPECT_NE(std::get_if<shared::ScenarioInputOperation>(&plan->operations()[0].data), nullptr);

    filename = "invalid.scenario";
    source = "scenario 2\n";
    auto const invalid = shared::parseScenario(filename, source, scenarioLimits());
    ASSERT_FALSE(invalid.has_value());
    filename.assign("changed");
    source.assign("changed");
    EXPECT_EQ(invalid.error().filename, "invalid.scenario");
    EXPECT_EQ(invalid.error().code, shared::ScenarioDiagnosticCode::UnsupportedVersion);
    EXPECT_FALSE(invalid.error().message.empty());
}

TEST(ScenarioParserTest, ParsesEveryCheckedInScenarioExample) {
    static constexpr std::array<std::string_view, 4> EXAMPLES{
        "camera_two_client.mcscenario",
        "canonical_sample.mcscenario",
        "comment_and_boundary.mcscenario",
        "two_players.mcscenario",
    };
    std::filesystem::path const scenario_directory = scenarioRepositoryRoot() / "scenarios";
    for (std::string_view const example : EXAMPLES) {
        std::filesystem::path const example_path = scenario_directory / example;
        std::ifstream file{ example_path, std::ios::binary };
        ASSERT_TRUE(file.is_open()) << example_path;
        std::string const source{
            std::istreambuf_iterator<char>{ file },
            std::istreambuf_iterator<char>{},
        };
        auto const result = shared::parseScenario(example_path.string(), source, scenarioLimits());
        ASSERT_TRUE(result.has_value()) << result.error().message;
    }
}

TEST(ScenarioParserTest, KeepsStableNamesForEveryDiagnosticCode) {
    static constexpr std::array<DiagnosticNameCase, 18> DIAGNOSTICS{
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::InvalidLimits, "invalid-limits" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::SourceTooLarge, "source-too-large" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::StatementLimitExceeded, "statement-limit-exceeded" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::ActorLimitExceeded, "actor-limit-exceeded" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::TickLimitExceeded, "tick-limit-exceeded" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::OperationLimitExceeded, "operation-limit-exceeded" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::EvidenceLimitExceeded, "evidence-limit-exceeded" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::MalformedSyntax, "malformed-syntax" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::UnsupportedVersion, "unsupported-version" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::UnsupportedProfile, "unsupported-profile" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::DuplicateActor, "duplicate-actor" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::UnknownActor, "unknown-actor" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::UnsupportedCommand, "unsupported-command" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::IntegerOverflow, "integer-overflow" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::InvalidInteger, "invalid-integer" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::InvalidRange, "invalid-range" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::InvalidCharacter, "invalid-character" },
        DiagnosticNameCase{ shared::ScenarioDiagnosticCode::MissingPlayer, "missing-player" },
    };
    for (DiagnosticNameCase const& diagnostic : DIAGNOSTICS) {
        EXPECT_EQ(shared::scenarioDiagnosticCodeName(diagnostic.code), diagnostic.name);
    }
}

TEST(ScenarioParserTest, ReportsGrammarReferenceProfileAndCommandErrorsAtStableLocations) {
    static constexpr std::string_view QUOTED_PROFILE = R"(scenario 1
profile "flat2d-v1"
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", QUOTED_PROFILE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MalformedSyntax,
        2,
        9
    );

    static constexpr std::string_view UNSUPPORTED_PROFILE = R"(scenario 1
profile cube3d-v1
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", UNSUPPORTED_PROFILE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::UnsupportedProfile,
        2,
        9
    );

    static constexpr std::string_view DUPLICATE_ACTOR = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
player alice character "#" at 2 2
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", DUPLICATE_ACTOR, scenarioLimits()),
        shared::ScenarioDiagnosticCode::DuplicateActor,
        5,
        8
    );

    static constexpr std::string_view QUOTED_ACTOR = R"(scenario 1
profile flat2d-v1
seed 1
player "alice" character "@" at 0 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", QUOTED_ACTOR, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MalformedSyntax,
        4,
        8
    );

    static constexpr std::string_view DUPLICATE_CHARACTER = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
player bob character "@" at 2 2
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", DUPLICATE_CHARACTER, scenarioLimits()),
        shared::ScenarioDiagnosticCode::DuplicateActor,
        5,
        22
    );

    static constexpr std::string_view UNKNOWN_ACTOR = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
input bob 0 0
end
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", UNKNOWN_ACTOR, scenarioLimits()),
        shared::ScenarioDiagnosticCode::UnknownActor,
        6,
        7
    );

    static constexpr std::string_view UNSUPPORTED_COMMAND = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
jump alice
end
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", UNSUPPORTED_COMMAND, scenarioLimits()),
        shared::ScenarioDiagnosticCode::UnsupportedCommand,
        6,
        1
    );

    static constexpr std::string_view MALFORMED = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin extra
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", MALFORMED, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MalformedSyntax,
        5,
        1
    );

    static constexpr std::string_view MISSING_PLAYER = R"(scenario 1
profile flat2d-v1
seed 1
begin
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", MISSING_PLAYER, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MissingPlayer,
        4,
        1
    );

    static constexpr std::string_view INCOMPLETE = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", INCOMPLETE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MalformedSyntax,
        6,
        1
    );

    static constexpr std::string_view UNCLOSED_STRING = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@ at 0 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", UNCLOSED_STRING, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MalformedSyntax,
        4,
        24
    );

    static constexpr std::string_view CR_ONLY_INCOMPLETE =
        "scenario 1\rprofile flat2d-v1\rseed 1\rplayer alice character \"@\" at 0 0\rbegin\r";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", CR_ONLY_INCOMPLETE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::MalformedSyntax,
        6,
        1
    );
}

TEST(ScenarioParserTest, RejectsUnsupportedValuesOverflowNonFiniteWordsAndInvalidRanges) {
    static constexpr std::string_view UNSUPPORTED_VERSION = R"(scenario 2
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", UNSUPPORTED_VERSION, scenarioLimits()),
        shared::ScenarioDiagnosticCode::UnsupportedVersion,
        1,
        10
    );

    static constexpr std::string_view INTEGER_OVERFLOW_SOURCE = R"(scenario 1
profile flat2d-v1
seed 18446744073709551616
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", INTEGER_OVERFLOW_SOURCE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::IntegerOverflow,
        3,
        6
    );

    static constexpr std::string_view NONFINITE = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
wait nan
end
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", NONFINITE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidInteger,
        6,
        6
    );

    static constexpr std::string_view LEADING_ZERO_WAIT = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
wait 01
end
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", LEADING_ZERO_WAIT, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidInteger,
        6,
        6
    );

    static constexpr std::string_view INVALID_RANGE = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "!" at 32 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", INVALID_RANGE, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidCharacter,
        4,
        24
    );

    static constexpr std::string_view INVALID_POSITION = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 32 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", INVALID_POSITION, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidRange,
        4,
        31
    );

    static constexpr std::string_view NEGATIVE_POSITION = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at -1 0
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", NEGATIVE_POSITION, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidRange,
        4,
        31
    );

    static constexpr std::string_view INVALID_INPUT = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
input alice 2 0
end
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", INVALID_INPUT, scenarioLimits()),
        shared::ScenarioDiagnosticCode::InvalidRange,
        6,
        13
    );

    static constexpr std::string_view CONFLICTING_PLACEMENT = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 4 4
player bob character "#" at 5 5
begin
end
)";
    auto const conflicting = shared::parseScenario("sample.scenario", CONFLICTING_PLACEMENT, scenarioLimits());
    expectDiagnostic(conflicting, shared::ScenarioDiagnosticCode::InvalidRange, 5, 1);
    EXPECT_NE(conflicting.error().message.find("alice"), std::string::npos);
    EXPECT_NE(conflicting.error().message.find("bob"), std::string::npos);

    static constexpr std::string_view SEPARATED_PLACEMENT = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 4 4
player bob character "#" at 6 5
begin
end
)";
    EXPECT_TRUE(shared::parseScenario("sample.scenario", SEPARATED_PLACEMENT, scenarioLimits()).has_value());
}

TEST(ScenarioParserTest, EnforcesHostSuppliedLimitsIncludingSameBoundaryOperationsAndWaitEdge) {
    static constexpr std::string_view COMPLETE = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
input alice 0 0
wait 5
expect player alice position 0 0
end
)";
    shared::ScenarioLimits limits = scenarioLimits();
    limits.max_source_bytes = 1;
    expectDiagnostic(
        shared::parseScenario("sample.scenario", COMPLETE, limits),
        shared::ScenarioDiagnosticCode::SourceTooLarge,
        1,
        1
    );

    limits = scenarioLimits();
    limits.max_statements = 2;
    expectDiagnostic(
        shared::parseScenario("sample.scenario", COMPLETE, limits),
        shared::ScenarioDiagnosticCode::StatementLimitExceeded,
        3,
        1
    );

    static constexpr std::string_view TWO_ACTORS = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
player bob character "#" at 2 2
begin
end
)";
    limits = scenarioLimits();
    limits.max_actors = 1;
    expectDiagnostic(
        shared::parseScenario("sample.scenario", TWO_ACTORS, limits),
        shared::ScenarioDiagnosticCode::ActorLimitExceeded,
        5,
        1
    );

    limits = scenarioLimits();
    limits.max_operations = 1;
    expectDiagnostic(
        shared::parseScenario("sample.scenario", COMPLETE, limits),
        shared::ScenarioDiagnosticCode::OperationLimitExceeded,
        7,
        1
    );

    limits = scenarioLimits();
    limits.max_total_ticks = 4;
    expectDiagnostic(
        shared::parseScenario("sample.scenario", COMPLETE, limits),
        shared::ScenarioDiagnosticCode::TickLimitExceeded,
        7,
        1
    );

    limits = scenarioLimits();
    limits.max_evidence = 0;
    expectDiagnostic(
        shared::parseScenario("sample.scenario", COMPLETE, limits),
        shared::ScenarioDiagnosticCode::InvalidLimits,
        1,
        1
    );

    limits = scenarioLimits();
    limits.max_evidence = 1;
    limits.max_total_ticks = 5;
    auto const at_wait_limit = shared::parseScenario("sample.scenario", COMPLETE, limits);
    ASSERT_TRUE(at_wait_limit.has_value());
    EXPECT_EQ(at_wait_limit->totalTicks(), 5u);

    static constexpr std::string_view TWO_EVIDENCE = R"(scenario 1
profile flat2d-v1
seed 1
player alice character "@" at 0 0
begin
expect player alice position 0 0
expect player alice position 0 0
end
)";
    expectDiagnostic(
        shared::parseScenario("sample.scenario", TWO_EVIDENCE, limits),
        shared::ScenarioDiagnosticCode::EvidenceLimitExceeded,
        7,
        1
    );
}
