#include "V10Foundation.hpp"
#include "V10PlatecSource.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#ifndef MC_V10_FIXTURE_DIRECTORY
#define MC_V10_FIXTURE_DIRECTORY ""
#endif

namespace
{
using namespace generation::detail;

std::vector<float> readRawFixture(std::filesystem::path const& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open V10 raw fixture");
    }
    std::vector<float> raw;
    while (input.peek() != std::char_traits<char>::eof())
    {
        uint32_t bits = 0U;
        for (uint32_t byte = 0U; byte < 4U; ++byte)
        {
            auto const value = input.get();
            if (value == std::char_traits<char>::eof())
            {
                throw std::runtime_error("truncated V10 raw fixture");
            }
            bits |= static_cast<uint32_t>(static_cast<unsigned char>(value)) << (8U * byte);
        }
        raw.push_back(std::bit_cast<float>(bits));
    }
    return raw;
}

std::string number(double value)
{
    std::ostringstream result;
    result << std::setprecision(17) << value;
    return result.str();
}

} // namespace

TEST(V10PlatecSourceTests, RejectsUnsupportedSeedWithoutHashingIt)
{
    try
    {
        static_cast<void>(generateV10PlatecSource(43U));
        FAIL() << "unsupported seed accepted";
    }
    catch (std::invalid_argument const& error)
    {
        EXPECT_STREQ(error.what(), "V10 Platec source requires seed 42");
    }
}

TEST(V10PlatecSourceTests, CancellationBeforeConstructionReturnsNoSource)
{
    std::stop_source cancellation;
    cancellation.request_stop();
    auto const result = generateV10PlatecSource(42U, cancellation.get_token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), V10PlatecFailure::Cancelled);
}

TEST(V10PlatecSourceTests, InitialOwnedProgressCanCancelBeforeFirstUpdate)
{
    std::stop_source cancellation;
    std::shared_ptr<V10PlatecSourceResult const> retained;
    auto const result = generateV10PlatecSource(42U, cancellation.get_token(), [&](V10PlatecProgress update) {
        EXPECT_EQ(update.steps_completed, 0U);
        EXPECT_FALSE(update.complete);
        retained = std::move(update.snapshot);
        cancellation.request_stop();
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), V10PlatecFailure::Cancelled);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->height_raw.size(), 256U * 256U);
    EXPECT_EQ(retained->current_plate_id.size(), 256U * 256U);
    EXPECT_EQ(retained->final_step, 0);
    EXPECT_EQ(retained->plate_direction_snapshot.observed_after_native_step, 0U);
    ASSERT_EQ(retained->plate_direction_snapshot.directions.size(), 10U);
    for (auto const& direction : retained->plate_direction_snapshot.directions)
    {
        EXPECT_TRUE(std::isfinite(direction.x));
        EXPECT_TRUE(std::isfinite(direction.y));
        EXPECT_GT(std::hypot(direction.x, direction.y), 0.0F);
    }
}

