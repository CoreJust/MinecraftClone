#include <shared/world/Chunk.hpp>
#include <shared/world/WorldGeneration.hpp>
#include <shared/world/WorldGenerationScheduler.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename Value>
Value const& requiredValue(std::optional<Value> const& optional)
{
    if (optional.has_value()) {
        return *optional;
    }
    throw std::runtime_error("Test expected an optional value");
}

[[nodiscard]]
std::string worldGenerationScript(std::string_view const declarations)
{
    std::string source{R"corelang(@version(0.1.5)
@use minecraft

pub fn configure_generation() {
)corelang"};
    source.append(declarations);
    source.append(R"corelang(
}

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang");
    return source;
}

} // namespace

static constexpr std::string_view LEGACY_WORLD_SCRIPT = R"corelang(
@version(0.1.5)
@use minecraft

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

static constexpr std::string_view OMITTED_PRIORITY_WORLD_SCRIPT = R"corelang(
@version(0.1.5)
@use minecraft

pub fn configure_generation() {
    generation_plan_version(1u8)
    generation_refinement(64u64)
    generation_virtual_stage(2u8)
}

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

static constexpr std::string_view UNSUPPORTED_VERSION_WORLD_SCRIPT = R"corelang(
@version(0.1.5)
@use minecraft

pub fn configure_generation() {
    generation_plan_version(2u8)
    generation_virtual_stage(2u8)
}

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

static constexpr std::string_view UNREGISTERED_EXTENT_WORLD_SCRIPT = R"corelang(
@version(0.1.5)
@use minecraft

pub fn configure_generation() {
    generation_plan_version(1u8)
    generation_refinement(2048u64)
    generation_virtual_stage(2u8)
}

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

static constexpr std::string_view WRONG_SIGNATURE_WORLD_SCRIPT = R"corelang(
@version(0.1.5)
@use minecraft

pub fn configure_generation() -> u8 { 1u8 }

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

static constexpr std::string_view STATEFUL_WORLD_SCRIPT = R"corelang(
@version(0.1.5)
@use minecraft

pub fn configure_generation() {
    static mut configured = 0u64
    configured += 32u64
    configured += 32u64
    generation_plan_version(1u8)
    generation_refinement(configured)
}

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

TEST(WorldGenerationTest, FormulaHasPublishedBaseCenterAndFirstCrest)
{
    static constexpr int64_t CENTER = 32'768;
    static constexpr int64_t FIRST_RING = 32'896;
    static constexpr uint16_t BASE_HEIGHT = 6U;
    static constexpr uint16_t CENTER_HEIGHT = 806U;
    static constexpr uint16_t FIRST_RING_HEIGHT = 406U;

    shared::TerrainGenerator generator;
    EXPECT_EQ(generator.heightAt(CENTER, CENTER), CENTER_HEIGHT);
    EXPECT_EQ(generator.heightAt(CENTER + FIRST_RING - CENTER, CENTER), FIRST_RING_HEIGHT);
    EXPECT_EQ(generator.heightAt(CENTER + 64, CENTER), BASE_HEIGHT);
}

TEST(WorldGenerationTest, HeightTileIsDeterministicAndUsesCoreLangScript)
{
    static constexpr shared::HeightTileCoordinate TILE{.x = 2'048, .y = 2'048};
    static constexpr shared::BlockCoordinate SAMPLE{.x = 3, .y = 7, .z = 0};

    shared::TerrainGenerator generator;
    shared::HeightTile const first = generator.generateHeightTile(TILE);
    std::unique_ptr<shared::HeightTile const> const repeated{
        new shared::HeightTile{generator.generateHeightTile(TILE)}
    };

    ASSERT_TRUE(generator.usingCoreLang());
    EXPECT_EQ(first.coordinate, TILE);
    EXPECT_EQ(first.heights, repeated->heights);
    ASSERT_TRUE(first.heightAt(SAMPLE).has_value());
    EXPECT_EQ(requiredValue(first.heightAt(SAMPLE)), generator.heightAt(32'771, 32'775));
}

TEST(WorldGenerationTest, ChunksContainStoneBelowTheGeneratedHeight)
{
    static constexpr shared::ChunkCoordinate COORDINATE{.x = 2'048, .y = 2'048, .z = 0};
    static constexpr shared::BlockCoordinate SAMPLE{.x = 3, .y = 7, .z = 0};

    shared::TerrainGenerator generator;
    shared::Chunk const chunk = generator.generateChunk(COORDINATE);

    EXPECT_EQ(chunk.blockAt(SAMPLE), shared::Block::Stone);
    EXPECT_EQ(chunk.blockAt({.x = SAMPLE.x, .y = SAMPLE.y, .z = 15U}), shared::Block::Stone);
}

