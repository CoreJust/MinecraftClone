#include <shared/corelang/ExecutionTypes.hpp>
#include <shared/scenario/Scenario.hpp>

#include <acceptance/EvidenceJson.hpp>
#include <acceptance/ScenarioRunner.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

class CancelAfterChecks final : public shared::ScenarioCancellation {
public:
    explicit CancelAfterChecks(uint32_t const cancel_at_check)
        : m_cancel_at_check(cancel_at_check)
    {
    }

    [[nodiscard]] bool isCancellationRequested() const noexcept override
    {
        return ++m_checks >= m_cancel_at_check;
    }

    [[nodiscard]] uint32_t checks() const noexcept { return m_checks; }

private:
    uint32_t m_cancel_at_check;
    mutable uint32_t m_checks{0U};
};

class CountCancellationChecks final : public shared::ScenarioCancellation {
public:
    [[nodiscard]] bool isCancellationRequested() const noexcept override
    {
        ++m_checks;
        return false;
    }

    [[nodiscard]] uint32_t checks() const noexcept { return m_checks; }

private:
    mutable uint32_t m_checks{0U};
};

TEST(CoreLangScenario, LowersBare015MultiClientFlightScriptIntoTypedNativeOperations)
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
    ASSERT_TRUE(result->executionReceipt().has_value());
    EXPECT_EQ(result->executionReceipt()->requested_backend, shared::corelang::Backend::Interpreter);
    EXPECT_EQ(result->executionReceipt()->actual_backend, shared::corelang::Backend::Interpreter);
    EXPECT_FALSE(shared::scenarioReplayId(*result).empty());
}

TEST(CoreLangScenario, SelectedBackendsReportActualExecutionAndMatchInterpreterPlan)
{
    std::string const source = readScenario("s5_flight_camera.core");
    auto const interpreter = shared::parseScenarioSource("backend-parity.core", source, scenarioLimits());

    ASSERT_TRUE(interpreter.has_value()) << interpreter.error().message;
    ASSERT_TRUE(interpreter->executionReceipt().has_value());
    EXPECT_EQ(
        interpreter->executionReceipt()->requested_backend,
        shared::corelang::Backend::Interpreter
    );
    EXPECT_EQ(
        interpreter->executionReceipt()->actual_backend,
        shared::corelang::Backend::Interpreter
    );
    EXPECT_NE(interpreter->executionReceipt()->source_sha256, (std::array<uint8_t, 32U>{}));
    EXPECT_NE(interpreter->executionReceipt()->bytecode_sha256, (std::array<uint8_t, 32U>{}));

    for (shared::corelang::Backend const backend : {
             shared::corelang::Backend::Jit,
             shared::corelang::Backend::Aot,
         }) {
        auto const selected = shared::parseScenarioSource(
            "backend-parity.core",
            source,
            scenarioLimits(),
            shared::ScenarioExecutionOptions{.backend = backend}
        );
        if (!selected) {
            EXPECT_EQ(selected.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
            EXPECT_NE(selected.error().message.find("unavailable"), std::string::npos);
            continue;
        }

        ASSERT_TRUE(selected->executionReceipt().has_value());
        EXPECT_EQ(selected->executionReceipt()->requested_backend, backend);
        EXPECT_EQ(selected->executionReceipt()->actual_backend, backend);
        EXPECT_EQ(selected->executionReceipt()->source_sha256, interpreter->executionReceipt()->source_sha256);
        EXPECT_EQ(selected->executionReceipt()->bytecode_sha256, interpreter->executionReceipt()->bytecode_sha256);
        EXPECT_EQ(shared::scenarioReplayId(*selected), shared::scenarioReplayId(*interpreter));
        if (backend == shared::corelang::Backend::Aot) {
            EXPECT_TRUE(selected->executionReceipt()->native_image_sha256.has_value());
        }
    }
}

TEST(CoreLangScenario, BoundsBackendArtifactsAndPreservesFiniteLoopExecution)
{
    std::string const source = readScenario("s5_flight_camera.core");

    shared::ScenarioExecutionOptions artifact_limited{};
    artifact_limited.limits.max_bytecode_bytes = 1U;
    auto const oversized = shared::parseScenarioSource(
        "oversized-backend-artifact.core",
        source,
        scenarioLimits(),
        artifact_limited
    );
    ASSERT_FALSE(oversized.has_value());
    EXPECT_EQ(oversized.error().code, shared::ScenarioDiagnosticCode::CoreLangCompileFailure);
    EXPECT_NE(oversized.error().message.find("bytecode"), std::string::npos);

    shared::ScenarioExecutionOptions image_limited{};
    image_limited.backend = shared::corelang::Backend::Aot;
    image_limited.limits.max_native_image_bytes = 1U;
    auto const oversized_image = shared::parseScenarioSource(
        "oversized-native-image.core",
        source,
        scenarioLimits(),
        image_limited
    );
    ASSERT_FALSE(oversized_image.has_value());
    EXPECT_EQ(oversized_image.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
    EXPECT_TRUE(
        oversized_image.error().message.find("native-image-too-large") != std::string::npos
        || oversized_image.error().message.find("backend-unavailable") != std::string::npos
    );

    shared::ScenarioExecutionOptions instruction_limited{};
    instruction_limited.limits.max_instructions = 1U;
    auto const over_limit = shared::parseScenarioSource(
        "oversized-instruction-plan.core",
        source,
        scenarioLimits(),
        instruction_limited
    );
    ASSERT_FALSE(over_limit.has_value());
    EXPECT_EQ(over_limit.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);

    static constexpr std::string_view FINITE_LOOP_SOURCE = R"(@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
    for index in 0i32..2i32 {
        wait(1u64)
    }
}
)";
    auto const finite_loop = shared::parseScenarioSource(
        "finite-loop.core",
        FINITE_LOOP_SOURCE,
        scenarioLimits()
    );
    ASSERT_TRUE(finite_loop.has_value()) << finite_loop.error().message;
    EXPECT_EQ(finite_loop->totalTicks(), 2U);
    ASSERT_EQ(finite_loop->operations().size(), 2U);
}

