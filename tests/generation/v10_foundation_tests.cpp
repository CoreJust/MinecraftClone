#include "V10Foundation.hpp"

#include "../../third_party/v10_perlin/perlin2.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MC_V10_FIXTURE_DIRECTORY
#define MC_V10_FIXTURE_DIRECTORY ""
#endif

namespace
{
using namespace generation::detail;
constexpr size_t SIDE = V10_FOUNDATION_SIDE;
constexpr size_t COUNT = SIDE * SIDE;

std::vector<float> halfLandSource()
{
    std::vector<float> raw(COUNT);
    for (size_t y = 0U; y < SIDE; ++y)
    {
        for (size_t x = 0U; x < SIDE; ++x)
        {
            raw[y * SIDE + x] = x < SIDE / 2U ? 2.0F : 0.5F;
        }
    }
    return raw;
}

template<typename T, typename Bits>
std::vector<T> readLittleEndian(std::filesystem::path const& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot open V10 numeric fixture");
    }
    std::vector<T> result;
    while (input.peek() != std::char_traits<char>::eof())
    {
        Bits bits = 0U;
        for (size_t byte = 0U; byte < sizeof(T); ++byte)
        {
            auto const value = input.get();
            if (value == std::char_traits<char>::eof())
            {
                throw std::runtime_error("truncated V10 numeric fixture");
            }
            bits |= static_cast<Bits>(static_cast<unsigned char>(value)) << (8U * byte);
        }
        result.push_back(std::bit_cast<T>(bits));
    }
    return result;
}

} // namespace

TEST(V10FoundationTests, RejectsUnregisteredSeed)
{
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(43U, halfLandSource())), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsWrongAtlasSize)
{
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, std::vector<float>(COUNT - 1U, 2.0F))), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsZeroRawHeight)
{
    auto raw = halfLandSource();
    raw[COUNT / 2U] = 0.0F;
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, raw)), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsNegativeRawHeight)
{
    auto raw = halfLandSource();
    raw[COUNT / 2U] = -1.0F;
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, raw)), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsNaNRawHeight)
{
    auto raw = halfLandSource();
    raw[COUNT / 2U] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, raw)), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsInfiniteRawHeight)
{
    auto raw = halfLandSource();
    raw[COUNT / 2U] = std::numeric_limits<float>::infinity();
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, raw)), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsSourceWithoutWetSupport)
{
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, std::vector<float>(COUNT, 2.0F))), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsSourceWithoutLandSupport)
{
    EXPECT_THROW(static_cast<void>(prepareV10Foundation(42U, std::vector<float>(COUNT, 1.0F))), std::invalid_argument);
}

TEST(V10FoundationTests, NormalizedSupportPreservesConstantMagnitudesAcrossPeriodicCoasts)
{
    auto const basis = prepareV10Foundation(42U, halfLandSource());
    double const u = std::log(2.0);
    double const magnitude = u * (70.0 + 60.0 * u);
    for (size_t i = 0U; i < COUNT; ++i)
    {
        ASSERT_NEAR(basis.land_magnitude_m[i], magnitude, 1e-9) << i;
        ASSERT_NEAR(basis.ocean_magnitude_m[i], magnitude, 1e-9) << i;
        ASSERT_NEAR(basis.regional_relief_m[i], 0.0, 1e-9) << i;
    }
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[0U], 128.0);
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[127U], 128.0);
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[128U], -128.0);
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[255U], -128.0);
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[64U], (64.0 - 0.5) * 256.0);
}

TEST(V10FoundationTests, BasisSamplesUsePositivePeriodicCoordinatesAndContinuousSeam)
{
    V10FoundationBasis basis;
    std::array<std::vector<double>*, 4> fields = {
        &basis.land_magnitude_m, &basis.ocean_magnitude_m,
        &basis.coast_distance_m, &basis.regional_relief_m,
    };
    for (size_t f = 0U; f < fields.size(); ++f)
    {
        fields[f]->resize(COUNT);
        for (size_t i = 0U; i < COUNT; ++i)
        {
            (*fields[f])[i] = static_cast<double>(i + f);
        }
    }
    auto const negative = sampleV10FoundationBasis(basis, -128.0, -64.0);
    auto const positive = sampleV10FoundationBasis(basis, 65'536.0 - 128.0, 65'536.0 - 64.0);
    EXPECT_EQ(negative, positive);
    EXPECT_EQ(sampleV10FoundationBasis(basis, 0.0, 0.0),
        sampleV10FoundationBasis(basis, -65'536.0, 2.0 * 65'536.0));
    // Halfway across the final X interval interpolates the last and first nodes.
    auto const seam = sampleV10FoundationBasis(basis, -128.0, 0.0);
    for (size_t f = 0U; f < fields.size(); ++f)
    {
        EXPECT_DOUBLE_EQ(seam[f], (255.0 + 2.0 * static_cast<double>(f)) / 2.0);
    }
}

