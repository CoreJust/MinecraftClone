#include <shared/world/TerrainWorld.hpp>

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]]
std::filesystem::path referenceFixtureRoot()
{
#ifdef MC_TERRAIN_REFERENCE_FIXTURE_DIR
    return std::filesystem::path{ MC_TERRAIN_REFERENCE_FIXTURE_DIR };
#else
    return std::filesystem::path{ __FILE__ }
        .parent_path().parent_path() / "fixtures" / "terrain_v2";
#endif
}

[[nodiscard]]
std::vector<uint8_t> readReferenceBytes(
    std::filesystem::path const& fixture_root,
    uint64_t const seed,
    std::string_view const field_name
)
{
    std::filesystem::path const path = fixture_root
        / ("seed-" + std::to_string(seed))
        / (std::string{ field_name } + ".raw");
    std::ifstream stream{ path, std::ios::binary };
    if (!stream.is_open()) {
        return {};
    }

    std::vector<uint8_t> bytes;
    char value = '\0';
    while (stream.get(value)) {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

constexpr std::array<std::array<uint8_t, 4U>, 7U> SURFACE_RGBA_BY_BLOCK{
    std::array<uint8_t, 4U>{ 189U, 217U, 237U, 255U },
    std::array<uint8_t, 4U>{ 128U, 128U, 128U, 255U },
    std::array<uint8_t, 4U>{ 92U, 147U, 69U, 255U },
    std::array<uint8_t, 4U>{ 136U, 103U, 72U, 255U },
    std::array<uint8_t, 4U>{ 42U, 40U, 49U, 255U },
    std::array<uint8_t, 4U>{ 244U, 247U, 250U, 255U },
    std::array<uint8_t, 4U>{ 128U, 201U, 214U, 255U },
};

[[nodiscard]]
uint32_t readLittleEndian32(std::vector<uint8_t> const& bytes, uint32_t const offset) noexcept
{
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<uint32_t>(bytes[offset + 3U]) << 24U);
}

uint64_t readLittleEndian64(std::vector<uint8_t> const& bytes, uint32_t const offset) noexcept
{
    uint64_t value = 0U;
    for (uint32_t byte = 0U; byte < 8U; ++byte) {
        value |= static_cast<uint64_t>(bytes[offset + byte]) << (8U * byte);
    }
    return value;
}

} // namespace

