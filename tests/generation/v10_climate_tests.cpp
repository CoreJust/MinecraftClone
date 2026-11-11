#include "V10Climate.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
using namespace generation::detail;

V10ClimateFields uniformFields(double temperature = -2.0, double rain = 800.0)
{
    return {std::vector<double>(8U * 32U * 32U, temperature), std::vector<double>(32U * 32U, 0.0),
        std::vector<double>(8U * 32U * 32U, 6.0), std::vector<double>(8U * 256U * 256U, rain)};
}

struct ReferencePoint
{
    double x, y, bed;
    V10WaterPresence water;
    std::array<double, 4U> ground;
    std::array<std::array<double, 4U>, 8U> air, marine, rain;
    std::array<double, 8U> temperature, snow, glacier;
    uint8_t biome;
};

// Stencils and float32 output from immutable native-v10-adoption-20261027
// reference-state-v10.npz SHA256:
// f5a3a742e85e36c30a340afe4acafca0d9faee6ad2820503317bd8178e97443a.
// fine_surface_climate_v1.py query, with actual pinned V10 bed.
ReferencePoint const REFERENCE_POINTS[]{
    {
        46'336.0, 9984.0, 723.342'269'667'154'6, {false, false, false},
        {6154.989'201'771'926'7, 4527.491'612'223'702'8, 1824.904'423'219'326'6, 1424.988'778'029'602'8},
        {{{-33.473'577'712'129'327, -23.309'397'153'170'142, -3.227'968'848'534'628'1, -0.905'571'872'175'656'27},
        {-30.545'240'342'313'068, -20.410'936'699'206'371, -0.048'980'477'654'019'694, 2.221'020'868'570'642'3},
        {-34.189'855'072'876'91, -23.911'291'431'243'949, -2.701'764'662'813'612'7, -0.369'581'298'427'021'92},
        {-41.658'029'940'254'131, -31.175'685'434'676'751, -9.019'194'591'165'138'8, -6.582'219'078'761'964'3},
        {-48.406'155'402'689'777, -37.796'934'642'787'15, -15.188'971'721'748'551, -12.652'240'461'361'373},
        {-50.760'941'205'910'221, -40.151'398'368'742'761, -17.861'887'574'182'02, -15.297'990'120'298'937},
        {-47.521'696'016'650'871, -37.047'136'745'707'768, -15.606'690'443'718'726, -13.131'688'712'792'148},
        {-40.562'048'247'561'947, -30.261'981'467'133'548, -9.711'760'970'261'780'2, -7.350'844'046'102'807'3}}},
        {{{6.533'852'099'388'194'1, 6.119'298'326'283'925'8, 8.633'909'902'390'994'9, 8.356'855'185'016'762'3},
        {9.462'189'469'204'449'9, 9.017'758'780'247'696'4, 11.812'898'273'271'603, 11.483'447'925'763'061},
        {5.817'574'738'640'612'3, 5.517'404'048'210'117'7, 9.160'114'088'112'010'2, 8.892'845'758'765'396'6},
        {-1.650'600'128'736'613'2, -1.746'989'955'222'684'6, 2.842'684'159'760'485'1, 2.680'207'978'430'454'2},
        {-8.398'725'591'172'258'5, -8.368'239'163'333'083, -3.327'092'970'822'928'2, -3.389'813'404'168'953'6},
        {-10.753'511'394'392'705, -10.722'702'889'288'692, -6.000'008'823'256'396'6, -6.035'563'063'106'518},
        {-7.514'266'205'133'352'2, -7.618'441'266'253'699'8, -3.744'811'692'793'102'8, -3.869'261'655'599'729},
        {-0.554'618'436'044'428'51, -0.833'285'987'679'481'37, 2.150'117'780'663'842'3, 1.911'583'011'089'611'7}}},
        {{{2058.455'810'546'875, 2024.749'633'789'062'5, 1715.375'610'351'562'5, 1680.011'718'75},
        {2336.295'898'437'5, 2302.281'494'140'625, 1887.594'116'210'937'5, 1857.901'123'046'875},
        {2184.785'644'531'25, 2168.153'564'453'125, 1829.835'693'359'375, 1806.467'041'015'625},
        {1720.684'204'101'562'5, 1707.756'103'515'625, 1567.452'758'789'062'5, 1543.179'931'640'625},
        {1261.743'530'273'437'5, 1259.070'434'570'312'5, 1207.458'251'953'125, 1195.293'823'242'187'5},
        {933.584'716'796'875, 924.997'253'417'968'75, 877.611'206'054'687'5, 859.942'077'636'718'75},
        {973.383'422'851'562'5, 969.950'195'312'5, 878.951'293'945'312'5, 868.515'319'824'218'75},
        {1397.929'809'570'312'5, 1377.371'704'101'562'5, 1212.552'246'093'75, 1185.686'401'367'187'5}}},
        {-39.741'249'084'472'656, -36.723'705'291'748'047, -39.982'200'622'558'594, -46.998'012'542'724'609,
            -53.514'667'510'986'328,
            -55.987'453'460'693'359, -53.131'938'934'326'172, -46.590'541'839'599'609},
        {0.257'306'963'205'337'52, 0.549'343'943'595'886'23, 0.822'442'173'957'824'71, 1.037'527'680'397'033'7,
            1.195'245'623'588'562,
            1.311'943'769'454'956'1, 1.433'616'638'183'593'8, 1.608'357'906'341'552'7},
        {33.324'752'807'617'188, 33.324'752'807'617'188, 33.324'752'807'617'188, 33.324'752'807'617'188, 33.324'752'807'617'188,
            33.324'752'807'617'188, 33.324'752'807'617'188, 33.324'752'807'617'188},
        0U,
    },
    {
        44'544.0, 0.0, -158.519'074'424'082'13, {true, false, false},
        {0, 508.216'007'309'888'48, 182.029'487'182'310'16, 1404.528'538'213'406'1},
        {{{0.376'557'470'476'686'81, -2.529'019'551'032'914'8, -0.615'711'906'011'251'7, -8.024'222'556'797'827'4},
        {4.296'478'334'425'279'9, 1.372'416'490'150'271'3, 3.400'145'001'212'539'2, -4.708'285'801'786'446'2},
        {3.036'382'036'023'836'5, -0.798'955'868'325'125'88, -0.841'474'953'777'131'55, -9.679'862'535'440'486'6},
        {-2.113'249'910'044'470'2, -6.646'306'524'532'320'7, -9.796'503'616'113'573'9, -18.963'693'554'206'852},
        {-7.763'521'134'025'405'8, -12.467'018'105'957'72, -17.922'915'446'154'768, -26.945'432'144'749'294},
        {-10.931'621'542'281'318, -15.291'692'399'650'614, -20.896'169'697'879'344, -29.295'814'069'523'349},
        {-10.177'866'785'216'363, -13.865'616'029'841'259, -17.273'094'476'260'002, -24.900'869'481'805'579},
        {-6.028'030'450'625'141'9, -9.085'133'274'765'906'8, -9.280'799'495'045'883'4, -16.412'040'890'479'336}}},
        {{{0.376'557'470'476'686'81, 0.774'384'496'481'360'43, 0.567'479'760'673'764'27, 1.105'212'941'589'311},
        {4.296'478'334'425'279'9, 4.675'820'537'664'546'5, 4.583'336'667'897'555'1, 4.421'149'696'600'692'5},
        {3.036'382'036'023'836'5, 2.504'448'179'189'149'3, 0.341'716'712'907'884'47, -0.550'427'037'053'347'17},
        {-2.113'249'910'044'470'2, -3.342'902'477'018'046, -8.613'311'949'428'558'4, -9.834'258'055'819'713'5},
        {-7.763'521'134'025'405'8, -9.163'614'058'443'444'7, -16.739'723'779'469'752, -17.815'996'646'362'155},
        {-10.931'621'542'281'318, -11.988'288'352'136'339, -19.712'978'031'194'329, -20.166'378'571'136'21},
        {-10.177'866'785'216'363, -10.562'211'982'326'984, -16.089'902'809'574'987, -15.771'433'983'418'44},
        {-6.028'030'450'625'141'9, -5.781'729'227'251'631'2, -8.097'607'828'360'867'9, -7.282'605'392'092'198}}},
        {{{149.393'737'792'968'75, 212.435'440'063'476'56, 114.912'918'090'820'31, 163.627'822'875'976'56},
        {172.523'712'158'203'12, 248.252'090'454'101'56, 132.169'235'229'492'19, 190.353'652'954'101'56},
        {239.255'889'892'578'12, 319.880'249'023'437'5, 182.393'875'122'070'31, 244.338'043'212'890'62},
        {178.660'491'943'359'38, 232.542'846'679'687'5, 136.622'390'747'070'31, 178.250'457'763'671'88},
        {179.827'224'731'445'31, 224.603'759'765'625, 136.533'325'195'312'5, 170.913'101'196'289'06},
        {100.047'676'086'425'78, 132.519'149'780'273'44, 75.339'263'916'015'625, 99.844'612'121'582'031},
        {107.382'293'701'171'88, 143.155'380'249'023'44, 81.012'145'996'093'75, 108.079'666'137'695'31},
        {108.651'100'158'691'41, 152.984'695'434'570'31, 83.052'490'234'375, 117.084'693'908'691'41}}},
        {0.588'963'627'815'246'58, 4.467'051'982'879'638'7, 1.511'039'614'677'429'2, -5.669'605'731'964'111'3,
            -12.561'167'716'979'98,
            -15.511'057'853'698'73, -13.142'119'407'653'809, -6.930'156'230'926'513'7},
        {0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0},
        10U,
    },
    {
        64'768.0, 16'384.0, 176.718'261'190'900'36, {false, false, false},
        {1493.291'686'256'551, 768.387'492'751'269'75, 1181.936'524'014'666'2, 1138.993'811'633'526'8},
        {{{3.356'057'832'899'878'7, 8.582'975'174'237'976'3, 8.746'647'858'264'442, 9.838'193'159'646'254},
        {5.938'348'003'521'769'1, 11.109'676'450'267'367, 11.097'909'341'813'288, 12.079'379'254'093'288},
        {4.976'270'602'608'726'9, 9.893'250'520'588'017'2, 10.184'689'028'616'351, 10.758'866'851'187'77},
        {1.065'460'228'457'515'9, 5.694'456'174'473'888'4, 6.576'285'134'022'245'2, 6.686'742'532'479'115'7},
        {-3.531'244'923'343'594'5, 0.936'519'863'807'006'03, 2.371'334'258'287'038, 2.220'169'018'527'915'5},
        {-6.299'913'162'023'170'1, -1.852'384'726'615'049'7, -0.176'582'726'709'096'25, -0.274'756'588'993'331'44},
        {-5.542'501'439'234'375'7, -0.812'753'238'673'193'93, 0.626'599'657'981'333'1, 0.950'699'007'473'732'04},
        {-1.478'060'416'390'796'7, 3.663'298'068'837'454'1, 4.406'013'579'733'907'7, 5.248'062'242'344'724'7}}},
        {{{13.062'453'793'567'46, 13.577'493'877'121'229, 16.429'235'264'359'772, 17.241'652'935'264'177},
        {15.644'743'964'189'351, 16.104'195'153'150'62, 18.780'496'747'908'618, 19.482'839'029'711'212},
        {14.682'666'563'276'308, 14.887'769'223'471'27, 17.867'276'434'711'68, 18.162'326'626'805'694},
        {10.771'856'189'125'097, 10.688'974'877'357'142, 14.258'872'540'117'576, 14.090'202'308'097'039},
        {6.175'151'037'323'987'1, 5.931'038'566'690'259'2, 10.053'921'664'382'369, 9.623'628'794'145'838'9},
        {3.406'482'798'644'411'9, 3.142'133'976'268'203'5, 7.506'004'679'386'234'3, 7.128'703'186'624'592},
        {4.163'894'521'433'205'9, 4.181'765'464'210'059'2, 8.309'187'064'076'663'6, 8.354'158'783'091'655'5},
        {8.228'335'544'276'784'9, 8.657'816'771'720'707'3, 12.088'600'985'829'238, 12.651'522'017'962'648}}},
        {{{1976.183'593'75, 1723.319'702'148'437'5, 1594.688'720'703'125, 1400.097'900'390'625},
        {2776.097'412'109'375, 2391.415'527'343'75, 2228.743'652'343'75, 1924.421'508'789'062'5},
        {3223.735'351'562'5, 2779.488'037'109'375, 2725.073'242'187'5, 2354.952'148'437'5},
        {3218.069'580'078'125, 2791.536'132'812'5, 2848.777'832'031'25, 2480.290'771'484'375},
        {2971.658'447'265'625, 2602.933'349'609'375, 2727.626'464'843'75, 2401.099'121'093'75},
        {2597.243'652'343'75, 2248.475'097'656'25, 2367.303'466'796'875, 2053.357'910'156'25},
        {2131.011'230'468'75, 1838.637'329'101'562'5, 1888.841'796'875, 1632.882'080'078'125},
        {1795.432'983'398'437'5, 1561.775'878'906'25, 1469.587'402'343'75, 1285.695'800'781'25}}},
        {3.342'123'746'871'948'2, 5.798'545'360'565'185'5, 4.819'543'838'500'976'6, 1.012'955'427'169'799'8,
            -3.414'300'918'579'101'6,
            -6.070'546'150'207'519'5, -5.246'218'681'335'449'2, -1.266'193'628'311'157'2},
        {0, 0, 0, 0, 0.371'457'308'530'807'5, 0.696'112'751'960'754'39, 0.962'489'187'717'437'74, 1.186'918'258'666'992'2},
        {0, 0, 0, 0, 0, 0, 0, 0},
        1U,
    },
};

void checkReference(size_t index)
{
    auto const& reference = REFERENCE_POINTS[index];
    auto fields = uniformFields();
    auto fill = [](auto& target, size_t side, int32_t bx, int32_t by, size_t phase, auto const& values) {
        for (size_t corner = 0U; corner < 4U; ++corner)
        {
            auto const n = static_cast<int32_t>(side);
            size_t const x = static_cast<size_t>((bx + static_cast<int32_t>(corner % 2U) + n) % n);
            size_t const y = static_cast<size_t>((by + static_cast<int32_t>(corner / 2U) + n) % n);
            target[(phase * side + y) * side + x] = values[corner];
        }
    };
    auto const cx = static_cast<int32_t>(std::floor(reference.x / 2048.0 - .5));
    auto const cy = static_cast<int32_t>(std::floor(reference.y / 2048.0 - .5));
    auto const rx = static_cast<int32_t>(reference.x / 256.0);
    auto const ry = static_cast<int32_t>(reference.y / 256.0);
    fill(fields.ground_m, 32U, cx, cy, 0U, reference.ground);
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        fill(fields.local_effective_celsius, 32U, cx, cy, phase, reference.air[phase]);
        fill(fields.theta_celsius, 32U, cx, cy, phase, reference.marine[phase]);
        fill(fields.precipitation_mm_year, 256U, rx, ry, phase, reference.rain[phase]);
    }
    V10ClimateForcing const forcing(std::move(fields));
    auto const sample = queryV10Climate(forcing, reference.x, reference.y, reference.bed, reference.water);
    EXPECT_EQ(sample.biome_id, reference.biome);
    for (size_t phase = 0U; phase < 8U; ++phase)
    {
        SCOPED_TRACE(phase);
        EXPECT_EQ(static_cast<float>(sample.temperature_celsius[phase]), reference.temperature[phase]);
        EXPECT_EQ(static_cast<float>(sample.snow_water_equivalent_m[phase]), reference.snow[phase]);
        EXPECT_EQ(static_cast<float>(sample.glacier_thickness_m[phase]), reference.glacier[phase]);
    }
    auto const wrapped = queryV10Climate(forcing, reference.x - 65'536.0, reference.y + 131'072.0,
        reference.bed, reference.water);
    EXPECT_EQ(wrapped.temperature_celsius, sample.temperature_celsius);
    EXPECT_EQ(wrapped.snow_water_equivalent_m, sample.snow_water_equivalent_m);
    EXPECT_EQ(wrapped.glacier_thickness_m, sample.glacier_thickness_m);
    EXPECT_EQ(wrapped.biome_id, sample.biome_id);
}

TEST(V10Climate, MatchesPinnedSummitPhasesAndPermanentSnowBiome)
{
    checkReference(0U);
}
TEST(V10Climate, MatchesPinnedCoastalOceanPhases)
{
    checkReference(1U);
}
TEST(V10Climate, MatchesPinnedLandSeasonalHistory)
{
    checkReference(2U);
}

TEST(V10Climate, RecomputesSnowAndBiomeAtActualBed)
{
    V10ClimateForcing const forcing(uniformFields(20.0, 800.0));
    auto const low = queryV10Climate(forcing, 0.0, 0.0, 0.0);
    auto const high = queryV10Climate(forcing, 0.0, 0.0, 500.0);
    EXPECT_NE(low.biome_id, high.biome_id);
    EXPECT_EQ(high.biome_id, 0U);
    EXPECT_DOUBLE_EQ(high.mean_temperature_celsius, -12.5);
    EXPECT_GT(high.glacier_thickness_m[0U], 0.0);
    EXPECT_EQ(low.mean_snow_water_equivalent_m, 0.0);
}

TEST(V10Climate, DistinguishesActualWaterAndKeepsRiverLandSnowRule)
{
    V10ClimateForcing const forcing(uniformFields());
    auto const ocean = queryV10Climate(forcing, 0, 0, 900, {true, true, true});
    auto const lake = queryV10Climate(forcing, 0, 0, 900, {false, true, true});
    auto const river = queryV10Climate(forcing, 0, 0, 900, {false, false, true});
    EXPECT_EQ(ocean.biome_id, 10U);
    EXPECT_EQ(lake.biome_id, 11U);
    EXPECT_EQ(river.biome_id, 12U);
    EXPECT_FALSE(ocean.water.lake);
    EXPECT_FALSE(ocean.water.river);
    EXPECT_FALSE(lake.water.river);
    EXPECT_DOUBLE_EQ(ocean.mean_temperature_celsius, 6.0);
    EXPECT_DOUBLE_EQ(ocean.mean_snow_water_equivalent_m, 0.0);
    EXPECT_DOUBLE_EQ(lake.mean_snow_water_equivalent_m, 0.0);
    EXPECT_GT(river.mean_snow_water_equivalent_m, 0.0);
}

TEST(V10Climate, SamplesClimateCentresAndPrecipitationVerticesSeparately)
{
    auto fields = uniformFields(1.0, 0.0);
    for (size_t p = 0U; p < 8U; ++p)
    {
        fields.local_effective_celsius[p * 1024U] = 5.0;
        fields.precipitation_mm_year[p * 65'536U] = 800.0;
    }
    V10ClimateForcing const forcing(std::move(fields));
    auto const at_centre = queryV10Climate(forcing, 1024.0, 1024.0, 0.0);
    auto const at_vertex = queryV10Climate(forcing, 0.0, 0.0, 0.0);
    EXPECT_DOUBLE_EQ(at_centre.mean_temperature_celsius, 5.0);
    EXPECT_DOUBLE_EQ(at_vertex.mean_temperature_celsius, 2.0);
    EXPECT_DOUBLE_EQ(at_vertex.mean_precipitation_mm_year, 800.0);
    EXPECT_DOUBLE_EQ(at_centre.mean_precipitation_mm_year, 0.0);
}

TEST(V10Climate, RejectsMalformedClimateShape)
{
    auto fields = uniformFields();
    fields.local_effective_celsius.pop_back();
    EXPECT_THROW(V10ClimateForcing(std::move(fields)), std::invalid_argument);
}
TEST(V10Climate, RejectsNonfiniteGround)
{
    auto fields = uniformFields();
    fields.ground_m[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(V10ClimateForcing(std::move(fields)), std::invalid_argument);
}
TEST(V10Climate, RejectsNegativePrecipitation)
{
    auto fields = uniformFields();
    fields.precipitation_mm_year[0] = -1.0;
    EXPECT_THROW(V10ClimateForcing(std::move(fields)), std::invalid_argument);
}
TEST(V10Climate, RejectsNonfiniteCoordinatesAndBed)
{
    V10ClimateForcing const forcing(uniformFields());
    EXPECT_THROW(static_cast<void>(queryV10Climate(forcing, std::numeric_limits<double>::infinity(), 0, 0)),
        std::invalid_argument);
    EXPECT_THROW(static_cast<void>(queryV10Climate(forcing, 0, 0, std::numeric_limits<double>::quiet_NaN())),
        std::invalid_argument);
}

} // namespace
