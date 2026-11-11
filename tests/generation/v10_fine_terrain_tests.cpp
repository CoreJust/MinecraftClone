#include "V10FineTerrain.hpp"
#include "V10Foundation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
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

V10FineTerrainParent parentFor(V10FineTerrainBedSampler sampler, std::string identity = "test-parent")
{
    return {
        .seed = V10_FINE_TERRAIN_SEED,
        .period_m = V10_FINE_TERRAIN_PERIOD_M,
        .identity_sha256 = std::move(identity),
        .bed = std::move(sampler),
    };
}

TEST(V10FineTerrainTests, RejectsParentsOutsideTheCurrentV10Contract)
{
    V10FineTerrainParent parent = parentFor([](double, double) { return 10.0; });
    parent.seed = 43U;
    EXPECT_THROW(static_cast<void>(V10FineTerrain{parent}), std::invalid_argument);

    parent.seed = V10_FINE_TERRAIN_SEED;
    parent.period_m = 1024.0;
    EXPECT_THROW(static_cast<void>(V10FineTerrain{parent}), std::invalid_argument);

    parent.period_m = V10_FINE_TERRAIN_PERIOD_M;
    parent.identity_sha256.clear();
    EXPECT_THROW(static_cast<void>(V10FineTerrain{parent}), std::invalid_argument);

    parent.identity_sha256 = "test-parent";
    parent.bed = {};
    EXPECT_THROW(static_cast<void>(V10FineTerrain{parent}), std::invalid_argument);

    EXPECT_THROW(static_cast<void>(V10FineTerrain{parentFor([](double, double) {
        return std::numeric_limits<double>::quiet_NaN();
    })}), std::invalid_argument);
}