TEST(WorldGenerationTest, BuiltInScriptPublishesItsOrderedGenerationPlan)
{
    static constexpr std::array<uint32_t, 2U> EXPECTED_EXTENTS{4'096U, 1'024U};
    static constexpr std::array<shared::GenerationStage, 2U> EXPECTED_STAGES{
        shared::GenerationStage::HeightTile,
        shared::GenerationStage::Materialize,
    };

    shared::TerrainGenerator const generator;
    shared::WorldGenerationPlan const& plan = generator.generationPlan();

    EXPECT_TRUE(plan.pre_generate);
    EXPECT_TRUE(std::ranges::equal(plan.refinement_extents, EXPECTED_EXTENTS));
    EXPECT_TRUE(std::ranges::equal(plan.chunk_stages, EXPECTED_STAGES));
    EXPECT_TRUE(std::ranges::equal(plan.priority_order, shared::DEFAULT_GENERATION_PRIORITY_ORDER));
}

TEST(WorldGenerationTest, MissingPlanEntrypointUsesCompatibleMaterializationDefault)
{
    static constexpr std::array<shared::GenerationStage, 1U> EXPECTED_STAGES{
        shared::GenerationStage::Materialize,
    };

    shared::TerrainGenerator const generator{LEGACY_WORLD_SCRIPT};
    shared::WorldGenerationPlan const& plan = generator.generationPlan();

    EXPECT_FALSE(plan.pre_generate);
    EXPECT_TRUE(plan.refinement_extents.empty());
    EXPECT_TRUE(std::ranges::equal(plan.chunk_stages, EXPECTED_STAGES));
    EXPECT_TRUE(std::ranges::equal(plan.priority_order, shared::DEFAULT_GENERATION_PRIORITY_ORDER));
}

TEST(WorldGenerationTest, OmittedPriorityUsesTheDocumentedDefaultOrder)
{
    static constexpr std::array<uint32_t, 1U> EXPECTED_EXTENTS{64U};
    static constexpr std::array<shared::GenerationStage, 1U> EXPECTED_STAGES{
        shared::GenerationStage::Materialize,
    };

    shared::TerrainGenerator const generator{OMITTED_PRIORITY_WORLD_SCRIPT};
    shared::WorldGenerationPlan const& plan = generator.generationPlan();

    EXPECT_FALSE(plan.pre_generate);
    EXPECT_TRUE(std::ranges::equal(plan.refinement_extents, EXPECTED_EXTENTS));
    EXPECT_TRUE(std::ranges::equal(plan.chunk_stages, EXPECTED_STAGES));
    EXPECT_TRUE(std::ranges::equal(plan.priority_order, shared::DEFAULT_GENERATION_PRIORITY_ORDER));
}

