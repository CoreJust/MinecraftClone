#include <shared/world/ScriptedWorld.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

[[nodiscard]]
std::string canonicalWorldScript()
{
    std::filesystem::path const path = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path()
        / "scenarios/world/canonical_world.core";
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]]
std::string_view missingPublicationScript()
{
    return R"(@version("0.1.2")
@use minecraft
pub fn generate(seed: u64) {
    let ignored = seed
}
)";
}

[[nodiscard]]
std::string_view outOfBoundsScript()
{
    return R"(@version("0.1.2")
@use minecraft
pub fn generate(seed: u64) {
    set_block(16i32, 0i32, 0i32, 1i32)
    publish()
}
)";
}

TEST(ScriptedWorldTest, CanonicalScriptMatchesTheNativeFixtureAndConfigurationIdentity)
{
    static constexpr uint64_t SEED = 42U;

    auto const world = shared::ScriptedWorld::load(canonicalWorldScript());

    ASSERT_TRUE(world.has_value()) << world.error().message;
    shared::Chunk const fixture = shared::Chunk::makeStoneFixture({}, SEED);
    EXPECT_EQ(world->chunk().contentIdentity(), fixture.contentIdentity());
    EXPECT_EQ(world->configuration().seed, SEED);
    EXPECT_EQ(world->configuration().chunk_content_digest, world->chunk().contentIdentity().content_hash);
    EXPECT_TRUE(shared::isValidWorldConfiguration(world->configuration()));
}

TEST(ScriptedWorldTest, SeedControlsRepeatableScriptedTerrain)
{
    static constexpr uint64_t FIRST_SEED = 42U;
    static constexpr uint64_t SECOND_SEED = 43U;

    auto const first = shared::ScriptedWorld::load(canonicalWorldScript(), {.seed = FIRST_SEED});
    auto const repeated = shared::ScriptedWorld::load(canonicalWorldScript(), {.seed = FIRST_SEED});
    auto const changed = shared::ScriptedWorld::load(canonicalWorldScript(), {.seed = SECOND_SEED});

    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(repeated.has_value()) << repeated.error().message;
    ASSERT_TRUE(changed.has_value()) << changed.error().message;
    EXPECT_EQ(first->chunk().contentIdentity(), repeated->chunk().contentIdentity());
    EXPECT_NE(first->chunk().contentIdentity(), changed->chunk().contentIdentity());
}

TEST(ScriptedWorldTest, FailuresDoNotPublishCandidateData)
{
    auto const published = shared::ScriptedWorld::load(canonicalWorldScript());
    ASSERT_TRUE(published.has_value()) << published.error().message;
    shared::ChunkContentIdentity const original_identity = published->chunk().contentIdentity();

    auto const malformed = shared::ScriptedWorld::load("@version(\"0.1.2\")\nthis is invalid");
    auto const missing_publication = shared::ScriptedWorld::load(missingPublicationScript());
    auto const out_of_bounds = shared::ScriptedWorld::load(outOfBoundsScript());

    EXPECT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().code, shared::ScriptedWorldErrorCode::CompilationFailed);
    EXPECT_FALSE(missing_publication.has_value());
    EXPECT_EQ(missing_publication.error().code, shared::ScriptedWorldErrorCode::IncompletePublication);
    EXPECT_FALSE(out_of_bounds.has_value());
    EXPECT_EQ(out_of_bounds.error().code, shared::ScriptedWorldErrorCode::RuntimeFailed);
    EXPECT_EQ(published->chunk().contentIdentity(), original_identity);
}

TEST(ScriptedWorldTest, RejectsUnsupportedOptionsAndCallbackBudget)
{
    static constexpr uint64_t TOO_SMALL_CALLBACK_BUDGET = 1U;

    auto const invalid_dimensions = shared::ScriptedWorld::load(
        canonicalWorldScript(),
        {.chunk_width = 8U}
    );
    auto const bounded = shared::ScriptedWorld::load(
        canonicalWorldScript(),
        {.max_host_calls = TOO_SMALL_CALLBACK_BUDGET}
    );

    EXPECT_FALSE(invalid_dimensions.has_value());
    EXPECT_EQ(invalid_dimensions.error().code, shared::ScriptedWorldErrorCode::InvalidOptions);
    EXPECT_FALSE(bounded.has_value());
    EXPECT_EQ(bounded.error().code, shared::ScriptedWorldErrorCode::HostCallLimitExceeded);
}

} // namespace
