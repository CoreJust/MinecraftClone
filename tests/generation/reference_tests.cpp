#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

static constexpr uint32_t REFERENCE_RESOLUTION = 256U;
static constexpr uint32_t SAMPLE_STEP = 16U;
static constexpr uint32_t SAMPLE_SIDE = REFERENCE_RESOLUTION / SAMPLE_STEP;
static constexpr uint64_t REFERENCE_SEED = 42U;
static constexpr std::array<uint64_t, 6U> REVISION_4_SEEDS{
    0U, 1U, 42U, 777U, 2004U, 8'800'555U,
};

[[nodiscard]]
std::filesystem::path revision4FixturePath()
{
#ifdef MC_TERRAIN_REFERENCE_FIXTURE_DIR
    return std::filesystem::path(MC_TERRAIN_REFERENCE_FIXTURE_DIR)
        .parent_path() / "terrain_v2_revision_4";
#else
    return std::filesystem::path(__FILE__).parent_path().parent_path()
        / "fixtures" / "terrain_v2_revision_4";
#endif
}

[[nodiscard]]
std::vector<uint8_t> readBytes(std::string const& field_name)
{
#ifdef MC_TERRAIN_REFERENCE_FIXTURE_DIR
    std::filesystem::path const fixture_path = MC_TERRAIN_REFERENCE_FIXTURE_DIR;
#else
    std::filesystem::path const fixture_path = std::filesystem::path(__FILE__)
        .parent_path().parent_path() / "fixtures" / "terrain_v2";
#endif
    std::filesystem::path const raw_path = fixture_path / "seed-42" / (field_name + ".raw");
    std::ifstream stream(raw_path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(stream.is_open()) << "Missing frozen reference field: " << raw_path.string();
    if (!stream.is_open()) {
        return {};
    }

    std::streamsize const size = stream.tellg();
    EXPECT_GE(size, 0) << "Could not determine frozen reference field length: " << raw_path.string();
    if (size < 0) {
        return {};
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    EXPECT_TRUE(stream.good() || stream.eof()) << "Could not read frozen reference field: "
        << raw_path.string();
    return bytes;
}

[[nodiscard]]
std::vector<uint8_t> readRevision4Bytes(uint64_t seed, std::string const& field_name)
{
    std::filesystem::path const raw_path = revision4FixturePath()
        / ("seed-" + std::to_string(seed)) / (field_name + ".raw");
    std::ifstream stream(raw_path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(stream.is_open()) << "Missing model revision 4 field: " << raw_path.string();
    if (!stream.is_open()) {
        return {};
    }

    std::streamsize const size = stream.tellg();
    EXPECT_GE(size, 0) << "Could not determine revision 4 field length: " << raw_path.string();
    if (size < 0) {
        return {};
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    EXPECT_TRUE(stream.good() || stream.eof()) << "Could not read revision 4 field: "
        << raw_path.string();
    return bytes;
}

template<typename T>
[[nodiscard]]
uint64_t bitsOf(T value)
{
    if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<uint32_t>(value);
    } else if constexpr (std::is_same_v<T, double>) {
        return std::bit_cast<uint64_t>(value);
    } else {
        using Unsigned = std::make_unsigned_t<T>;
        return std::bit_cast<Unsigned>(value);
    }
}

template<typename T>
[[nodiscard]]
std::vector<uint8_t> encodeLittleEndian(std::span<T const> values)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(values.size() * sizeof(T));
    for (T value : values) {
        uint64_t const bits = bitsOf(value);
        for (size_t byte = 0U; byte < sizeof(T); ++byte) {
            bytes.push_back(static_cast<uint8_t>((bits >> (byte * 8U)) & 0xffU));
        }
    }
    return bytes;
}

template<typename T>
void expectAtlasField(std::string const& field_name, std::span<T const> field)
{
    ASSERT_EQ(field.size(), REFERENCE_RESOLUTION * REFERENCE_RESOLUTION)
        << field_name << " has an unexpected full-atlas size";

    std::vector<T> samples;
    samples.reserve(SAMPLE_SIDE * SAMPLE_SIDE);
    for (uint32_t y = 0U; y < SAMPLE_SIDE; ++y) {
        for (uint32_t x = 0U; x < SAMPLE_SIDE; ++x) {
            uint32_t const index = y * SAMPLE_STEP * REFERENCE_RESOLUTION + x * SAMPLE_STEP;
            samples.push_back(field[index]);
        }
    }

    std::vector<uint8_t> const expected = readBytes(field_name);
    std::vector<uint8_t> const actual = encodeLittleEndian<T>(samples);
    ASSERT_EQ(actual.size(), expected.size()) << field_name << " fixture byte length differs";

    for (size_t byte = 0U; byte < actual.size(); ++byte) {
        if (actual[byte] != expected[byte]) {
            size_t const value_index = byte / sizeof(T);
            ADD_FAILURE() << field_name << " differs at sampled atlas (y="
                << value_index / SAMPLE_SIDE << ", x=" << value_index % SAMPLE_SIDE
                << "), byte " << byte % sizeof(T) << ": expected 0x" << std::hex
                << static_cast<uint32_t>(expected[byte]) << " but got 0x"
                << static_cast<uint32_t>(actual[byte]);
            return;
        }
    }
}

template<typename T>
void expectAtlasField(std::string const& field_name, std::vector<T> const& field)
{
    expectAtlasField<T>(field_name, std::span<T const>(field));
}

template<typename T>
void expectRevision4AtlasField(
    uint64_t seed,
    std::string const& field_name,
    std::span<T const> field
)
{
    ASSERT_EQ(field.size(), REFERENCE_RESOLUTION * REFERENCE_RESOLUTION)
        << field_name << " has an unexpected full-atlas size";

    std::vector<T> samples;
    samples.reserve(SAMPLE_SIDE * SAMPLE_SIDE);
    for (uint32_t y = 0U; y < SAMPLE_SIDE; ++y) {
        for (uint32_t x = 0U; x < SAMPLE_SIDE; ++x) {
            uint32_t const index = y * SAMPLE_STEP * REFERENCE_RESOLUTION + x * SAMPLE_STEP;
            samples.push_back(field[index]);
        }
    }

    std::vector<uint8_t> const expected = readRevision4Bytes(seed, field_name);
    std::vector<uint8_t> const actual = encodeLittleEndian<T>(samples);
    ASSERT_EQ(actual.size(), expected.size()) << field_name << " fixture byte length differs";

    for (size_t byte = 0U; byte < actual.size(); ++byte) {
        if (actual[byte] != expected[byte]) {
            size_t const value_index = byte / sizeof(T);
            ADD_FAILURE() << "seed=" << seed << ' ' << field_name
                << " differs at sampled atlas (y="
                << value_index / SAMPLE_SIDE << ", x=" << value_index % SAMPLE_SIDE
                << "), byte " << byte % sizeof(T) << ": expected 0x" << std::hex
                << static_cast<uint32_t>(expected[byte]) << " but got 0x"
                << static_cast<uint32_t>(actual[byte]);
            return;
        }
    }
}

template<typename T>
void expectRevision4AtlasField(
    uint64_t seed,
    std::string const& field_name,
    std::vector<T> const& field
)
{
    expectRevision4AtlasField<T>(seed, field_name, std::span<T const>(field));
}

TEST(TerrainReferenceTests, Seed42FinalAtlasMatchesFrozenRawFieldsExactly)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = REFERENCE_SEED;
    configuration.resolution = REFERENCE_RESOLUTION;

    std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
        configuration
    );
    ASSERT_NE(atlas, nullptr);
    EXPECT_EQ(atlas->configuration, configuration);
    EXPECT_EQ(atlas->configuration.resolution, REFERENCE_RESOLUTION);

    expectAtlasField("atlas__elevation", atlas->elevation);
    expectAtlasField("atlas__tectonic_elevation", atlas->tectonic_elevation);
    expectAtlasField("atlas__filled", atlas->filled);
    expectAtlasField("atlas__water_level", atlas->water_level);
    expectAtlasField("atlas__flow_accumulation", atlas->flow_accumulation);
    expectAtlasField("atlas__downstream", atlas->downstream);
    expectAtlasField("atlas__lake_id", atlas->lake_id);
    expectAtlasField("atlas__outlet_id", atlas->outlet_id);
    expectAtlasField("atlas__plate_id", atlas->plate_id);
    expectAtlasField("atlas__ocean_mask", atlas->ocean_mask);
    expectAtlasField("atlas__lake_mask", atlas->lake_mask);
    expectAtlasField("atlas__river_mask", atlas->river_mask);
    expectAtlasField("atlas__fjord_candidate_mask", atlas->fjord_candidate_mask);
    expectAtlasField("atlas__harbour_candidate_mask", atlas->harbour_candidate_mask);
    expectAtlasField("atlas__temperature", atlas->temperature);
    expectAtlasField("atlas__precipitation", atlas->precipitation);
    expectAtlasField("atlas__moisture", atlas->moisture);
    expectAtlasField("atlas__wind_u", atlas->wind_u);
    expectAtlasField("atlas__wind_v", atlas->wind_v);
    expectAtlasField("atlas__current_u", atlas->current_u);
    expectAtlasField("atlas__current_v", atlas->current_v);
    expectAtlasField("atlas__climate_zone", atlas->climate_zone);
    expectAtlasField("atlas__snow_mask", atlas->snow_mask);
    expectAtlasField("atlas__uplift", atlas->uplift);
    expectAtlasField("atlas__island_arc_uplift", atlas->island_arc_uplift);
    expectAtlasField("atlas__hotspot", atlas->hotspot);
    expectAtlasField("atlas__subduction", atlas->subduction);
    expectAtlasField("atlas__divergence", atlas->divergence);
    expectAtlasField("atlas__continental", atlas->continental);
    expectAtlasField("atlas__plate_vx", atlas->plate_vx);
    expectAtlasField("atlas__plate_vy", atlas->plate_vy);

    std::array<double, 1U> const vertical_scale{atlas->vertical_scale};
    std::vector<uint8_t> const expected_vertical_scale = readBytes("atlas__vertical_scale");
    EXPECT_EQ(encodeLittleEndian<double>(vertical_scale), expected_vertical_scale)
        << "atlas__vertical_scale differs from the frozen binary64 scalar";
}