void expectFrozenAndRevision4SurfaceSamplesForSeed(uint64_t const seed)
{
    static constexpr uint32_t SAMPLE_SIDE = 16U;
    static constexpr uint32_t SAMPLE_COUNT = SAMPLE_SIDE * SAMPLE_SIDE;
    static constexpr uint32_t SAMPLE_INSET = 128U;
    static constexpr uint32_t SAMPLE_STRIDE = 4'096U;
    static constexpr uint32_t LEAF_EXTENT = 64U;
    static constexpr uint32_t MATERIALIZATION_SAMPLE_INDEX = 71U;
    std::filesystem::path const legacy_fixture_root = referenceFixtureRoot();
    std::filesystem::path const revision4_fixture_root = legacy_fixture_root.parent_path()
        / "terrain_v2_revision_4";
    std::filesystem::path const revision5_fixture_root = legacy_fixture_root.parent_path()
        / "terrain_v2_revision_5";
    std::vector<uint8_t> const frozen_heights = readReferenceBytes(
        legacy_fixture_root,
        seed,
        "height"
    );
    std::vector<uint8_t> const frozen_top_materials = readReferenceBytes(
        legacy_fixture_root,
        seed,
        "top_material"
    );
    std::vector<uint8_t> const frozen_ground_materials = readReferenceBytes(
        legacy_fixture_root,
        seed,
        "ground_material"
    );
    std::vector<uint8_t> const revision4_heights = readReferenceBytes(
        revision4_fixture_root,
        seed,
        "height"
    );
    std::vector<uint8_t> const revision4_top_materials = readReferenceBytes(
        revision4_fixture_root,
        seed,
        "top_material"
    );
    std::vector<uint8_t> const revision4_ground_materials = readReferenceBytes(
        revision4_fixture_root,
        seed,
        "ground_material"
    );
    std::vector<uint8_t> const revision4_pixel_rgba = readReferenceBytes(
        revision4_fixture_root,
        seed,
        "pixel_rgba"
    );
    std::vector<uint8_t> const revision5_precipitation = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "input_precipitation"
    );
    std::vector<uint8_t> const revision4_vertical_scale = readReferenceBytes(
        legacy_fixture_root,
        seed,
        "atlas__vertical_scale"
    );
    std::vector<uint8_t> const revision5_points = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "point_xy"
    );
    std::vector<uint8_t> const revision5_heights_v4 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "height_v4"
    );
    std::vector<uint8_t> const revision5_heights_v5 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "height_v5"
    );
    std::vector<uint8_t> const revision5_top_v4 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "top_material_v4"
    );
    std::vector<uint8_t> const revision5_ground_v4 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "ground_material_v4"
    );
    std::vector<uint8_t> const revision5_top_v5 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "top_material_v5"
    );
    std::vector<uint8_t> const revision5_ground_v5 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "ground_material_v5"
    );
    std::vector<uint8_t> const revision5_elevation_v4 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "elevation_v4"
    );
    std::vector<uint8_t> const revision5_elevation_v5 = readReferenceBytes(
        revision5_fixture_root,
        seed,
        "elevation_v5"
    );
    ASSERT_EQ(static_cast<uint64_t>(frozen_heights.size()), SAMPLE_COUNT * 4U) << "seed " << seed;
    ASSERT_EQ(static_cast<uint64_t>(frozen_top_materials.size()), SAMPLE_COUNT) << "seed " << seed;
    ASSERT_EQ(static_cast<uint64_t>(frozen_ground_materials.size()), SAMPLE_COUNT) << "seed " << seed;
    ASSERT_EQ(static_cast<uint64_t>(revision4_heights.size()), SAMPLE_COUNT * 4U) << "seed " << seed;
    ASSERT_EQ(static_cast<uint64_t>(revision4_top_materials.size()), SAMPLE_COUNT) << "seed " << seed;
    ASSERT_EQ(static_cast<uint64_t>(revision4_ground_materials.size()), SAMPLE_COUNT) << "seed " << seed;
    ASSERT_EQ(static_cast<uint64_t>(revision4_pixel_rgba.size()), SAMPLE_COUNT * 4U) << "seed " << seed;
    ASSERT_EQ(revision5_precipitation.size(), 256U * 256U * sizeof(float)) << "seed " << seed;
    ASSERT_EQ(revision4_vertical_scale.size(), sizeof(double)) << "seed " << seed;
    ASSERT_GT(revision5_points.size(), 0U) << "seed " << seed;
    ASSERT_EQ(revision5_points.size() % (2U * sizeof(uint32_t)), 0U) << "seed " << seed;
    uint32_t const revision5_point_count = static_cast<uint32_t>(
        revision5_points.size() / (2U * sizeof(uint32_t))
    );
    ASSERT_EQ(revision5_heights_v4.size(), revision5_point_count * sizeof(uint32_t)) << "seed " << seed;
    ASSERT_EQ(revision5_heights_v5.size(), revision5_point_count * sizeof(uint32_t)) << "seed " << seed;
    ASSERT_EQ(revision5_top_v4.size(), revision5_point_count) << "seed " << seed;
    ASSERT_EQ(revision5_ground_v4.size(), revision5_point_count) << "seed " << seed;
    ASSERT_EQ(revision5_top_v5.size(), revision5_point_count) << "seed " << seed;
    ASSERT_EQ(revision5_ground_v5.size(), revision5_point_count) << "seed " << seed;
    ASSERT_EQ(revision5_elevation_v4.size(), revision5_point_count * sizeof(double)) << "seed " << seed;
    ASSERT_EQ(revision5_elevation_v5.size(), revision5_point_count * sizeof(double)) << "seed " << seed;

    generation::GenerationConfiguration configuration;
    configuration.seed = seed;
    configuration.resolution = 256U;
    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(configuration);
    ASSERT_NE(atlas, nullptr) << "seed " << seed;
    ASSERT_EQ(atlas->precipitation.size(), 256U * 256U) << "seed " << seed;
    for (size_t field_index = 0U; field_index < atlas->precipitation.size(); ++field_index) {
        uint32_t const actual_bits = std::bit_cast<uint32_t>(atlas->precipitation[field_index]);
        uint32_t const expected_bits = readLittleEndian32(
            revision5_precipitation,
            static_cast<uint32_t>(field_index * sizeof(uint32_t))
        );
        ASSERT_EQ(actual_bits, expected_bits) << "seed " << seed << " precipitation index " << field_index;
    }
    EXPECT_EQ(
        std::bit_cast<uint64_t>(atlas->vertical_scale),
        readLittleEndian64(revision4_vertical_scale, 0U)
    ) << "seed " << seed;
    shared::TerrainWorld const world{ atlas };
    EXPECT_NE(world.identity().configuration_identity.find("model-revision=5"), std::string::npos);
    EXPECT_NE(
        world.identity().configuration_identity.find(generation::NUMERICAL_KERNEL_IDENTITY),
        std::string::npos
    );

    for (uint32_t sample_index = 0U; sample_index < SAMPLE_COUNT; ++sample_index) {
        uint32_t const sample_x = sample_index % SAMPLE_SIDE;
        uint32_t const sample_y = sample_index / SAMPLE_SIDE;
        uint32_t const world_x = SAMPLE_INSET + sample_x * SAMPLE_STRIDE;
        uint32_t const world_y = SAMPLE_INSET + sample_y * SAMPLE_STRIDE;
        shared::TerrainRegionKey const leaf_key{
            .origin_x = (world_x / LEAF_EXTENT) * LEAF_EXTENT,
            .origin_y = (world_y / LEAF_EXTENT) * LEAF_EXTENT,
            .extent = LEAF_EXTENT,
        };
        std::shared_ptr<shared::RegionalArtifact const> const leaf = world.prepareRegion(leaf_key);
        shared::RegionalSample const sample = world.sampleRegion(
            *leaf,
            static_cast<double>(world_x),
            static_cast<double>(world_y)
        );
        uint32_t const height_offset = sample_index * 4U;
        uint32_t const frozen_height = readLittleEndian32(frozen_heights, height_offset);
        uint32_t const revision4_height = readLittleEndian32(revision4_heights, height_offset);
        uint8_t const frozen_top_material = frozen_top_materials[sample_index];
        uint8_t const revision4_top_material = revision4_top_materials[sample_index];
        SCOPED_TRACE("seed=" + std::to_string(seed)
            + " sample_x=" + std::to_string(sample_x)
            + " sample_y=" + std::to_string(sample_y));
        ASSERT_EQ(sample.highest_solid_z, frozen_height);
        ASSERT_EQ(sample.highest_solid_z, revision4_height);
        ASSERT_EQ(static_cast<uint8_t>(sample.top_material), frozen_top_material);
        ASSERT_EQ(static_cast<uint8_t>(sample.top_material), revision4_top_material);
        ASSERT_EQ(
            static_cast<uint8_t>(sample.ground_material),
            frozen_ground_materials[sample_index]
        );
        ASSERT_EQ(
            static_cast<uint8_t>(sample.ground_material),
            revision4_ground_materials[sample_index]
        );

        ASSERT_LT(static_cast<uint32_t>(revision4_top_material), SURFACE_RGBA_BY_BLOCK.size());
        uint32_t const rgba_offset = sample_index * 4U;
        std::array<uint8_t, 4U> const revision4_pixel{
            revision4_pixel_rgba[rgba_offset],
            revision4_pixel_rgba[rgba_offset + 1U],
            revision4_pixel_rgba[rgba_offset + 2U],
            revision4_pixel_rgba[rgba_offset + 3U],
        };
        EXPECT_EQ(revision4_pixel, SURFACE_RGBA_BY_BLOCK[revision4_top_material]);
    }

    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<shared::RegionalArtifact const>> regions;
    std::map<std::pair<int32_t, int32_t>, shared::SurfaceTile> surfaces;
    uint32_t changed_height_samples = 0U;
    uint32_t changed_sample_index = 0U;
    for (uint32_t sample_index = 0U; sample_index < revision5_point_count; ++sample_index) {
        uint32_t const point_offset = sample_index * 2U * sizeof(uint32_t);
        uint32_t const point_x = readLittleEndian32(revision5_points, point_offset);
        uint32_t const point_y = readLittleEndian32(revision5_points, point_offset + sizeof(uint32_t));
        uint32_t const region_x = (point_x / LEAF_EXTENT) * LEAF_EXTENT;
        uint32_t const region_y = (point_y / LEAF_EXTENT) * LEAF_EXTENT;
        auto const region_key = std::pair{ region_x, region_y };
        auto region = regions.find(region_key);
        if (region == regions.end()) {
            region = regions.emplace(
                region_key,
                world.prepareRegion({
                    .origin_x = region_x,
                    .origin_y = region_y,
                    .extent = LEAF_EXTENT,
                })
            ).first;
        }

        shared::RegionalSample const regional = world.sampleRegion(
            *region->second,
            static_cast<double>(point_x),
            static_cast<double>(point_y)
        );
        uint32_t const height_offset = sample_index * sizeof(uint32_t);
        uint32_t const expected_v4_height = readLittleEndian32(revision5_heights_v4, height_offset);
        uint32_t const expected_v5_height = readLittleEndian32(revision5_heights_v5, height_offset);
        EXPECT_EQ(regional.highest_solid_z, expected_v4_height)
            << "seed " << seed << " revision-4 point " << point_x << ',' << point_y;
        EXPECT_EQ(static_cast<uint8_t>(regional.top_material), revision5_top_v4[sample_index])
            << "seed " << seed << " revision-4 point " << point_x << ',' << point_y;
        EXPECT_EQ(static_cast<uint8_t>(regional.ground_material), revision5_ground_v4[sample_index])
            << "seed " << seed << " revision-4 point " << point_x << ',' << point_y;
        EXPECT_NEAR(
            regional.geological_elevation_km,
            std::bit_cast<double>(readLittleEndian64(revision5_elevation_v4, sample_index * sizeof(double))),
            2e-12
        ) << "seed " << seed << " revision-4 point " << point_x << ',' << point_y;

        int32_t const chunk_x = static_cast<int32_t>(point_x / shared::Chunk::SIDE_LENGTH);
        int32_t const chunk_y = static_cast<int32_t>(point_y / shared::Chunk::SIDE_LENGTH);
        auto const surface_key = std::pair{ chunk_x, chunk_y };
        auto surface = surfaces.find(surface_key);
        if (surface == surfaces.end()) {
            surface = surfaces.emplace(
                surface_key,
                world.sampleSurface(
                    *region->second,
                    { .x = chunk_x, .y = chunk_y, .z = 0 }
                )
            ).first;
        }
        uint32_t const column_index = (point_y % shared::Chunk::SIDE_LENGTH)
            * shared::Chunk::SIDE_LENGTH + point_x % shared::Chunk::SIDE_LENGTH;
        shared::TerrainColumn const& column = surface->second.columns[column_index];
        EXPECT_EQ(column.highest_solid_z, expected_v5_height)
            << "seed " << seed << " revision-5 point " << point_x << ',' << point_y;
        EXPECT_EQ(static_cast<uint8_t>(column.top_material), revision5_top_v5[sample_index])
            << "seed " << seed << " revision-5 point " << point_x << ',' << point_y;
        EXPECT_EQ(static_cast<uint8_t>(column.ground_material), revision5_ground_v5[sample_index])
            << "seed " << seed << " revision-5 point " << point_x << ',' << point_y;
        EXPECT_NEAR(
            column.geological_elevation_km,
            std::bit_cast<double>(readLittleEndian64(revision5_elevation_v5, sample_index * sizeof(double))),
            2e-12
        ) << "seed " << seed << " revision-5 point " << point_x << ',' << point_y;
        if (expected_v5_height != expected_v4_height) {
            if (changed_height_samples == 0U) {
                changed_sample_index = sample_index;
            }
            ++changed_height_samples;
        }
    }
    ASSERT_GT(changed_height_samples, 0U) << "seed " << seed;

    uint32_t const changed_point_offset = changed_sample_index * 2U * sizeof(uint32_t);
    uint32_t const changed_x = readLittleEndian32(revision5_points, changed_point_offset);
    uint32_t const changed_y = readLittleEndian32(revision5_points, changed_point_offset + sizeof(uint32_t));
    int32_t const changed_chunk_x = static_cast<int32_t>(changed_x / shared::Chunk::SIDE_LENGTH);
    int32_t const changed_chunk_y = static_cast<int32_t>(changed_y / shared::Chunk::SIDE_LENGTH);
    shared::SurfaceTile const& changed_surface = surfaces.at({ changed_chunk_x, changed_chunk_y });
    uint32_t const changed_column_index = (changed_y % shared::Chunk::SIDE_LENGTH)
        * shared::Chunk::SIDE_LENGTH + changed_x % shared::Chunk::SIDE_LENGTH;
    shared::TerrainColumn const& changed_column = changed_surface.columns[changed_column_index];
    shared::Chunk const changed_chunk = world.materializeChunk(
        changed_surface,
        {
            .x = changed_chunk_x,
            .y = changed_chunk_y,
            .z = static_cast<int32_t>(changed_column.highest_solid_z / shared::Chunk::SIDE_LENGTH),
        }
    );
    EXPECT_EQ(
        changed_chunk.blockAt({
            .x = static_cast<uint8_t>(changed_x % shared::Chunk::SIDE_LENGTH),
            .y = static_cast<uint8_t>(changed_y % shared::Chunk::SIDE_LENGTH),
            .z = static_cast<uint8_t>(changed_column.highest_solid_z % shared::Chunk::SIDE_LENGTH),
        }),
        changed_column.materialAt(changed_column.highest_solid_z, changed_surface.block_sea_level)
    ) << "seed " << seed;

    uint32_t const sample_x = MATERIALIZATION_SAMPLE_INDEX % SAMPLE_SIDE;
    uint32_t const sample_y = MATERIALIZATION_SAMPLE_INDEX / SAMPLE_SIDE;
    uint32_t const world_x = SAMPLE_INSET + sample_x * SAMPLE_STRIDE;
    uint32_t const world_y = SAMPLE_INSET + sample_y * SAMPLE_STRIDE;
    shared::ChunkCoordinate const coordinate{
        .x = static_cast<int32_t>(world_x / shared::Chunk::SIDE_LENGTH),
        .y = static_cast<int32_t>(world_y / shared::Chunk::SIDE_LENGTH),
        .z = 0,
    };
    shared::SurfaceTile const surface = world.sampleSurface(coordinate);
    shared::TerrainColumn const& column = surface.columns.front();
    uint32_t const expected_height = readLittleEndian32(
        revision4_heights,
        MATERIALIZATION_SAMPLE_INDEX * 4U
    );
    ASSERT_EQ(column.highest_solid_z, expected_height) << "seed " << seed;
    ASSERT_EQ(
        static_cast<uint8_t>(column.top_material),
        revision4_top_materials[MATERIALIZATION_SAMPLE_INDEX]
    );
    ASSERT_EQ(
        static_cast<uint8_t>(column.ground_material),
        revision4_ground_materials[MATERIALIZATION_SAMPLE_INDEX]
    );

    shared::Chunk const chunk = world.materializeChunk(
        surface,
        {
            .x = coordinate.x,
            .y = coordinate.y,
            .z = static_cast<int32_t>(expected_height / shared::Chunk::SIDE_LENGTH),
        }
    );
    shared::Block const expected_surface_material = expected_height == 0U
        ? shared::Block::Bedrock
        : (expected_height >= surface.block_sea_level
            ? column.top_material
            : (expected_height >= column.ground_first_z
                ? column.ground_material
                : shared::Block::Stone));
    EXPECT_EQ(
        chunk.blockAt({
            .x = 0U,
            .y = 0U,
            .z = static_cast<uint8_t>(expected_height % shared::Chunk::SIDE_LENGTH),
        }),
        expected_surface_material
    ) << "seed " << seed;
}