TEST(WorldGenerationTest, UnsupportedPlanVersionFailsClosed)
{
    EXPECT_THROW(
        shared::TerrainGenerator{UNSUPPORTED_VERSION_WORLD_SCRIPT},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, UnregisteredRefinementExtentFailsClosed)
{
    EXPECT_THROW(
        shared::TerrainGenerator{UNREGISTERED_EXTENT_WORLD_SCRIPT},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, WrongPlanEntrypointSignatureFailsClosed)
{
    EXPECT_THROW(
        shared::TerrainGenerator{WRONG_SIGNATURE_WORLD_SCRIPT},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, FunctionLocalStaticIsBoundedWithinEntry)
{
    shared::TerrainGenerator const generator{STATEFUL_WORLD_SCRIPT};

    EXPECT_EQ(generator.generationPlan().refinement_extents, std::vector<uint32_t>{64U});
    EXPECT_EQ(generator.heightAt(32'768, 32'768), 806U);
}

TEST(WorldGenerationTest, OversizedSourceIsRejectedWithTheAdapterErrorType)
{
    static constexpr size_t MAX_SOURCE_BYTES = 65'536U;
    std::string const source(MAX_SOURCE_BYTES + 1U, ' ');

    EXPECT_THROW(shared::TerrainGenerator{source}, std::runtime_error);
}

TEST(WorldGenerationTest, RejectsVersionDeclarationsOutOfOrderOrRepeated)
{
    static constexpr std::string_view BEFORE_VERSION = R"corelang(
    generation_pregen()
    generation_plan_version(1u8)
    generation_virtual_stage(2u8)
)corelang";
    static constexpr std::string_view REPEATED_VERSION = R"corelang(
    generation_plan_version(1u8)
    generation_plan_version(1u8)
    generation_virtual_stage(2u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(BEFORE_VERSION)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(REPEATED_VERSION)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, RejectsRepeatedPregenDeclaration)
{
    static constexpr std::string_view DECLARATIONS = R"corelang(
    generation_plan_version(1u8)
    generation_pregen()
    generation_pregen()
    generation_virtual_stage(2u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DECLARATIONS)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, RejectsInvalidRefinementOrderDuplicateAndPregenStart)
{
    static constexpr std::string_view DUPLICATE = R"corelang(
    generation_plan_version(1u8)
    generation_refinement(4096u64)
    generation_refinement(4096u64)
    generation_virtual_stage(2u8)
)corelang";
    static constexpr std::string_view OUT_OF_ORDER = R"corelang(
    generation_plan_version(1u8)
    generation_refinement(1024u64)
    generation_refinement(4096u64)
    generation_virtual_stage(2u8)
)corelang";
    static constexpr std::string_view PREGEN_START = R"corelang(
    generation_plan_version(1u8)
    generation_pregen()
    generation_refinement(16384u64)
    generation_virtual_stage(2u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DUPLICATE)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(OUT_OF_ORDER)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(PREGEN_START)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, AcceptsFiveRefinementExtentsAndRejectsTheSixth)
{
    static constexpr std::string_view AT_LIMIT = R"corelang(
    generation_plan_version(1u8)
    generation_refinement(16384u64)
    generation_refinement(4096u64)
    generation_refinement(1024u64)
    generation_refinement(256u64)
    generation_refinement(64u64)
)corelang";
    static constexpr std::string_view ONE_OVER = R"corelang(
    generation_plan_version(1u8)
    generation_refinement(16384u64)
    generation_refinement(4096u64)
    generation_refinement(1024u64)
    generation_refinement(256u64)
    generation_refinement(64u64)
    generation_refinement(16384u64)
)corelang";

    shared::TerrainGenerator const at_limit{worldGenerationScript(AT_LIMIT)};
    EXPECT_EQ(
        at_limit.generationPlan().refinement_extents,
        (std::vector<uint32_t>{16'384U, 4'096U, 1'024U, 256U, 64U})
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(ONE_OVER)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, RejectsInvalidVirtualStageValuesAndOrdering)
{
    static constexpr std::string_view INVALID_VALUE = R"corelang(
    generation_plan_version(1u8)
    generation_virtual_stage(3u8)
)corelang";
    static constexpr std::string_view DUPLICATE_HEIGHT_TILE = R"corelang(
    generation_plan_version(1u8)
    generation_virtual_stage(1u8)
    generation_virtual_stage(1u8)
)corelang";
    static constexpr std::string_view DUPLICATE_MATERIALIZE = R"corelang(
    generation_plan_version(1u8)
    generation_virtual_stage(2u8)
    generation_virtual_stage(2u8)
)corelang";
    static constexpr std::string_view MATERIALIZE_NOT_LAST = R"corelang(
    generation_plan_version(1u8)
    generation_virtual_stage(2u8)
    generation_virtual_stage(1u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(INVALID_VALUE)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DUPLICATE_HEIGHT_TILE)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DUPLICATE_MATERIALIZE)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(MATERIALIZE_NOT_LAST)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, RejectsFirstVirtualStageBeyondTwoStages)
{
    static constexpr std::string_view DECLARATIONS = R"corelang(
    generation_plan_version(1u8)
    generation_virtual_stage(1u8)
    generation_virtual_stage(2u8)
    generation_virtual_stage(1u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DECLARATIONS)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, RejectsInvalidDuplicateAndExcessPriorityDimensions)
{
    static constexpr std::string_view INVALID_VALUE = R"corelang(
    generation_plan_version(1u8)
    generation_priority(3u8)
)corelang";
    static constexpr std::string_view DUPLICATE = R"corelang(
    generation_plan_version(1u8)
    generation_priority(0u8)
    generation_priority(0u8)
)corelang";
    static constexpr std::string_view ONE_OVER = R"corelang(
    generation_plan_version(1u8)
    generation_priority(0u8)
    generation_priority(1u8)
    generation_priority(2u8)
    generation_priority(0u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(INVALID_VALUE)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DUPLICATE)},
        std::runtime_error
    );
    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(ONE_OVER)},
        std::runtime_error
    );
}

TEST(WorldGenerationTest, RejectsAPlanContainingOnlyItsVersion)
{
    static constexpr std::string_view DECLARATIONS = R"corelang(
    generation_plan_version(1u8)
)corelang";

    EXPECT_THROW(
        shared::TerrainGenerator{worldGenerationScript(DECLARATIONS)},
        std::runtime_error
    );
}
