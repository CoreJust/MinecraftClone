#include "Sha256.hpp"
#include "V9PlatecSource.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

struct SeedBridgeOracle final
{
    uint64_t user_seed = 0U;
    uint64_t selection_value = 0U;
    uint32_t native_seed32 = 0U;
    generation::detail::V9TopologyRegime regime =
        generation::detail::V9TopologyRegime::FragmentedLandforms;
    uint32_t initial_plate_count = 0U;
    float initial_ocean_fraction = 0.0F;
};

struct SourceArrayOracle final
{
    uint64_t user_seed = 0U;
    int32_t final_step = 0;
    std::string height_raw_sha256;
    std::string current_plate_id_sha256;
    std::string final_step_sha256;
};

[[nodiscard]]
std::filesystem::path fixtureDirectory()
{
#ifdef MC_V9_PLATEC_FIXTURE_DIR
    return std::filesystem::path(MC_V9_PLATEC_FIXTURE_DIR);
#else
    return std::filesystem::path(__FILE__).parent_path().parent_path()
        / "fixtures" / "terrain_v9";
#endif
}

[[nodiscard]]
std::vector<SeedBridgeOracle> readSeedBridgeOracles()
{
    std::filesystem::path const path = fixtureDirectory() / "seed_bridge.tsv";
    std::ifstream stream{path};
    EXPECT_TRUE(stream.is_open()) << "Missing v9 seed bridge fixture: " << path.string();
    if (!stream.is_open())
    {
        return {};
    }

    std::vector<SeedBridgeOracle> oracles;
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        SeedBridgeOracle oracle;
        std::string regime;
        std::istringstream row{line};
        row >> oracle.user_seed
            >> oracle.selection_value
            >> oracle.native_seed32
            >> regime
            >> oracle.initial_plate_count
            >> oracle.initial_ocean_fraction;
        EXPECT_TRUE(row.good() || row.eof()) << "Malformed seed bridge row: " << line;
        if (row.fail())
        {
            return {};
        }
        if (regime == "supercontinent-oriented")
        {
            oracle.regime = generation::detail::V9TopologyRegime::SupercontinentOriented;
        }
        else if (regime == "mixed-landmass")
        {
            oracle.regime = generation::detail::V9TopologyRegime::MixedLandmass;
        }
        else if (regime != "fragmented-landforms")
        {
            ADD_FAILURE() << "Unknown topology regime in fixture: " << regime;
            return {};
        }
        oracles.push_back(std::move(oracle));
    }
    return oracles;
}

[[nodiscard]]
std::vector<SourceArrayOracle> readSourceArrayOracles()
{
    std::filesystem::path const path = fixtureDirectory() / "source_oracles.tsv";
    std::ifstream stream{path};
    EXPECT_TRUE(stream.is_open()) << "Missing v9 source-array fixture: " << path.string();
    if (!stream.is_open())
    {
        return {};
    }

    std::vector<SourceArrayOracle> oracles;
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        SourceArrayOracle oracle;
        std::istringstream row{line};
        row >> oracle.user_seed
            >> oracle.final_step
            >> oracle.height_raw_sha256
            >> oracle.current_plate_id_sha256
            >> oracle.final_step_sha256;
        EXPECT_TRUE(row.good() || row.eof()) << "Malformed source-array row: " << line;
        if (row.fail())
        {
            return {};
        }
        oracles.push_back(std::move(oracle));
    }
    return oracles;
}

[[nodiscard]]
std::optional<SeedBridgeOracle> seedBridgeOracle(uint64_t user_seed)
{
    for (SeedBridgeOracle const& oracle : readSeedBridgeOracles())
    {
        if (oracle.user_seed == user_seed)
        {
            return oracle;
        }
    }
    return std::nullopt;
}

[[nodiscard]]
std::optional<SourceArrayOracle> sourceArrayOracle(uint64_t user_seed)
{
    for (SourceArrayOracle const& oracle : readSourceArrayOracles())
    {
        if (oracle.user_seed == user_seed)
        {
            return oracle;
        }
    }
    return std::nullopt;
}