TEST(V10FoundationTests, RejectsNonfiniteSampleCoordinates)
{
    V10FoundationBasis const basis;
    EXPECT_THROW(static_cast<void>(sampleV10FoundationBasis(
        basis, std::numeric_limits<double>::infinity(), 0.0
    )), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(sampleV10FoundationBasis(
        basis, 0.0, std::numeric_limits<double>::quiet_NaN()
    )), std::invalid_argument);
}

TEST(V10FoundationTests, RejectsMalformedBasisInsteadOfReadingOutsideItsFields)
{
    V10FoundationBasis const basis;
    EXPECT_THROW(static_cast<void>(sampleV10FoundationBasis(basis, 0.0, 0.0)), std::invalid_argument);
}

TEST(V10FoundationTests, NativeRawPreparationMatchesEveryPinnedSeed42BasisValue)
{
    std::filesystem::path const directory{MC_V10_FIXTURE_DIRECTORY};
    ASSERT_FALSE(directory.empty()) << "Set MC_V10_FIXTURE_DIRECTORY to the hash-checked MC-AI-0649 fixture bundle";
    auto const raw = readLittleEndian<float, uint32_t>(directory / "raw-height.f32le");
    auto const expected = readLittleEndian<double, uint64_t>(directory / "basis-L-O-d-R2.f64le");
    ASSERT_EQ(raw.size(), COUNT);
    ASSERT_EQ(expected.size(), 4U * COUNT);
    auto const basis = prepareV10Foundation(42U, raw);
    std::array<std::vector<double> const*, 4> const fields = {
        &basis.land_magnitude_m, &basis.ocean_magnitude_m,
        &basis.coast_distance_m, &basis.regional_relief_m,
    };
    for (size_t field = 0U; field < fields.size(); ++field)
    {
        double max_error = 0.0;
        size_t worst_index = 0U;
        for (size_t i = 0U; i < COUNT; ++i)
        {
            ASSERT_TRUE(std::isfinite((*fields[field])[i])) << field << ':' << i;
            double const error = std::abs((*fields[field])[i] - expected[field * COUNT + i]);
            if (error > max_error)
            {
                max_error = error;
                worst_index = i;
            }
        }
        std::ostringstream diagnostic;
        diagnostic << std::setprecision(17) << max_error;
        RecordProperty("basis_field_" + std::to_string(field) + "_max_absolute_error_m", diagnostic.str());
        RecordProperty("basis_field_" + std::to_string(field) + "_worst_index", static_cast<int32_t>(worst_index));
        EXPECT_LE(max_error, 1e-9) << "field L/O/d/R2=" << field << " index=" << worst_index;
    }
}

TEST(V10FoundationTests, RawSeaLevelIsWetAndDoesNotMoveTheHalfCellCoast)
{
    auto raw = halfLandSource();
    raw[127U] = 1.0F;
    auto const basis = prepareV10Foundation(42U, raw);
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[127U], -128.0);
    EXPECT_DOUBLE_EQ(basis.coast_distance_m[126U], 128.0);
}