TEST(TerrainWorldReferenceTest, Seed0MatchesFrozenAndRevision4AndRevision5SurfaceSamples)
{
    expectFrozenAndRevision4SurfaceSamplesForSeed(0U);
}

TEST(TerrainWorldReferenceTest, Seed1MatchesFrozenAndRevision4AndRevision5SurfaceSamples)
{
    expectFrozenAndRevision4SurfaceSamplesForSeed(1U);
}

TEST(TerrainWorldReferenceTest, Seed42MatchesFrozenAndRevision4AndRevision5SurfaceSamples)
{
    expectFrozenAndRevision4SurfaceSamplesForSeed(42U);
}

TEST(TerrainWorldReferenceTest, Seed777MatchesFrozenAndRevision4AndRevision5SurfaceSamples)
{
    expectFrozenAndRevision4SurfaceSamplesForSeed(777U);
}

TEST(TerrainWorldReferenceTest, Seed2004MatchesFrozenAndRevision4AndRevision5SurfaceSamples)
{
    expectFrozenAndRevision4SurfaceSamplesForSeed(2'004U);
}

TEST(TerrainWorldReferenceTest, SeedEightMillionMatchesFrozenAndRevision4AndRevision5SurfaceSamples)
{
    expectFrozenAndRevision4SurfaceSamplesForSeed(8'800'555U);
}
