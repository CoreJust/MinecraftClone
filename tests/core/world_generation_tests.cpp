#include <shared/world/WorldGeneration.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

static constexpr std::string_view LEGACY_WORLD_SCRIPT = R"corelang(
@version("0.1.2")
@use minecraft

pub fn base_height() -> f64 { 6.0f64 }
pub fn crest_spacing() -> f64 { 128.0f64 }
pub fn crest_amplitude() -> f64 { 800.0f64 }
)corelang";

static constexpr std::string_view OMITTED_PRIORITY_WORLD_SCRIPT = R"corelang(
@version("0.1.2")
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
@version("0.1.2")
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
@version("0.1.2")
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
@version("0.1.2")
@use minecraft

pub fn configure_generation() -> u8 { 1u8 }

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
    shared::HeightTile const repeated = generator.generateHeightTile(TILE);

    ASSERT_TRUE(generator.usingCoreLang());
    EXPECT_EQ(first.coordinate, TILE);
    EXPECT_EQ(first.heights, repeated.heights);
    ASSERT_TRUE(first.heightAt(SAMPLE).has_value());
    EXPECT_EQ(*first.heightAt(SAMPLE), generator.heightAt(32'771, 32'775));
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