TEST(CoreLangScenario, EnforcesCombinedBackendComparisonBudgets)
{
    using Backend = shared::corelang::Backend;
    using DiagnosticCode = shared::ScenarioDiagnosticCode;
    struct BudgetCase final {
        Backend backend;
        uint64_t max_execution_count;
        uint64_t max_instructions;
        std::optional<DiagnosticCode> expected_error;
    };
    static constexpr std::string_view SOURCE = R"core(@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
    wait(1u64)
}
)core";
    static constexpr std::array<BudgetCase, 24U> BUDGET_CASES{
        BudgetCase{Backend::Interpreter, 1U, 16U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Jit, 1U, 16U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Aot, 1U, 16U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Interpreter, 1U, 17U, std::nullopt},
        BudgetCase{Backend::Jit, 1U, 17U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Aot, 1U, 17U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Interpreter, 1U, 33U, std::nullopt},
        BudgetCase{Backend::Jit, 1U, 33U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Aot, 1U, 33U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Interpreter, 1U, 34U, std::nullopt},
        BudgetCase{Backend::Jit, 1U, 34U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Aot, 1U, 34U, DiagnosticCode::InvalidLimits},
        BudgetCase{Backend::Interpreter, 2U, 16U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Jit, 2U, 16U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Aot, 2U, 16U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Interpreter, 2U, 17U, std::nullopt},
        BudgetCase{Backend::Jit, 2U, 17U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Aot, 2U, 17U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Interpreter, 2U, 33U, std::nullopt},
        BudgetCase{Backend::Jit, 2U, 33U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Aot, 2U, 33U, DiagnosticCode::CoreLangRuntimeFailure},
        BudgetCase{Backend::Interpreter, 2U, 34U, std::nullopt},
        BudgetCase{Backend::Jit, 2U, 34U, std::nullopt},
        BudgetCase{Backend::Aot, 2U, 34U, std::nullopt},
    };

    for (BudgetCase const& budget_case : BUDGET_CASES) {
        SCOPED_TRACE(
            ::testing::Message{}
                << "backend=" << static_cast<uint32_t>(budget_case.backend)
                << ", execution_count=" << budget_case.max_execution_count
                << ", instructions=" << budget_case.max_instructions
        );
        shared::ScenarioExecutionOptions execution{.backend = budget_case.backend};
        execution.limits.max_execution_count = budget_case.max_execution_count;
        execution.limits.max_instructions = budget_case.max_instructions;

        auto const plan = shared::parseScenarioSource("budget.core", SOURCE, scenarioLimits(), execution);

        EXPECT_EQ(plan.has_value(), !budget_case.expected_error.has_value());
        if (budget_case.expected_error.has_value()) {
            if (plan) {
                continue;
            }
            EXPECT_EQ(plan.error().code, *budget_case.expected_error);
            std::string_view const error_name = *budget_case.expected_error == DiagnosticCode::InvalidLimits
                ? "invalid-configuration"
                : "instruction-limit-exceeded";
            EXPECT_NE(plan.error().message.find(error_name), std::string::npos);
            continue;
        }
        ASSERT_TRUE(plan.has_value()) << plan.error().message;
        EXPECT_EQ(plan->totalTicks(), 1U);
        ASSERT_TRUE(plan->executionReceipt().has_value());
        auto const& receipt = *plan->executionReceipt();
        EXPECT_EQ(receipt.requested_backend, budget_case.backend);
        EXPECT_EQ(receipt.actual_backend, budget_case.backend);
        EXPECT_EQ(receipt.native_image_sha256.has_value(), budget_case.backend == Backend::Aot);
        if (receipt.native_image_sha256.has_value()) {
            EXPECT_NE(*receipt.native_image_sha256, (std::array<uint8_t, 32U>{}));
        }
    }
}

