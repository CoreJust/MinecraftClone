#include <generation/GenerationStages.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace
{

uint64_t appendLittleEndian(uint64_t hash, uint32_t bits)
{
    static constexpr uint32_t BYTE_COUNT = 4U;
    static constexpr uint32_t BYTE_BITS = 8U;
    static constexpr uint64_t FNV_PRIME = 1'099'511'628'211ULL;
    for (uint32_t byte = 0U; byte < BYTE_COUNT; ++byte) {
        hash ^= (bits >> (byte * BYTE_BITS)) & 0xffU;
        hash *= FNV_PRIME;
    }
    return hash;
}

uint64_t sampleFixtureHash(std::span<float const> values, uint32_t resolution)
{
    static constexpr uint32_t SAMPLE_STEP = 16U;
    static constexpr uint64_t FNV_OFFSET = 14'695'981'039'346'656'037ULL;
    uint64_t hash = FNV_OFFSET;
    for (uint32_t y = 0U; y < SAMPLE_STEP; ++y) {
        for (uint32_t x = 0U; x < SAMPLE_STEP; ++x) {
            uint32_t const index = y * SAMPLE_STEP * resolution + x * SAMPLE_STEP;
            hash = appendLittleEndian(hash, std::bit_cast<uint32_t>(values[index]));
        }
    }
    return hash;
}

uint64_t sampleFixtureHash(std::span<int16_t const> values, uint32_t resolution)
{
    static constexpr uint32_t SAMPLE_STEP = 16U;
    static constexpr uint64_t FNV_OFFSET = 14'695'981'039'346'656'037ULL;
    uint64_t hash = FNV_OFFSET;
    for (uint32_t y = 0U; y < SAMPLE_STEP; ++y) {
        for (uint32_t x = 0U; x < SAMPLE_STEP; ++x) {
            uint32_t const index = y * SAMPLE_STEP * resolution + x * SAMPLE_STEP;
            uint32_t const bits = static_cast<uint32_t>(static_cast<int32_t>(values[index]));
            hash = appendLittleEndian(hash, bits);
        }
    }
    return hash;
}

TEST(GeologyTests, Seed42MatchesFrozenPrototypeSamples)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t SAMPLE_COUNT = 5U;
    static constexpr float TOLERANCE = 0.0001F;
    static constexpr uint64_t ELEVATION_HASH = 0x9d3a'c06c'700e'5fb8ULL;
    static constexpr uint64_t UPLIFT_HASH = 0x9bfd'ea97'5da3'df2dULL;
    static constexpr uint64_t SUBDUCTION_HASH = 0x30d0'ab71'8e1d'2149ULL;
    static constexpr uint64_t DIVERGENCE_HASH = 0xe3b7'6215'7f4f'f4ceULL;
    static constexpr uint64_t CONTINENTAL_HASH = 0x8656'ff6e'bc56'7598ULL;
    static constexpr uint64_t PLATE_ID_HASH = 0xc410'7dbb'09b5'e848ULL;

    struct Sample
    {
        uint32_t x;
        uint32_t y;
        float elevation;
        float uplift;
        float subduction;
        float divergence;
        float continental;
        int16_t plate_id;
    };

    static constexpr std::array<Sample, SAMPLE_COUNT> SAMPLES{
        Sample{0U, 0U, -1.435'314'416'885'376F, 9.144'227'988'144'848e-7F,
            -3.326'826'799'820'992e-6F, 0.746'137'917'041'778'6F, 0.117'943'935'096'263'89F, 2},
        Sample{16U, 16U, -1.057'239'532'470'703'1F, 3.336'429'654'154'926'5e-5F,
            -0.011'307'503'096'759'32F, 0.456'622'004'508'972'17F, 0.261'491'090'059'280'4F, 13},
        Sample{128U, 128U, 0.437'185'764'312'744'14F, 0.098'793'812'096'118'93F,
            -0.000'431'615'306'297'317'15F, -0.020'759'657'025'337'22F, 0.610'469'579'696'655'3F, 4},
        Sample{240U, 240U, 0.904'360'234'737'396'2F, 0.114'040'270'447'731'02F,
            -5.311'329'687'174'293e-7F, -0.010'595'723'055'303'097F, 0.695'406'675'338'745'1F, 2},
        Sample{240U, 0U, -0.028'630'614'280'700'684F, 0.311'972'856'521'606'45F,
            -0.041'007'369'756'698'61F, -0.090'052'813'291'549'68F, 0.518'905'520'439'148F, 2},
    };

    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.resolution = RESOLUTION;
    configuration.epochs = 12U;

    std::vector<uint32_t> completed;
    generation::detail::TectonicsFields const fields = generation::detail::generateTectonics(
        configuration,
        {},
        [&completed](generation::GenerationProgress const& progress) {
            completed.push_back(progress.completed);
        }
    );
    std::vector<float> centered_elevation = fields.elevation;
    float const sea_shift = generation::detail::medianFloat32(centered_elevation);
    for (float& value : centered_elevation) {
        value -= sea_shift;
    }

    uint32_t const FIELD_SIZE = RESOLUTION * RESOLUTION;
    ASSERT_EQ(fields.elevation.size(), FIELD_SIZE);
    ASSERT_EQ(fields.uplift.size(), FIELD_SIZE);
    ASSERT_EQ(fields.subduction.size(), FIELD_SIZE);
    ASSERT_EQ(fields.divergence.size(), FIELD_SIZE);
    ASSERT_EQ(fields.continental.size(), FIELD_SIZE);
    ASSERT_EQ(fields.plate_id.size(), FIELD_SIZE);
    ASSERT_FALSE(completed.empty());
    EXPECT_EQ(completed.back(), configuration.epochs);

    for (Sample const& sample : SAMPLES) {
        uint32_t const index = sample.y * RESOLUTION + sample.x;
        EXPECT_NEAR(centered_elevation[index], sample.elevation, TOLERANCE);
        EXPECT_NEAR(fields.uplift[index], sample.uplift, TOLERANCE);
        EXPECT_NEAR(fields.subduction[index], sample.subduction, TOLERANCE);
        EXPECT_NEAR(fields.divergence[index], sample.divergence, TOLERANCE);
        EXPECT_NEAR(fields.continental[index], sample.continental, TOLERANCE);
        EXPECT_EQ(fields.plate_id[index], sample.plate_id);
    }
    EXPECT_EQ(sampleFixtureHash(centered_elevation, RESOLUTION), ELEVATION_HASH);
    EXPECT_EQ(sampleFixtureHash(fields.uplift, RESOLUTION), UPLIFT_HASH);
    EXPECT_EQ(sampleFixtureHash(fields.subduction, RESOLUTION), SUBDUCTION_HASH);
    EXPECT_EQ(sampleFixtureHash(fields.divergence, RESOLUTION), DIVERGENCE_HASH);
    EXPECT_EQ(sampleFixtureHash(fields.continental, RESOLUTION), CONTINENTAL_HASH);
    EXPECT_EQ(sampleFixtureHash(fields.plate_id, RESOLUTION), PLATE_ID_HASH);
}

TEST(GeologyTests, RejectsUnsupportedResolutionBeforeProgress)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = 7U;
    bool progress_received = false;

    EXPECT_THROW(
        static_cast<void>(generation::detail::generateTectonics(
            configuration,
            {},
            [&progress_received](generation::GenerationProgress const&) {
                progress_received = true;
            }
        )),
        std::invalid_argument
    );
    EXPECT_FALSE(progress_received);
}

TEST(GeologyTests, RejectsCancellationBeforeProgress)
{
    generation::GenerationConfiguration configuration;
    std::stop_source cancellation;
    cancellation.request_stop();
    bool progress_received = false;

    EXPECT_THROW(
        static_cast<void>(generation::detail::generateTectonics(
            configuration,
            cancellation.get_token(),
            [&progress_received](generation::GenerationProgress const&) {
                progress_received = true;
            }
        )),
        generation::GenerationCancelled
    );
    EXPECT_FALSE(progress_received);
}

} // namespace
