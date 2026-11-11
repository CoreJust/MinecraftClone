#include "V9Water.hpp"
#include "V9WaterClimate.hpp"
#include "V9WaterFields.hpp"
#include "V9WaterModel.hpp"
#include "V9WaterQuery.hpp"
#include "V9WaterRouting.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{

using namespace generation::detail;

TEST(V9Water, RejectsMissingMarineOutlet)
{
    std::vector<double> const bed(16, 10);
    V9WaterRates const rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_FALSE(water);
    EXPECT_EQ(water.error().kind, V9WaterFailure::Kind::MissingMarineOutlet);
}

TEST(V9Water, RejectsNonfiniteBedAndNegativeForcing)
{
    std::vector<double> bed(16, -1);
    V9WaterRates rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    bed[5] = std::numeric_limits<double>::quiet_NaN();
    auto water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_FALSE(water);
    EXPECT_EQ(water.error().kind, V9WaterFailure::Kind::InvalidInput);
    bed[5] = 0;
    rates.runoff_m_s[7] = -1;
    water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_FALSE(water);
    EXPECT_EQ(water.error().kind, V9WaterFailure::Kind::InvalidInput);
}

TEST(V9Water, RejectsOverflowingCellArea)
{
    std::vector<double> const bed(16, -1);
    V9WaterRates const rates{std::vector<double>(16, 1e-8), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates, 1e308);
    ASSERT_FALSE(water);
    EXPECT_EQ(water.error().kind, V9WaterFailure::Kind::InvalidInput);
    EXPECT_EQ(water.error().message, "finite positive water cell area required");
}

TEST(V9Water, RejectsUnderflowingCellArea)
{
    std::vector<double> const bed(16, -1);
    V9WaterRates const rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates, std::numeric_limits<double>::denorm_min());
    ASSERT_FALSE(water);
    EXPECT_EQ(water.error().kind, V9WaterFailure::Kind::InvalidInput);
    EXPECT_EQ(water.error().message, "finite positive water cell area required");
}

TEST(V9Water, ConnectsMarinePocketsAcrossThePeriodicTinDiagonal)
{
    std::vector<double> bed(16, 10);
    bed[0] = -1;
    bed[1] = -2;
    bed[5] = -3;
    bed[10] = -4;
    V9WaterRates const rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_TRUE(water) << water.error().message;
    EXPECT_TRUE(water->ocean[0]);
    EXPECT_TRUE(water->ocean[1]);
    EXPECT_TRUE(water->ocean[5]);
    EXPECT_TRUE(water->ocean[10]);
    EXPECT_EQ(water->vertex_receiver[15], 10);
    EXPECT_TRUE(water->bodies.empty());
    EXPECT_EQ(water->hydraulic_bed_m, bed);
}