TEST(CoreLangScenario, ForwardsCancellationIntoBoundedCompilation)
{
    CancelAfterChecks const cancellation{3U};
    auto const result = shared::parseScenarioSource(
        "compile-cancelled.core",
        readScenario("s5_flight_camera.core"),
        scenarioLimits(),
        shared::ScenarioExecutionOptions{},
        &cancellation
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, shared::ScenarioDiagnosticCode::Cancelled);
    EXPECT_EQ(cancellation.checks(), 3U);
}

TEST(CoreLangScenario, PreservesCoreLangCompilationDiagnosticSourceLocation)
{
    static constexpr std::string_view SOURCE = R"(@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    missingHost()
}
)";
    auto const result = shared::parseScenarioSource("bad-source-location.core", SOURCE, scenarioLimits());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, shared::ScenarioDiagnosticCode::CoreLangCompileFailure);
    EXPECT_EQ(result.error().location.line, 6U);
    EXPECT_EQ(result.error().location.column, 5U);
}

TEST(CoreLangScenario, ForwardsCancellationIntoBoundedExecution)
{
    std::string const source = readScenario("s5_flight_camera.core");
    CountCancellationChecks compile_counter;
    shared::ScenarioExecutionOptions instruction_limited{};
    instruction_limited.limits.max_instructions = 1U;
    auto const bounded = shared::parseScenarioSource(
        "execution-cancelled.core",
        source,
        scenarioLimits(),
        instruction_limited,
        &compile_counter
    );
    ASSERT_FALSE(bounded.has_value());
    EXPECT_EQ(bounded.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);

    CancelAfterChecks const cancellation{compile_counter.checks() + 1U};
    auto const cancelled = shared::parseScenarioSource(
        "execution-cancelled.core",
        source,
        scenarioLimits(),
        shared::ScenarioExecutionOptions{},
        &cancellation
    );
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code, shared::ScenarioDiagnosticCode::Cancelled);
    EXPECT_EQ(cancellation.checks(), compile_counter.checks() + 1U);
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
    using RunResult = std::expected<acceptance::RuntimeEvidence, std::string>;
    std::unique_ptr<RunResult const> const multiplayer_result_owner{
        new RunResult{acceptance::runScenario(*multiplayer, options)}
    };
    auto const& multiplayer_result = *multiplayer_result_owner;
    std::unique_ptr<RunResult const> const camera_result_owner{
        new RunResult{acceptance::runScenario(*camera, options)}
    };
    auto const& camera_result = *camera_result_owner;
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
    static constexpr std::string_view VALID_PREFIX = R"(@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
)";
    static constexpr std::string_view LIMITED = R"(@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(1u64)
    playerXYZ("alice", '@'c8, 0i32, 0i32, 0i32, 0i16, 0i16, 0i16)
    wait(2u64)
}
)";
    static constexpr std::string_view MAX_TICK_THEN_INPUT = R"(@version(0.1.5)
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
        shared::parseScenarioSource("unknown-version.core", "scenario 10\n", scenarioLimits()).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader
    );
    EXPECT_EQ(
        shared::parseScenarioSource(
            "legacy-012.core", "@version(\"0.1.2\")\n", scenarioLimits()
        ).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader
    );
    EXPECT_EQ(
        shared::parseScenarioSource(
            "legacy-013.core", "@version(\"0.1.3\")\n", scenarioLimits()
        ).error().code,
        shared::ScenarioDiagnosticCode::UnknownSourceHeader
    );
    EXPECT_EQ(
        shared::parseScenarioSource("malformed-015.core", "@version(0.1.5)\nscenario 1\n", scenarioLimits())
            .error().code,
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

TEST(CoreLangScenario, LoadsCanonicalCoreLangScenario)
{
    auto const migrated = shared::parseScenarioSource(
        "canonical_sample.core",
        readScenario("canonical_sample.core"),
        scenarioLimits()
    );

    ASSERT_TRUE(migrated.has_value()) << migrated.error().message;
    ASSERT_EQ(migrated->actors().size(), 1U);
    EXPECT_GT(migrated->totalTicks(), 0U);
    EXPECT_GT(migrated->evidenceCount(), 0U);
}

TEST(CoreLangScenario, LowersServerOwnedMovementPermissionAndPhysicsCommands)
{
    auto const parsed = shared::parseScenarioSource(
        "s7_permissions_physics.core",
        readScenario("s7_permissions_physics.core"),
        scenarioLimits()
    );

    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->totalTicks(), 47U);
    EXPECT_EQ(parsed->evidenceCount(), 8U);
    EXPECT_EQ(parsed->actors().size(), 2U);
    std::string const replay_id = shared::scenarioReplayId(*parsed);
    EXPECT_FALSE(replay_id.empty());
    auto const repeated = shared::parseScenarioSource(
        "s7_permissions_physics.core",
        readScenario("s7_permissions_physics.core"),
        scenarioLimits()
    );
    ASSERT_TRUE(repeated.has_value()) << repeated.error().message;
    EXPECT_EQ(shared::scenarioReplayId(*repeated), replay_id);
}