[[nodiscard]]
std::string digestHex(std::span<uint8_t const> bytes)
{
    static constexpr std::array<char, 16U> HEX_DIGITS{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::array<uint8_t, 32U> const digest = generation::detail::sha256Digest(bytes);
    std::string result;
    result.reserve(digest.size() * 2U);
    for (uint8_t const value : digest)
    {
        result.push_back(HEX_DIGITS[static_cast<std::array<char, 16U>::size_type>(value >> 4U)]);
        result.push_back(HEX_DIGITS[static_cast<std::array<char, 16U>::size_type>(value & 0x0fU)]);
    }
    return result;
}

template<typename T>
void appendLittleEndian(std::vector<uint8_t>& bytes, T value)
{
    static_assert(sizeof(T) == 4U);
    uint32_t bits = 0U;
    if constexpr (std::is_same_v<T, float>)
    {
        bits = std::bit_cast<uint32_t>(value);
    }
    else
    {
        bits = static_cast<uint32_t>(value);
    }
    for (uint32_t byte = 0U; byte < 4U; ++byte)
    {
        bytes.push_back(static_cast<uint8_t>(bits >> (byte * 8U)));
    }
}

template<typename T>
[[nodiscard]]
std::string canonicalArrayDigest(std::string_view dtype, std::span<T const> values)
{
    std::vector<uint8_t> bytes;
    std::string const prefix = std::string{dtype} + "[256, 256]";
    bytes.reserve(prefix.size() + values.size() * sizeof(T));
    for (char const character : prefix)
    {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(character)));
    }
    for (T const value : values)
    {
        appendLittleEndian(bytes, value);
    }
    return digestHex(bytes);
}

[[nodiscard]]
std::string canonicalFinalStepDigest(int32_t final_step)
{
    // The frozen helper's ascontiguousarray turns a scalar into shape [1].
    std::vector<uint8_t> bytes{
        static_cast<uint8_t>('i'),
        static_cast<uint8_t>('n'),
        static_cast<uint8_t>('t'),
        static_cast<uint8_t>('3'),
        static_cast<uint8_t>('2'),
        static_cast<uint8_t>('['),
        static_cast<uint8_t>('1'),
        static_cast<uint8_t>(']'),
    };
    appendLittleEndian(bytes, final_step);
    return digestHex(bytes);
}

void expectSourceOracle(uint64_t user_seed)
{
    std::optional<SourceArrayOracle> const oracle = sourceArrayOracle(user_seed);
    ASSERT_TRUE(oracle.has_value()) << "No raw-array oracle for seed " << user_seed;

    std::vector<generation::detail::V9PlatecProgress> progress;
    std::expected<generation::detail::V9PlatecSourceResult, generation::detail::V9PlatecFailure> const result =
        generation::detail::generateV9PlatecSource(
            user_seed,
            {},
            [&progress](generation::detail::V9PlatecProgress update)
            {
                progress.push_back(update);
            }
        );
    ASSERT_TRUE(result.has_value()) << "Native Platec stage failed for seed " << user_seed;

    EXPECT_EQ(result->final_step, oracle->final_step);
    EXPECT_EQ(
        result->height_raw.size(),
        generation::detail::V9_PLATEC_SOURCE_RESOLUTION * generation::detail::V9_PLATEC_SOURCE_RESOLUTION
    );
    EXPECT_EQ(result->current_plate_id.size(), result->height_raw.size());
    EXPECT_EQ(
        canonicalArrayDigest<float>("float32", result->height_raw),
        oracle->height_raw_sha256
    );
    EXPECT_EQ(
        canonicalArrayDigest<uint32_t>("uint32", result->current_plate_id),
        oracle->current_plate_id_sha256
    );
    EXPECT_EQ(canonicalFinalStepDigest(result->final_step), oracle->final_step_sha256);

    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.front().steps_completed, 0U);
    EXPECT_EQ(progress.front().step_limit, generation::detail::V9_PLATEC_SOURCE_STEP_LIMIT);
    EXPECT_FALSE(progress.front().complete);
    for (uint64_t index = 1U; index < progress.size(); ++index)
    {
        generation::detail::V9PlatecProgress const& previous =
            progress[static_cast<std::vector<generation::detail::V9PlatecProgress>::size_type>(index - 1U)];
        generation::detail::V9PlatecProgress const& current =
            progress[static_cast<std::vector<generation::detail::V9PlatecProgress>::size_type>(index)];
        EXPECT_GT(current.steps_completed, previous.steps_completed);
        EXPECT_LE(current.steps_completed, current.step_limit);
        EXPECT_EQ(current.step_limit, generation::detail::V9_PLATEC_SOURCE_STEP_LIMIT);
    }
    EXPECT_TRUE(progress.back().complete);
    EXPECT_EQ(progress.back().steps_completed, static_cast<uint32_t>(result->final_step));

    generation::detail::V9PlateDirectionSnapshot const& direction_snapshot =
        result->plate_direction_snapshot;
    EXPECT_EQ(
        direction_snapshot.phase,
        generation::detail::V9PlateDirectionSnapshotPhase::ActiveBeforeTerminalRestart
    );
    EXPECT_EQ(
        direction_snapshot.observed_after_native_step,
        static_cast<uint32_t>(result->final_step - 1)
    );
    std::vector<generation::detail::V9PlateDirection> const& plate_directions =
        direction_snapshot.directions;
    ASSERT_FALSE(plate_directions.empty());
    EXPECT_LE(
        plate_directions.size(),
        result->parameters.initial_plate_count
    );
    EXPECT_TRUE(std::all_of(
        result->current_plate_id.begin(),
        result->current_plate_id.end(),
        [&plate_directions](uint32_t plate_id)
        {
            return plate_id < plate_directions.size();
        }
    ));
    bool has_nonzero_direction = false;
    for (uint32_t index = 0U; index < plate_directions.size(); ++index)
    {
        generation::detail::V9PlateDirection const& direction =
            plate_directions[static_cast<std::vector<generation::detail::V9PlateDirection>::size_type>(index)];
        EXPECT_EQ(direction.plate_id, index);
        EXPECT_TRUE(std::isfinite(direction.x));
        EXPECT_TRUE(std::isfinite(direction.y));
        has_nonzero_direction = has_nonzero_direction || direction.x != 0.0F || direction.y != 0.0F;
    }
    EXPECT_TRUE(has_nonzero_direction);
}