TEST(V9Water, EquilibriumAndReceiverBudgetsCloseWithoutChangingBed)
{
    std::vector<double> const bed = {
        -1, -1, -1, -1, 10, 10, 10, 10, 10, 2, 4, 10, 10, 10, 10, 10,
    };
    V9WaterRates const rates{std::vector<double>(16, 2e-8), std::vector<double>(16, 1e-8),
                             std::vector<double>(16, 1e-8)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_TRUE(water) << water.error().message;
    ASSERT_EQ(water->bodies.size(), 1U);
    EXPECT_EQ(water->hydraulic_bed_m, bed);
    EXPECT_NEAR(water->water_residual_m3_s, 0, 1e-8);
    EXPECT_GT(water->bodies[0].stage_m, 2);
    EXPECT_LE(water->bodies[0].stage_m, 10);
    EXPECT_GT(water->bodies[0].volume_m3, 0);
    EXPECT_TRUE(water->lake[9]);
    EXPECT_EQ(water->vertex_receiver[9], 9);
    auto const route = routeV9Water(*water);
    ASSERT_TRUE(route) << route.error().message;
    EXPECT_NEAR(route->ocean_export_m3_s, water->ocean_export_m3_s, 1e-8);
    EXPECT_LT(route->body_max_residual_m3_s, 1e-7);
    auto const mesh = queryV9LakeSurface(*water, bed, 4, 65'536);
    ASSERT_TRUE(mesh);
    ASSERT_FALSE(mesh->empty());
    for (auto const& triangle : *mesh) {
        EXPECT_EQ(triangle.owner, water->bodies[0].id);
        for (auto const& p : triangle.xyz_m) {
            EXPECT_DOUBLE_EQ(p[2], water->bodies[0].stage_m);
        }
    }
    auto changed_bed = bed;
    changed_bed[9] += .01;
    auto const partial = queryV9LakeSurface(*water, changed_bed, 4, 65'536);
    ASSERT_FALSE(partial);
    EXPECT_EQ(partial.error().kind, V9WaterFailure::Kind::RegionalReceiverClosureRequired);
}

TEST(V9Water, RegistersCellCentresAtPeriodicVertices)
{
    std::vector<double> const cells = {0, 2, 4, 6};
    auto const vertices = registerV9WaterContinuous(cells, 2, 4, 1);
    ASSERT_TRUE(vertices);
    ASSERT_EQ(vertices->size(), 16U);
    EXPECT_DOUBLE_EQ((*vertices)[0], 3);
    EXPECT_DOUBLE_EQ((*vertices)[5], 0);
    EXPECT_DOUBLE_EQ((*vertices)[7], 2);
    EXPECT_DOUBLE_EQ((*vertices)[13], 4);
    EXPECT_DOUBLE_EQ((*vertices)[15], 6);
}

TEST(V9Water, UsesPersistentStockAndSeasonalRainForBiomes)
{
    V9WaterLandClimate climate;
    climate.phase_count = 4;
    for (uint32_t phase = 0; phase < 4; ++phase) {
        climate.temperature_C.insert(climate.temperature_C.end(), {-10, 5, 18, 18, 20, 20});
        climate.precipitation_mm_year.insert(climate.precipitation_mm_year.end(), {1'000, 100, 720, 719, 0, 0});
    }
    std::vector<uint8_t> const ocean = {0, 0, 0, 0, 1, 0}, lake = {0, 0, 0, 0, 0, 1};
    auto const inventory = deriveV9WaterLandInventory(climate, ocean, lake);
    ASSERT_TRUE(inventory);
    EXPECT_EQ(inventory->biome_id, (std::vector<uint8_t>{0, 3, 8, 7, 10, 11}));
    EXPECT_GT(inventory->glacier_thickness_m[0], 0);
    EXPECT_FLOAT_EQ(inventory->snow_water_equivalent_m[4], 0);
    EXPECT_NEAR(inventory->remaining_grid_sum_m, inventory->accumulation_grid_sum_m - inventory->melt_grid_sum_m,
                1e-10);
}

TEST(V9Water, IntegratesMarinePhaseAndHonorsCancellation)
{
    V9WaterForcing forcing;
    forcing.resolution = 2;
    forcing.phase_count = 4;
    forcing.ground_m.assign(4, 0);
    forcing.reference_ps.assign(4, 100'000);
    forcing.wet_fraction.assign(4, 1);
    for (auto* field : {&forcing.local_effective_C, &forcing.theta_C}) {
        field->assign(16, -2);
    }
    for (auto* field :
         {&forcing.rain_kg_m2_s, &forcing.runoff_kg_m2_s, &forcing.u_m_s, &forcing.v_m_s, &forcing.layer0_vapor_RH,
          &forcing.ocean_east_volume_m3_s, &forcing.ocean_north_volume_m3_s, &forcing.ocean_evap_kg_m2_s}) {
        field->assign(16, 0);
    }
    std::vector<double> const bed(256 * 256, -10);
    std::vector<V9WaterProgress> reports;
    auto const water = generateV9Water(bed, forcing, {}, [&](auto item) { reports.push_back(item); });
    ASSERT_TRUE(water) << water.error().message;
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.back().phase, V9WaterPhase::Complete);
    EXPECT_FLOAT_EQ(water->salinity_g_kg[0], 35);
    EXPECT_GT(water->sea_ice_thickness_m[0], 0);
    EXPECT_GT(water->sea_ice_brine_salinity_g_kg[0], 35);
    EXPECT_EQ(water->land_inventory.biome_id[0], 10);
    EXPECT_FLOAT_EQ(water->land_inventory.snow_water_equivalent_m[0], 0);
    EXPECT_LT(water->phase_enthalpy_residual_J_m2, .01);
    auto const fields = v9WaterNativeFields(*water);
    ASSERT_EQ(fields.size(), 45U);
    auto const downstream =
        std::find_if(fields.begin(), fields.end(), [](auto const& field) { return field.name == "downstream"; });
    ASSERT_NE(downstream, fields.end());
    EXPECT_EQ(downstream->dtype, V9WaterFieldType::Int64);
    EXPECT_EQ(downstream->bytes.size(), 256U * 256U * sizeof(int64_t));
    auto const biome =
        std::find_if(fields.begin(), fields.end(), [](auto const& field) { return field.name == "biome_id"; });
    ASSERT_NE(biome, fields.end());
    EXPECT_EQ(biome->dtype, V9WaterFieldType::UInt8);
    for (auto const& field : fields) {
        uint64_t elements = 1;
        for (uint64_t const extent : field.shape) {
            elements *= extent;
        }
        uint64_t const width =
            field.dtype == V9WaterFieldType::Float64 || field.dtype == V9WaterFieldType::Int64
                ? 8
                : (field.dtype == V9WaterFieldType::Float32 || field.dtype == V9WaterFieldType::Int32 ? 4 : 1);
        EXPECT_EQ(field.bytes.size(), elements * width) << field.name;
    }
    std::stop_source stop;
    stop.request_stop();
    auto const cancelled = generateV9Water(bed, forcing, stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().kind, V9WaterFailure::Kind::Cancelled);
}

TEST(V9Water, LocalRiverClassificationExcludesRegisteredSnowStock)
{
    V9WaterLandClimate const climate{4, std::vector<double>(8, -10), std::vector<double>(8, 1'000)};
    std::vector<double> const snow(8, 1), glacier(8, 2);
    std::vector<uint8_t> const ocean = {0, 0}, lake = {0, 0}, river = {0, 1};
    auto const result = classifyV9WaterLocalBiomes(climate, snow, glacier, ocean, lake, river);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->biome_id, (std::vector<uint8_t>{0, 12}));
    EXPECT_FLOAT_EQ(result->snow_water_equivalent_m[0], 1);
    EXPECT_FLOAT_EQ(result->snow_water_equivalent_m[1], 0);
    EXPECT_FLOAT_EQ(result->glacier_thickness_m[1], 0);
}

TEST(V9Water, V21LocalRiverSupportIncludesClosedChordEndpoints)
{
    V9WaterEquilibrium equilibrium;
    equilibrium.resolution = 4U;
    equilibrium.hydraulic_bed_m.assign(16U, 0.0);
    equilibrium.ocean.assign(16U, 0U);
    equilibrium.triangle_basin_owner.assign(32U, -1);
    V9WaterFixedSupport fixed;
    fixed.size = 8U;

    V9RefinementReach reach;
    reach.start = 17;
    reach.a = {100.0, 100.0};
    reach.delta = {0.0, 10.0};
    reach.water_stage0 = 9.0;
    reach.water_stage1 = 8.0;
    reach.halfwidth = 6.0;
    reach.discharge_m3_s = 1.0;
    V9WaterQueryModel const model{{}, std::span<V9RefinementReach const>{&reach, 1U}, {}, {}};
    std::vector<double> const bed(4U, 0.0);
    V9WaterQueryGrid const grid{2U, 2U, 99.0, 115.0, 1.0, bed};

    auto const v21 = queryV21WaterLocalSupport(equilibrium, fixed, grid, model);
    ASSERT_TRUE(v21);
    EXPECT_EQ(v21->river, (std::vector<uint8_t>{1U, 1U, 0U, 1U}));
    EXPECT_EQ(v21->river_owner_id, (std::vector<int32_t>{17, 17, -1, 17}));
    EXPECT_EQ(v21->water_level_m, (std::vector<float>{8.0F, 8.0F, 0.0F, 8.0F}));

    auto const v9 = queryV9WaterLocalSupport(equilibrium, fixed, grid, model);
    ASSERT_TRUE(v9);
    EXPECT_EQ(v9->river, (std::vector<uint8_t>{0U, 0U, 0U, 0U}));
}

TEST(V9Water, QueryUsesBilinearSupportWithoutNativeRiverAndCancelsExplicitly)
{
    std::vector<double> const bed = {-1, -2, -3, -4, -2, -3, -4, -5, -3, -4, -5, -6, -4, -5, -6, -7};
    V9WaterRates const rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_TRUE(water);
    auto const routing = routeV9Water(*water);
    ASSERT_TRUE(routing);
    auto const refinement = V9WaterRefinement::create(*water, *routing);
    ASSERT_TRUE(refinement);
    auto model = makeV9WaterQueryModel(*refinement);
    EXPECT_FALSE(model.base);
    auto const fixed = buildV9WaterFixedSupport(*water, model);
    ASSERT_TRUE(fixed);
    std::vector<double> const fine_bed(4, -5);
    V9WaterQueryGrid const grid{2, 2, 65'280, 65'280, 256, fine_bed};
    auto const mesh = queryV9WaterMesh(*water, *fixed, grid, model);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->triangles.size(), 2U);
    EXPECT_EQ(mesh->kind, (std::vector<uint8_t>{1, 1}));
    for (auto const& triangle : mesh->triangles) {
        for (auto const& xyz : triangle.xyz_m) {
            EXPECT_DOUBLE_EQ(xyz[2], 0);
        }
    }
    model.progress = [](auto, auto, auto) { return false; };
    auto const cancelled = buildV9WaterFixedSupport(*water, model);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().kind, V9WaterFailure::Kind::Cancelled);
}

TEST(V9Water, LargePeriodicOriginsKeepLocalRiverAndMeshQueriesBounded)
{
    std::vector<double> bed(16, 10);
    bed[10] = -1;
    V9WaterRates const rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_TRUE(water);
    auto const fixed = buildV9WaterFixedSupport(*water);
    ASSERT_TRUE(fixed);
    V9RefinementReach reach;
    reach.start = 7;
    reach.a = {0, 0};
    reach.delta = {1'024, 0};
    reach.halfwidth = 256;
    reach.water_stage0 = reach.water_stage1 = 5;
    reach.discharge_m3_s = 1;
    V9WaterQueryModel const model{{}, std::span(&reach, 1), {}, {}};
    std::vector<double> const fine_bed(4, 0);
    V9WaterQueryGrid const baseline{2, 2, 0, 0, 256, fine_bed};
    auto const local = queryV9WaterLocalSupport(*water, *fixed, baseline, model);
    ASSERT_TRUE(local);
    EXPECT_EQ(local->river, (std::vector<uint8_t>{1, 1, 1, 1}));
    auto const mesh = queryV9WaterMesh(*water, *fixed, baseline, model);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->kind, (std::vector<uint8_t>{3, 3}));
    for (double const shift : {std::ldexp(65'536.0, 40), std::ldexp(65'536.0, 700)}) {
        auto shifted = baseline;
        shifted.origin_x_m = shift;
        shifted.origin_y_m = -shift;
        auto const wrapped = queryV9WaterLocalSupport(*water, *fixed, shifted, model);
        ASSERT_TRUE(wrapped);
        EXPECT_EQ(wrapped->river, local->river);
        EXPECT_EQ(wrapped->river_owner_id, local->river_owner_id);
        EXPECT_EQ(wrapped->water_depth_m, local->water_depth_m);
        auto const shifted_mesh = queryV9WaterMesh(*water, *fixed, shifted, model);
        if (shift < std::numeric_limits<float>::max()) {
            ASSERT_TRUE(shifted_mesh);
            EXPECT_EQ(shifted_mesh->kind, mesh->kind);
            EXPECT_EQ(shifted_mesh->triangles.size(), mesh->triangles.size());
            for (auto const& triangle : shifted_mesh->triangles) {
                for (auto const& xyz : triangle.xyz_m) {
                    EXPECT_TRUE(std::isfinite(xyz[0]));
                    EXPECT_TRUE(std::isfinite(xyz[1]));
                }
            }
        } else {
            ASSERT_FALSE(shifted_mesh);
            EXPECT_EQ(shifted_mesh.error().kind, V9WaterFailure::Kind::InvalidInput);
        }
    }
}

TEST(V9Water, RejectsOverflowingQueryEndpointsAndUnrepresentableClippingRanges)
{
    std::vector<double> const bed(16, -1);
    V9WaterRates const rates{std::vector<double>(16), std::vector<double>(16), std::vector<double>(16)};
    auto const water = solveV9WaterEquilibrium(bed, 4, rates);
    ASSERT_TRUE(water);
    auto const fixed = buildV9WaterFixedSupport(*water);
    ASSERT_TRUE(fixed);
    std::vector<double> const fine_bed(4, 0);
    V9WaterQueryGrid const overflow{
        2, 2, std::numeric_limits<double>::max(), 0, std::numeric_limits<double>::max(), fine_bed};
    auto const local = queryV9WaterLocalSupport(*water, *fixed, overflow);
    ASSERT_FALSE(local);
    EXPECT_EQ(local.error().kind, V9WaterFailure::Kind::InvalidInput);
    auto const mesh = queryV9WaterMesh(*water, *fixed, overflow);
    ASSERT_FALSE(mesh);
    EXPECT_EQ(mesh.error().kind, V9WaterFailure::Kind::InvalidInput);
    V9RefinementReach reach;
    reach.delta = {1'024, 0};
    reach.halfwidth = 256;
    V9WaterQueryModel const model{{}, std::span(&reach, 1), {}, {}};
    V9WaterQueryGrid const excessive_span{2, 2, 0, 0, 1e30, fine_bed};
    auto const unbounded = queryV9WaterLocalSupport(*water, *fixed, excessive_span, model);
    ASSERT_FALSE(unbounded);
    EXPECT_EQ(unbounded.error().kind, V9WaterFailure::Kind::InvalidInput);
    auto const unbounded_mesh = queryV9WaterMesh(*water, *fixed, excessive_span, model);
    ASSERT_FALSE(unbounded_mesh);
    EXPECT_EQ(unbounded_mesh.error().kind, V9WaterFailure::Kind::InvalidInput);
    V9WaterQueryGrid const tiny_spacing{2, 2, 0, 0, 1e-300, fine_bed};
    auto const tiny_local = queryV9WaterLocalSupport(*water, *fixed, tiny_spacing, model);
    ASSERT_TRUE(tiny_local);
    auto const unrepresentable_clip = queryV9WaterMesh(*water, *fixed, tiny_spacing, model);
    ASSERT_FALSE(unrepresentable_clip);
    EXPECT_EQ(unrepresentable_clip.error().kind, V9WaterFailure::Kind::InvalidInput);
}

} // namespace