TEST(V10PlatecSourceTests, GeneratedRawMatchesFrozenReferenceAndMountainWitnesses)
{
    std::filesystem::path const directory{MC_V10_FIXTURE_DIRECTORY};
    ASSERT_FALSE(directory.empty());
    auto const expected = readRawFixture(directory / "raw-height.f32le");
    uint32_t last_step = 0U;
    bool terminal_seen = false;
    std::shared_ptr<V10PlatecSourceResult const> terminal;
    auto const result = generateV10PlatecSource(42U, {}, [&](V10PlatecProgress update) {
        EXPECT_GE(update.steps_completed, last_step);
        EXPECT_LE(update.steps_completed, V10_PLATEC_SOURCE_STEP_LIMIT);
        last_step = update.steps_completed;
        if (update.complete)
        {
            terminal_seen = true;
            terminal = std::move(update.snapshot);
        }
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(terminal_seen);
    ASSERT_NE(terminal, nullptr);
    EXPECT_EQ(terminal->height_raw, result->height_raw);
    auto const& motion = result->plate_direction_snapshot;
    EXPECT_EQ(motion.phase, V10PlateDirectionSnapshotPhase::ActiveBeforeTerminalRestart);
    ASSERT_GT(result->final_step, 0);
    EXPECT_EQ(motion.observed_after_native_step, static_cast<uint32_t>(result->final_step - 1));
    ASSERT_FALSE(motion.directions.empty());
    EXPECT_EQ(terminal->plate_direction_snapshot.observed_after_native_step, motion.observed_after_native_step);
    ASSERT_EQ(terminal->plate_direction_snapshot.directions.size(), motion.directions.size());
    for (size_t i = 0U; i < motion.directions.size(); ++i)
    {
        auto const& direction = motion.directions[i];
        EXPECT_EQ(direction.plate_id, static_cast<uint32_t>(i));
        EXPECT_TRUE(std::isfinite(direction.x));
        EXPECT_TRUE(std::isfinite(direction.y));
        EXPECT_GT(std::hypot(direction.x, direction.y), 0.0F);
        EXPECT_EQ(terminal->plate_direction_snapshot.directions[i].x, direction.x);
        EXPECT_EQ(terminal->plate_direction_snapshot.directions[i].y, direction.y);
    }
    RecordProperty("motion_observed_after_native_step", static_cast<int32_t>(motion.observed_after_native_step));
    RecordProperty("motion_plate_count", static_cast<int32_t>(motion.directions.size()));
    RecordProperty("motion_first_x", number(motion.directions.front().x));
    RecordProperty("motion_first_y", number(motion.directions.front().y));
    ASSERT_EQ(result->height_raw.size(), 256U * 256U);
    ASSERT_EQ(result->height_raw.size(), expected.size());
    size_t mismatches = 0U;
    double max_error = 0.0;
    for (size_t i = 0U; i < expected.size(); ++i)
    {
        ASSERT_TRUE(std::isfinite(result->height_raw[i]));
        ASSERT_GT(result->height_raw[i], 0.0F);
        if (std::bit_cast<uint32_t>(result->height_raw[i]) != std::bit_cast<uint32_t>(expected[i]))
        {
            ++mismatches;
        }
        max_error = std::max(max_error,
            std::abs(static_cast<double>(result->height_raw[i]) - expected[i]));
    }
    RecordProperty("raw_exact_mismatches", static_cast<int32_t>(mismatches));
    RecordProperty("raw_max_absolute_error", number(max_error));
    RecordProperty("source_steps", result->final_step);
    EXPECT_EQ(mismatches, 0U);
    auto const basis = prepareV10Foundation(42U, result->height_raw);
    double const summit = sampleV10FoundationHeight(basis, 46'336.0, 9984.0);
    double const fine_peak = sampleV10FoundationHeight(basis, 46'357.0, 10'018.0);
    RecordProperty("summit_m", number(summit));
    RecordProperty("fine_peak_m", number(fine_peak));
    EXPECT_NEAR(summit, 723.342'269'667'154'6, 1.0);
    EXPECT_NEAR(fine_peak, 726.704'491'978'525'8, 1.0);
#ifdef MC_V10_SOURCE_EVIDENCE_DIRECTORY
    std::ofstream raw_output(std::filesystem::path{MC_V10_SOURCE_EVIDENCE_DIRECTORY} / "direct-v10-raw.f32le",
        std::ios::binary);
    ASSERT_TRUE(raw_output.is_open());
    for (float const height : result->height_raw)
    {
        uint32_t const bits = std::bit_cast<uint32_t>(height);
        for (uint32_t byte = 0U; byte < 4U; ++byte)
        {
            raw_output.put(static_cast<char>((bits >> (8U * byte)) & 0xffU));
        }
    }
    ASSERT_TRUE(raw_output.good());
#endif
}

TEST(V10PlatecSourceTests, CancellationDuringUpdatesKeepsOwnedInitialMotion)
{
    std::stop_source cancellation;
    std::shared_ptr<V10PlatecSourceResult const> initial;
    float initial_x = 0.0F, initial_y = 0.0F;
    auto const result = generateV10PlatecSource(42U, cancellation.get_token(), [&](V10PlatecProgress update) {
        if (update.steps_completed == 0U)
        {
            initial = std::move(update.snapshot);
            ASSERT_NE(initial, nullptr);
            ASSERT_FALSE(initial->plate_direction_snapshot.directions.empty());
            initial_x = initial->plate_direction_snapshot.directions.front().x;
            initial_y = initial->plate_direction_snapshot.directions.front().y;
        }
        else
        {
            cancellation.request_stop();
        }
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), V10PlatecFailure::Cancelled);
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->plate_direction_snapshot.observed_after_native_step, 0U);
    ASSERT_EQ(initial->plate_direction_snapshot.directions.size(), 10U);
    EXPECT_EQ(initial->plate_direction_snapshot.directions.front().x, initial_x);
    EXPECT_EQ(initial->plate_direction_snapshot.directions.front().y, initial_y);
    EXPECT_GT(std::hypot(initial_x, initial_y), 0.0F);
}