TEST(V9PlatecStageTests, Sha256MatchesStandardAndDomainSeparatedVectors)
{
    static constexpr std::array<uint8_t, 3U> ABC_BYTES{'a', 'b', 'c'};
    static constexpr std::string_view EXPECTED_ABC_SHA256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    EXPECT_EQ(digestHex(ABC_BYTES), EXPECTED_ABC_SHA256);
}

TEST(V9PlatecStageTests, FullUint64SeedBridgeMatchesFrozenVectors)
{
    std::vector<SeedBridgeOracle> const oracles = readSeedBridgeOracles();
    ASSERT_EQ(oracles.size(), 8U);

    for (SeedBridgeOracle const& oracle : oracles)
    {
        generation::detail::V9PlatecParameters const parameters =
            generation::detail::deriveV9PlatecParameters(oracle.user_seed);
        EXPECT_EQ(parameters.topology_selection_value, oracle.selection_value)
            << "seed=" << oracle.user_seed;
        EXPECT_EQ(parameters.native_engine_seed32, oracle.native_seed32)
            << "seed=" << oracle.user_seed;
        EXPECT_EQ(parameters.regime, oracle.regime) << "seed=" << oracle.user_seed;
        EXPECT_EQ(parameters.initial_plate_count, oracle.initial_plate_count)
            << "seed=" << oracle.user_seed;
        EXPECT_FLOAT_EQ(parameters.initial_ocean_fraction, oracle.initial_ocean_fraction)
            << "seed=" << oracle.user_seed;
    }
}

TEST(V9PlatecStageTests, Seed10MatchesRawNativePlatecOracle)
{
    static constexpr uint64_t USER_SEED = 10U;
    expectSourceOracle(USER_SEED);
}

TEST(V9PlatecStageTests, Seed2MatchesRawNativePlatecOracle)
{
    static constexpr uint64_t USER_SEED = 2U;
    expectSourceOracle(USER_SEED);
}

TEST(V9PlatecStageTests, Seed42MatchesRawNativePlatecOracle)
{
    static constexpr uint64_t USER_SEED = 42U;
    expectSourceOracle(USER_SEED);
}

TEST(V9PlatecStageTests, PreRequestedCancellationStopsBeforeNativeWork)
{
    static constexpr uint64_t USER_SEED = 42U;
    std::stop_source cancellation;
    cancellation.request_stop();
    uint32_t progress_callback_count = 0U;

    std::expected<generation::detail::V9PlatecSourceResult, generation::detail::V9PlatecFailure> const result =
        generation::detail::generateV9PlatecSource(
            USER_SEED,
            cancellation.get_token(),
            [&progress_callback_count](generation::detail::V9PlatecProgress)
            {
                ++progress_callback_count;
            }
        );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), generation::detail::V9PlatecFailure::Cancelled);
    EXPECT_EQ(progress_callback_count, 0U);
}

TEST(V9PlatecStageTests, CancellationAfterOneStepReturnsNoPartialStage)
{
    static constexpr uint64_t USER_SEED = 42U;
    std::stop_source cancellation;
    std::vector<generation::detail::V9PlatecProgress> progress;

    std::expected<generation::detail::V9PlatecSourceResult, generation::detail::V9PlatecFailure> const result =
        generation::detail::generateV9PlatecSource(
            USER_SEED,
            cancellation.get_token(),
            [&cancellation, &progress](generation::detail::V9PlatecProgress update)
            {
                progress.push_back(update);
                if (update.steps_completed == 1U)
                {
                    cancellation.request_stop();
                }
            }
        );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), generation::detail::V9PlatecFailure::Cancelled);
    ASSERT_EQ(progress.size(), 2U);
    EXPECT_EQ(progress.back().steps_completed, 1U);
    EXPECT_FALSE(progress.back().complete);
}

} // namespace