TEST(V10FoundationTests, GeneratedBasisPointQueriesMatchAllFrozenHeightVectors)
{
    std::filesystem::path const directory{MC_V10_FIXTURE_DIRECTORY};
    ASSERT_FALSE(directory.empty());
    auto const raw = readLittleEndian<float, uint32_t>(directory / "raw-height.f32le");
    auto const basis = prepareV10Foundation(42U, raw);
    std::ifstream input(directory / "height-reference.tsv");
    ASSERT_TRUE(input.is_open());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
    size_t count = 0U;
    size_t failures = 0U;
    double max_error = 0.0;
    double worst_x = 0.0;
    double worst_y = 0.0;
    while (std::getline(input, line))
    {
        std::istringstream row(line);
        std::string name;
        double x = 0.0, y = 0.0, expected = 0.0;
        ASSERT_TRUE(static_cast<bool>(row >> name >> x >> y >> expected));
        double const actual = sampleV10FoundationHeight(basis, x, y);
        ASSERT_TRUE(std::isfinite(actual)) << name << ':' << x << ',' << y;
        double const error = std::abs(actual - expected);
        if (error > max_error)
        {
            max_error = error;
            worst_x = x;
            worst_y = y;
        }
        if (error > 1e-9)
        {
            ++failures;
        }
        ++count;
    }
    EXPECT_EQ(count, 94'747U);
    EXPECT_EQ(failures, 0U) << worst_x << ',' << worst_y;
    std::ostringstream diagnostic;
    diagnostic << std::setprecision(17) << max_error;
    RecordProperty("height_max_absolute_error_m", diagnostic.str());
    RecordProperty("height_vector_count", static_cast<int32_t>(count));
    diagnostic.str("");
    diagnostic << std::setprecision(17) << worst_x << ',' << worst_y;
    RecordProperty("height_worst_xy_m", diagnostic.str());
    double const summit = sampleV10FoundationHeight(basis, 46'336.0, 9984.0);
    double const fine_peak = sampleV10FoundationHeight(basis, 46'357.0, 10'018.0);
    EXPECT_NEAR(summit, 723.342'269'667'154'6, 1e-9);
    EXPECT_NEAR(fine_peak, 726.704'491'978'525'8, 1e-9);
    diagnostic.str("");
    diagnostic << std::setprecision(17) << summit;
    RecordProperty("summit_m", diagnostic.str());
    diagnostic.str("");
    diagnostic << std::setprecision(17) << fine_peak;
    RecordProperty("fine_peak_m", diagnostic.str());
}

TEST(V10FoundationTests, UnchangedPerlinKernelMatchesEveryFrozenBinary32Vector)
{
    std::filesystem::path const directory{MC_V10_FIXTURE_DIRECTORY};
    ASSERT_FALSE(directory.empty());
    std::ifstream input(directory / "kernel-reference.tsv");
    ASSERT_TRUE(input.is_open());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
    size_t count = 0U, failures = 0U;
    while (std::getline(input, line))
    {
        std::istringstream row(line);
        double x = 0.0, y = 0.0, expected = 0.0;
        int32_t repeat_x = 0, repeat_y = 0, base = 0;
        ASSERT_TRUE(static_cast<bool>(row >> x >> y >> repeat_x >> repeat_y >> base >> expected));
        double const actual = platec_noise2(
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(repeat_x), static_cast<float>(repeat_y), base
        );
        if (actual != expected)
        {
            ++failures;
        }
        ++count;
    }
    EXPECT_EQ(count, 4096U);
    EXPECT_EQ(failures, 0U);
    RecordProperty("kernel_vector_count", static_cast<int32_t>(count));
    RecordProperty("kernel_exact_failures", static_cast<int32_t>(failures));
}

TEST(V10FoundationTests, CanonicalHeightIsNotFinalL0HeightInterpolation)
{
    auto const directory = std::filesystem::path{MC_V10_FIXTURE_DIRECTORY};
    ASSERT_FALSE(directory.empty());
    auto const raw = readLittleEndian<float, uint32_t>(directory / "raw-height.f32le");
    auto const basis = prepareV10Foundation(42U, raw);
    double const tx = 21.0 / 256.0, ty = 34.0 / 256.0;
    double interpolated = sampleV10FoundationHeight(basis, 46'336.0, 9984.0) * (1.0 - ty) * (1.0 - tx);
    interpolated += sampleV10FoundationHeight(basis, 46'592.0, 9984.0) * (1.0 - ty) * tx;
    interpolated += sampleV10FoundationHeight(basis, 46'336.0, 10'240.0) * ty * (1.0 - tx);
    interpolated += sampleV10FoundationHeight(basis, 46'592.0, 10'240.0) * ty * tx;
    double const canonical = sampleV10FoundationHeight(basis, 46'357.0, 10'018.0);
    EXPECT_GT(std::abs(canonical - interpolated), 1e-6);
    std::ostringstream diagnostic;
    diagnostic << std::setprecision(17) << interpolated;
    RecordProperty("final_l0_interpolated_height_m", diagnostic.str());
}

TEST(V10FoundationTests, PointHeightRejectsNonfiniteCoordinates)
{
    V10FoundationBasis const basis;
    EXPECT_THROW(static_cast<void>(sampleV10FoundationHeight(
        basis, std::numeric_limits<double>::infinity(), 0.0
    )), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(sampleV10FoundationHeight(
        basis, 0.0, std::numeric_limits<double>::quiet_NaN()
    )), std::invalid_argument);
}

TEST(V10FoundationTests, PointHeightRejectsMalformedBasis)
{
    V10FoundationBasis const basis;
    EXPECT_THROW(static_cast<void>(sampleV10FoundationHeight(basis, 0.0, 0.0)), std::invalid_argument);
}

TEST(V10FoundationTests, PointHeightRejectsNonfiniteSampledBasis)
{
    V10FoundationBasis basis;
    basis.land_magnitude_m.assign(COUNT, std::numeric_limits<double>::quiet_NaN());
    basis.ocean_magnitude_m.assign(COUNT, 1.0);
    basis.coast_distance_m.assign(COUNT, 1.0);
    basis.regional_relief_m.assign(COUNT, 1.0);
    EXPECT_THROW(static_cast<void>(sampleV10FoundationHeight(basis, 0.0, 0.0)), std::invalid_argument);
}

TEST(V10FoundationTests, PointHeightUsesNegativeAndPeriodShiftedCoordinates)
{
    auto const basis = prepareV10Foundation(42U, halfLandSource());
    double const direct = sampleV10FoundationHeight(basis, 123.5, 567.25);
    EXPECT_NEAR(sampleV10FoundationHeight(basis, 123.5 - 65'536.0, 567.25 - 65'536.0), direct, 1e-9);
    EXPECT_NEAR(sampleV10FoundationHeight(basis, 123.5 + 3.0 * 65'536.0, 567.25 + 2.0 * 65'536.0), direct, 1e-9);
    EXPECT_EQ(sampleV10FoundationHeight(basis, 0.0, 65'536.0), sampleV10FoundationHeight(basis, 65'536.0, 0.0));
}