TEST(V10FineTerrainTests, PreparedPlanBindsParentAndKeepsLevelIdentitiesSeparate)
{
    auto query_count = std::make_shared<size_t>(0U);
    V10FineTerrain field(parentFor([query_count](double x, double y) {
        ++*query_count;
        double const px = 2.0 * std::numbers::pi * x / V10_FINE_TERRAIN_PERIOD_M;
        double const py = 2.0 * std::numbers::pi * y / V10_FINE_TERRAIN_PERIOD_M;
        return 220.0 * std::sin(px) + 118.0 * std::cos(py) + 32.0 * std::sin(px + 2.0 * py);
    }));
    auto const& receipt = field.receipt();
    EXPECT_NE(receipt.source_id.find("6365ed7e79067c12fbc4d717e17ce2e1a239fbb672abf170ecf9cf4b1c255717"),
        std::string::npos);
    EXPECT_EQ(receipt.parent_identity_sha256, "test-parent");
    EXPECT_EQ(receipt.prepared_parent_queries, 591'296U);
    EXPECT_EQ(*query_count, receipt.prepared_parent_queries);
    EXPECT_EQ(receipt.mountain_site_count, 64U);
    EXPECT_GT(receipt.valley_network_count, 0U);
    EXPECT_EQ(receipt.regional_site_count, 65'536U);
    EXPECT_EQ(receipt.regional_draws_sha256,
        "262b0a3b6afd2f81b7322d89416e4cf01ffc87ed984b9e3c053dc8e86de75c2a");
    EXPECT_EQ(receipt.regional_feature_sha256.size(), 64U);
    EXPECT_EQ(receipt.plan_sha256.size(), 64U);
    EXPECT_NE(receipt.field_identity_sha256, receipt.parent_identity_sha256);
    EXPECT_NE(receipt.l2_identity_sha256, receipt.l3_identity_sha256);

    static_cast<void>(field.bed(123.0, 456.0, V10FineTerrainLevel::L2));
    EXPECT_EQ(*query_count, receipt.prepared_parent_queries + 9U);
    EXPECT_THROW(static_cast<void>(field.bed(0.0, 0.0, static_cast<V10FineTerrainLevel>(255U))),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(field.bed(
        std::numeric_limits<double>::infinity(), 0.0, V10FineTerrainLevel::L2)),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(field.bedWithParentCenter(
        0.0, 0.0, V10FineTerrainLevel::L2, std::numeric_limits<double>::quiet_NaN())),
        std::invalid_argument);
}

TEST(V10FineTerrainTests, ActualF775OracleSliceMatchesL2AndL3)
{
    std::filesystem::path const fixture_directory{MC_V10_FIXTURE_DIRECTORY};
    ASSERT_FALSE(fixture_directory.empty());
    std::vector<float> const raw = readRawFixture(fixture_directory / "raw-height.f32le");
    V10FoundationBasis const foundation = prepareV10Foundation(V10_FINE_TERRAIN_SEED, raw);
    auto query_count = std::make_shared<size_t>(0U);
    V10FineTerrain const field(parentFor([&foundation, query_count](double x, double y) {
        ++*query_count;
        return sampleV10FoundationHeight(foundation, x, y);
    }, "533523820f1696c52a234d9a988bdee56247656f00b0098d5dfe8b408171807e"));
    EXPECT_EQ(field.receipt().parent_identity_sha256,
        "533523820f1696c52a234d9a988bdee56247656f00b0098d5dfe8b408171807e");

    struct OracleSample final
    {
        double x;
        double y;
        double parent;
        double l2;
        double l3;
    };
    constexpr std::array<OracleSample, 7U> SAMPLES{{
        // Actual f775 source, known S2 problem point.
        {35'360.0, 33'230.0, 159.400'207'750'389'6, 167.124'357'622'848'75, 167.862'544'865'654'23},
        // Actual f775 maximum-impact ridge witness.
        {52'372.486'699'750'08, 4'937.492'867'001'128, 381.412'640'785'166'3,
            451.201'526'762'104'44, 453.145'070'453'644'5},
        // Signed-height coast witness.
        {45'056.0, 0.0, 23.946'090'327'436'206, 11.305'210'844'927'878, 12.325'530'351'962'147},
        // Low-relief plain control, including the near-zero regional activation.
        {25'856.0, 9'728.0, 25.250'060'325'517'556, 25.250'060'325'517'556, 25.250'060'325'517'556},
        // Actual f775 subzero control.
        {0.0, 0.0, -428.152'148'207'374'4, -428.152'148'207'374'4, -428.152'148'207'374'4},
        // Periodic edge control.
        {65'535.75, 8'192.0, -479.315'980'572'568'9, -479.315'980'572'568'9, -479.315'980'572'568'9},
        // Equivalent negative-coordinate periodic edge control.
        {-0.25, 8'192.0, -479.315'980'572'568'9, -479.315'980'572'568'9, -479.315'980'572'568'9},
    }};
    EXPECT_EQ(*query_count, field.receipt().prepared_parent_queries);
    constexpr size_t PROVIDER_NATIVE_CENTER_QUERY_COUNT = 1U; // nativeBedSample runs once before fine terrain.
    for (OracleSample const& sample : SAMPLES)
    {
        double const parent = sampleV10FoundationHeight(foundation, sample.x, sample.y);
        auto const bed_with_center_parity = [&](V10FineTerrainLevel const level) {
            size_t const before = *query_count;
            double const standalone = field.bed(sample.x, sample.y, level);
            size_t const standalone_fine_queries = *query_count - before;
            EXPECT_EQ(standalone_fine_queries, 9U);
            EXPECT_EQ(PROVIDER_NATIVE_CENTER_QUERY_COUNT + standalone_fine_queries, 10U);
            size_t const before_reused = *query_count;
            double const reused = field.bedWithParentCenter(sample.x, sample.y, level, parent);
            size_t const reused_fine_queries = *query_count - before_reused;
            EXPECT_EQ(reused_fine_queries, 8U);
            EXPECT_EQ(PROVIDER_NATIVE_CENTER_QUERY_COUNT + reused_fine_queries, 9U);
            EXPECT_EQ(std::bit_cast<uint64_t>(reused), std::bit_cast<uint64_t>(standalone));
            return standalone;
        };
        double const l2 = bed_with_center_parity(V10FineTerrainLevel::L2);
        double const l3 = bed_with_center_parity(V10FineTerrainLevel::L3);
        EXPECT_NEAR(parent, sample.parent, 1e-6);
        EXPECT_NEAR(l2, sample.l2, 1e-6);
        EXPECT_NEAR(l3, sample.l3, 1e-6);
        if (parent <= 0.0)
        {
            EXPECT_DOUBLE_EQ(l2, parent);
            EXPECT_DOUBLE_EQ(l3, parent);
        }
        else
        {
            EXPECT_GE(l2, 0.42 * parent - 1e-9);
            EXPECT_GE(l3, 0.42 * parent - 1e-9);
        }
    }

    EXPECT_DOUBLE_EQ(field.bed(-0.25, 8192.0, V10FineTerrainLevel::L2),
        field.bed(65'535.75, 8192.0, V10FineTerrainLevel::L2));
    EXPECT_DOUBLE_EQ(field.bed(-0.25, 8192.0, V10FineTerrainLevel::L3),
        field.bed(65'535.75, 8192.0, V10FineTerrainLevel::L3));
    EXPECT_NE(field.receipt().l2_identity_sha256, field.receipt().l3_identity_sha256);
}

} // namespace
