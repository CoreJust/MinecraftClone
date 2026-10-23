#include "Numerics.hpp"

#include <generation/Generation.hpp>
#include <generation/GenerationStages.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
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

TEST(CoastalTests, Seed42MatchesFrozenPrototypeSamples)
{
    static constexpr uint32_t RESOLUTION = 256U;
    static constexpr uint32_t SAMPLE_COUNT = 5U;
    static constexpr float TOLERANCE = 0.0001F;
    static constexpr uint64_t ELEVATION_HASH = 0xddfc'88b4'a78c'4af7ULL;

    struct Sample
    {
        uint32_t x;
        uint32_t y;
        float elevation;
    };

    static constexpr std::array<Sample, SAMPLE_COUNT> SAMPLES{
        Sample{0U, 0U, -1.435'314'416'885'376F},
        Sample{16U, 16U, -1.025'589'346'885'681'2F},
        Sample{128U, 128U, 0.702'210'843'563'079'8F},
        Sample{240U, 240U, 1.039'703'249'931'335'4F},
        Sample{240U, 0U, -1.577'676'892'280'578'6F},
    };

    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.resolution = RESOLUTION;
    configuration.epochs = 12U;

    generation::detail::TectonicsFields const geology = generation::detail::generateTectonics(
        configuration,
        {},
        {}
    );
    std::vector<float> coast_input = geology.elevation;
    float const sea_shift = generation::detail::medianFloat32(coast_input);
    for (float& value : coast_input) {
        value -= sea_shift;
    }
    std::vector<float> activity(geology.elevation.size());
    for (uint32_t index = 0U; index < activity.size(); ++index) {
        activity[index] = std::clamp(
            (geology.uplift[index] - geology.subduction[index] + std::abs(geology.divergence[index]))
                / 2.5F,
            0.0F,
            1.0F
        );
    }

    generation::detail::CoastalFields const fields = generation::detail::elaborateCoasts(
        configuration,
        coast_input,
        activity,
        {},
        {}
    );

    ASSERT_EQ(fields.elevation.size(), geology.elevation.size());
    ASSERT_EQ(fields.displacement.size(), geology.elevation.size());
    ASSERT_EQ(fields.roughness_potential.size(), geology.elevation.size());
    for (Sample const& sample : SAMPLES) {
        uint32_t const index = sample.y * RESOLUTION + sample.x;
        EXPECT_NEAR(fields.elevation[index], sample.elevation, TOLERANCE);
        EXPECT_TRUE(std::isfinite(fields.displacement[index]));
        EXPECT_TRUE(std::isfinite(fields.roughness_potential[index]));
    }
    EXPECT_EQ(sampleFixtureHash(fields.elevation, RESOLUTION), ELEVATION_HASH);
}

TEST(CoastalTests, DisabledModePreservesElevationAndProducesNoDisplacement)
{
    static constexpr uint32_t RESOLUTION = 8U;
    static constexpr uint32_t FIELD_SIZE = RESOLUTION * RESOLUTION;

    generation::GenerationConfiguration configuration;
    configuration.resolution = RESOLUTION;
    configuration.coastal_elaboration = false;
    std::vector<float> elevation(FIELD_SIZE);
    std::vector<float> activity(FIELD_SIZE);
    for (uint32_t y = 0U; y < RESOLUTION; ++y) {
        for (uint32_t x = 0U; x < RESOLUTION; ++x) {
            uint32_t const index = y * RESOLUTION + x;
            elevation[index] = static_cast<float>(static_cast<int32_t>(x % RESOLUTION) - 4) * 0.3F;
            activity[index] = static_cast<float>(y) / static_cast<float>(RESOLUTION - 1U);
        }
    }

    generation::detail::CoastalFields const fields = generation::detail::elaborateCoasts(
        configuration,
        elevation,
        activity,
        {},
        {}
    );

    EXPECT_EQ(fields.elevation, elevation);
    ASSERT_EQ(fields.displacement.size(), FIELD_SIZE);
    ASSERT_EQ(fields.roughness_potential.size(), FIELD_SIZE);
    EXPECT_TRUE(std::ranges::all_of(fields.displacement, [](float value) {
        return value == 0.0F;
    }));
    EXPECT_TRUE(std::ranges::all_of(fields.roughness_potential, [](float value) {
        return std::isfinite(value) && value > 0.0F;
    }));
}

TEST(CoastalTests, RejectsMismatchedFields)
{
    generation::GenerationConfiguration configuration;
    configuration.resolution = 8U;
    std::vector<float> elevation(64U);
    std::vector<float> activity(63U);

    EXPECT_THROW(
        static_cast<void>(generation::detail::elaborateCoasts(
            configuration,
            elevation,
            activity,
            {},
            {}
        )),
        std::invalid_argument
    );
}

TEST(CoastalTests, RejectsNonFiniteFields)
{
    static constexpr uint32_t RESOLUTION = 8U;
    static constexpr uint32_t FIELD_SIZE = RESOLUTION * RESOLUTION;

    generation::GenerationConfiguration configuration;
    configuration.resolution = RESOLUTION;
    std::vector<float> elevation(FIELD_SIZE);
    std::vector<float> activity(FIELD_SIZE);
    elevation[17U] = std::numeric_limits<float>::quiet_NaN();

    EXPECT_THROW(
        static_cast<void>(generation::detail::elaborateCoasts(
            configuration,
            elevation,
            activity,
            {},
            {}
        )),
        std::invalid_argument
    );
}

TEST(CoastalTests, RejectsCancellationBeforeProgress)
{
    static constexpr uint32_t RESOLUTION = 8U;
    static constexpr uint32_t FIELD_SIZE = RESOLUTION * RESOLUTION;

    generation::GenerationConfiguration configuration;
    configuration.resolution = RESOLUTION;
    std::vector<float> elevation(FIELD_SIZE);
    std::vector<float> activity(FIELD_SIZE);
    std::stop_source cancellation;
    cancellation.request_stop();
    bool progress_received = false;

    EXPECT_THROW(
        static_cast<void>(generation::detail::elaborateCoasts(
            configuration,
            elevation,
            activity,
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
