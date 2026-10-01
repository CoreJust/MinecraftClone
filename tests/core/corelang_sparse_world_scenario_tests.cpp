#include <shared/scenario/Scenario.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <variant>

namespace {

void expectRuntimeFailure(
    std::string_view const filename,
    std::string_view const source,
    shared::ScenarioLimits const& limits
)
{
    auto const parsed = shared::parseScenarioSource(filename, source, limits);

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, shared::ScenarioDiagnosticCode::CoreLangRuntimeFailure);
}

TEST(CoreLangSparseWorldScenario, LowersVersionedOptionsAndBoundedObservations)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
    expectBlockXYZ(0i64, 0i64, 0i64, 1u8)
    expectResidentChunks(1u64)
}
)";

    auto const parsed = shared::parseScenarioSource("sparse-world.core", SOURCE, SCENARIO_LIMITS);

    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->profile(), shared::ScenarioProfile::SparseWorldV1);
    EXPECT_EQ(parsed->seed(), 42U);
    EXPECT_TRUE(parsed->actors().empty());
    EXPECT_EQ(parsed->totalTicks(), 0U);
    EXPECT_EQ(parsed->evidenceCount(), 2U);
    ASSERT_EQ(parsed->operations().size(), 3U);

    auto const* const options = std::get_if<shared::ScenarioSparseWorldOptionsOperation>(
        &parsed->operations()[0].data
    );
    auto const* const block = std::get_if<shared::ScenarioExpectBlockOperation>(
        &parsed->operations()[1].data
    );
    auto const* const resident_chunks = std::get_if<shared::ScenarioExpectResidentChunksOperation>(
        &parsed->operations()[2].data
    );
    ASSERT_NE(options, nullptr);
    ASSERT_NE(block, nullptr);
    ASSERT_NE(resident_chunks, nullptr);
    EXPECT_EQ(options->generator_version, 1U);
    EXPECT_EQ(options->max_resident_chunks, 1U);
    EXPECT_EQ(block->x, 0);
    EXPECT_EQ(block->y, 0);
    EXPECT_EQ(block->z, 0);
    EXPECT_EQ(block->block, shared::Block::Stone);
    EXPECT_EQ(resident_chunks->count, 1U);
}

TEST(CoreLangSparseWorldScenario, RejectsZeroResidentChunkLimit)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
        .max_sparse_world_resident_chunks = 4U,
    };
    static constexpr std::string_view ZERO_RESIDENT_LIMIT = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 0u64)
}
)";
    expectRuntimeFailure("zero-resident-limit.core", ZERO_RESIDENT_LIMIT, SCENARIO_LIMITS);
}

TEST(CoreLangSparseWorldScenario, RejectsUnsupportedGeneratorVersion)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(2u32, 1u64)
}
)";

    expectRuntimeFailure("unsupported-generator-version.core", SOURCE, SCENARIO_LIMITS);
}

TEST(CoreLangSparseWorldScenario, RejectsResidentLimitAboveHostConfiguration)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
        .max_sparse_world_resident_chunks = 4U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 5u64)
}
)";

    expectRuntimeFailure("excessive-resident-limit.core", SOURCE, SCENARIO_LIMITS);
}

TEST(CoreLangSparseWorldScenario, RejectsObservationsBeyondTheEvidenceLimit)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 1U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
    expectBlockXYZ(0i64, 0i64, 0i64, 1u8)
    expectResidentChunks(1u64)
}
)";

    expectRuntimeFailure("sparse-world-evidence-limit.core", SOURCE, SCENARIO_LIMITS);
}

TEST(CoreLangSparseWorldScenario, RejectsObservationsBeyondTheOperationLimit)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 2U,
        .max_evidence = 8U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
    expectBlockXYZ(0i64, 0i64, 0i64, 1u8)
    expectResidentChunks(1u64)
}
)";

    expectRuntimeFailure("sparse-world-operation-limit.core", SOURCE, SCENARIO_LIMITS);
}

TEST(CoreLangSparseWorldScenario, KeepsSparseWorldHostCallsOutOfVersion012)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.2")
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
}
)";

    auto const parsed = shared::parseScenarioSource("legacy-sparse-world.core", SOURCE, SCENARIO_LIMITS);

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, shared::ScenarioDiagnosticCode::CoreLangCompileFailure);
}

TEST(CoreLangSparseWorldScenario, RejectsTickOperationsAfterSparseProfile)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    wait(1u64)
    sparseWorldOptions(1u32, 1u64)
}
)";

    expectRuntimeFailure("wait-after-profile.core", SOURCE, SCENARIO_LIMITS);
}

TEST(CoreLangSparseWorldScenario, RejectsTickOperationsBeforeSparseProfile)
{
    static constexpr shared::ScenarioLimits SCENARIO_LIMITS{
        .max_source_bytes = 8'192U,
        .max_statements = 64U,
        .max_actors = 4U,
        .max_total_ticks = 100U,
        .max_operations = 32U,
        .max_evidence = 8U,
    };
    static constexpr std::string_view SOURCE = R"(@version("0.1.3")
@use minecraft
pub fn scenario() {
    wait(1u64)
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
}
)";

    expectRuntimeFailure("wait-before-profile.core", SOURCE, SCENARIO_LIMITS);
}

} // namespace