TEST(TerrainRevision4ReferenceTests, SixSeedSamplesMatchApprovedPortableCandidate)
{
    for (uint64_t const seed : REVISION_4_SEEDS) {
        generation::GenerationConfiguration configuration;
        configuration.seed = seed;
        configuration.resolution = REFERENCE_RESOLUTION;

        std::vector<float> tectonic_elevation;
        std::shared_ptr<generation::GlobalAtlas const> const atlas = generation::prepareAtlas(
            configuration,
            {},
            [&tectonic_elevation, seed](generation::GenerationProgress const& progress) {
                if (seed == REFERENCE_SEED
                    && progress.stage == generation::GenerationStage::Tectonics
                    && progress.snapshot != nullptr) {
                    tectonic_elevation = progress.snapshot->elevation;
                }
            }
        );
        ASSERT_NE(atlas, nullptr) << "seed=" << seed;

        expectRevision4AtlasField(seed, "atlas__elevation", atlas->elevation);
        expectRevision4AtlasField(seed, "atlas__temperature", atlas->temperature);
        expectRevision4AtlasField(seed, "atlas__water_level", atlas->water_level);
        expectRevision4AtlasField(seed, "atlas__ocean_mask", atlas->ocean_mask);
        expectRevision4AtlasField(seed, "atlas__lake_mask", atlas->lake_mask);
        expectRevision4AtlasField(seed, "atlas__river_mask", atlas->river_mask);
        if (seed == REFERENCE_SEED) {
            ASSERT_FALSE(tectonic_elevation.empty());
            expectRevision4AtlasField(
                seed,
                "stage__geology_elevation",
                tectonic_elevation
            );
        }
    }
}

} // namespace