TEST(CoreLangScenario, RunsPermissionFlightPhaseAndJumpAssertionsOnTheAuthoritativeServer)
{
    auto const parsed = shared::parseScenarioSource(
        "s7_permissions_physics.core",
        readScenario("s7_permissions_physics.core"),
        scenarioLimits()
    );
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto const result = acceptance::runScenario(*parsed, {
        .deadline = std::chrono::seconds{5},
        .network_poll_interval = std::chrono::milliseconds{1},
    });

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->passed);
    EXPECT_EQ(result->clients_requested, 2U);
    EXPECT_EQ(result->clients_accepted, 2U);
    EXPECT_EQ(result->ticks, 47U);
    EXPECT_EQ(result->inputs_sent, 47U);
    EXPECT_EQ(result->expectations_passed, 8U);

    auto const replay = acceptance::runScenario(*parsed, {
        .deadline = std::chrono::seconds{5},
        .network_poll_interval = std::chrono::milliseconds{1},
    });
    ASSERT_TRUE(replay.has_value()) << replay.error();
    EXPECT_TRUE(replay->passed);
    EXPECT_EQ(replay->replay_id, result->replay_id);
    EXPECT_EQ(replay->ticks, result->ticks);
    EXPECT_EQ(replay->inputs_sent, result->inputs_sent);
    EXPECT_EQ(replay->expectations_passed, result->expectations_passed);
}

TEST(CoreLangScenario, RejectsCollisionBypassWithoutFlightPermission)
{
    static constexpr std::string_view SOURCE = R"(@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(7u64)
    playerXYZ("alice", '@'c8, 4i32, 4i32, 16i32, 0i16, 0i16, 0i16)
    movementPermissions(false, true)
}
)";

    auto const parsed = shared::parseScenarioSource("invalid-movement-permissions.core", SOURCE, scenarioLimits());

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
    EXPECT_NE(parsed.error().message.find("collision bypass requires flight permission"), std::string::npos);
}


} // namespace
