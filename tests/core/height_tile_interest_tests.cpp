#include <shared/net/Message.hpp>
#include <shared/world/HeightTileInterest.hpp>
#include <shared/world/SparseWorld.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

bool contains(shared::HeightTileInterest const& interest, int32_t const x, int32_t const y)
{
    return std::ranges::find(interest.keys, shared::HeightTileKey{.x = x, .y = y}) != interest.keys.end();
}

TEST(HeightTileInterestTest, PreservesOriginalHeadingOrderFingerprints)
{
    struct Baseline final {
        uint32_t radius;
        std::array<uint64_t, 16U> hashes;
    };
    // Captured from the original full-sort implementation, including every offset and heading.
    static constexpr std::array<Baseline, 3U> BASELINES{{
        { 1U, {
            11'254'520'555'852'257'826ULL, 13'943'013'371'091'943'897ULL,
            9'819'753'738'694'386'157ULL, 11'618'431'828'640'474'297ULL,
            12'109'331'379'025'100'178ULL, 1'382'562'719'028'028'956ULL, 9'047'317'337'558'893'130ULL, 2'802'696'263'903'018'868ULL,
            16'421'506'190'942'674'335ULL, 2'306'065'568'160'379'841ULL,
            12'377'034'909'211'740'949ULL, 13'443'844'748'545'663'561ULL,
            12'860'148'461'067'592'127ULL, 5'535'012'338'154'235'204ULL, 4'466'505'788'483'797'498ULL, 5'202'862'089'297'621'636ULL,
        } },
        { 40U, {
            16'824'413'909'184'029'722ULL, 9'466'763'224'643'782'545ULL, 858'423'624'978'917'781ULL, 15'337'253'388'917'477'745ULL,
            11'476'221'976'382'764'826ULL, 6'674'339'148'622'736'660ULL,
            13'452'472'826'443'356'514ULL, 3'156'001'089'244'112'220ULL,
            1'709'428'795'340'881'927ULL, 5'555'376'622'236'698'793ULL,
            14'982'447'306'852'398'061ULL, 15'898'250'816'453'927'121ULL,
            7'340'667'474'648'203'351ULL, 6'081'006'544'832'207'260ULL, 15'942'579'914'775'977'986ULL, 3'211'697'760'548'871'564ULL,
        } },
        { 256U, {
            7'446'899'933'587'136'746ULL, 2'188'743'064'180'132'225ULL, 17'674'738'516'632'895'749ULL, 1'156'214'908'620'347'809ULL,
            13'953'874'988'558'914'906ULL, 13'964'594'077'771'873'308ULL,
            12'970'211'630'275'343'050ULL, 15'810'096'822'904'410'708ULL,
            16'298'183'092'229'547'703ULL, 16'337'045'997'911'316'609ULL,
            18'435'645'009'032'071'629ULL, 2'066'199'828'783'967'185ULL,
            13'817'243'481'765'727'687ULL, 4'644'262'339'430'638'260ULL,
            10'464'263'245'499'507'434ULL, 2'488'582'886'254'504'844ULL,
        } },
    }};
    for (auto const& baseline : BASELINES) {
        SCOPED_TRACE(baseline.radius);
        auto const orders = shared::prepareHeightTileInterestOrders(baseline.radius);
        for (size_t index = 0U; index < orders->orders.size(); ++index) {
            SCOPED_TRACE(index);
            auto const& order = orders->orders[index];
            uint64_t hash = 14'695'981'039'346'656'037ULL;
            auto const append = [&](uint32_t const number) {
                for (uint32_t shift = 0U; shift < 32U; shift += 8U) {
                    hash = (hash ^ static_cast<uint8_t>(number >> shift)) * 1'099'511'628'211ULL;
                }
            };
            append(static_cast<uint32_t>(order.heading_x));
            append(static_cast<uint32_t>(order.heading_y));
            for (shared::HeightTileKey const key : order.keys) {
                append(static_cast<uint32_t>(key.x));
                append(static_cast<uint32_t>(key.y));
            }
            EXPECT_EQ(hash, baseline.hashes[index]);
        }
    }
}

TEST(HeightTileInterestTest, BuildsCameraIndependentCircularResidency)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 127, 0);

    ASSERT_FALSE(interest.keys.empty());
    EXPECT_EQ(interest.keys.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
    EXPECT_EQ(interest.keys.front(), center);
    EXPECT_TRUE(contains(interest, center.x + 256, center.y));
    EXPECT_TRUE(contains(interest, center.x - 256, center.y));
    EXPECT_TRUE(contains(interest, center.x, center.y + 256));
    EXPECT_TRUE(contains(interest, center.x + 181, center.y + 181));
    EXPECT_FALSE(contains(interest, center.x + 182, center.y + 182));
    EXPECT_FALSE(contains(interest, center.x + 257, center.y));
}

TEST(HeightTileInterestTest, MembershipUsesTheExactWrappedCircularRadius)
{
    static constexpr int32_t TILE_EXTENT = static_cast<int32_t>(
        shared::WorldExtent::WIDTH / shared::HEIGHT_TILE_SIDE_LENGTH
    );
    static constexpr uint32_t RADIUS = 256U;
    static constexpr std::array CENTERS{
        shared::HeightTileKey{ .x = 2'000, .y = 2'000 },
        shared::HeightTileKey{ .x = TILE_EXTENT - 1, .y = TILE_EXTENT - 1 },
        shared::HeightTileKey{ .x = -1, .y = TILE_EXTENT + 2 },
    };

    for (shared::HeightTileKey const center : CENTERS) {
        EXPECT_TRUE(shared::heightTileWithinInterest(center, center, RADIUS));
        EXPECT_TRUE(shared::heightTileWithinInterest(
            center,
            { .x = center.x + static_cast<int32_t>(RADIUS), .y = center.y },
            RADIUS
        ));
        EXPECT_FALSE(shared::heightTileWithinInterest(
            center,
            { .x = center.x + static_cast<int32_t>(RADIUS) + 1, .y = center.y },
            RADIUS
        ));
        EXPECT_TRUE(shared::heightTileWithinInterest(
            center,
            { .x = center.x + 181, .y = center.y + 181 },
            RADIUS
        ));
        EXPECT_FALSE(shared::heightTileWithinInterest(
            center,
            { .x = center.x + 182, .y = center.y + 182 },
            RADIUS
        ));
    }
}

TEST(HeightTileInterestTest, DistributesFullDiskWrappedAndNegativeKeysAcrossPowerOfTwoBuckets)
{
    static constexpr uint32_t BUCKET_COUNT = 262'144U;
    static constexpr uint32_t MINIMUM_OCCUPIED_BUCKETS = BUCKET_COUNT / 2U;
    static constexpr uint32_t MAXIMUM_BUCKET_OCCUPANCY = 16U;
    static constexpr std::array CENTERS{
        shared::HeightTileKey{.x = 2'048, .y = 2'048},
        shared::HeightTileKey{.x = 4'095, .y = 0},
        shared::HeightTileKey{.x = -2'048, .y = -2'048},
    };
    for (shared::HeightTileKey const center : CENTERS) {
        std::vector<uint32_t> buckets(BUCKET_COUNT, 0U);
        shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 127, 0);
        ASSERT_EQ(interest.keys.size(), shared::HEIGHT_TILE_INTEREST_COUNT);
        for (shared::HeightTileKey const key : interest.keys) {
            uint64_t const hash = shared::heightTileCoordinateHash(
                center.x < 0 ? key.x - 4'096 : key.x,
                center.y < 0 ? key.y - 4'096 : key.y
            );
            ++buckets[hash & (BUCKET_COUNT - 1U)];
        }
        EXPECT_GE(std::ranges::count_if(buckets, [](uint32_t const count) { return count != 0U; }),
            MINIMUM_OCCUPIED_BUCKETS);
        EXPECT_LE(*std::ranges::max_element(buckets), MAXIMUM_BUCKET_OCCUPANCY);
    }
}

TEST(HeightTileInterestTest, RotationChangesPriorityWithoutChangingResidency)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const east = shared::makeHeightTileInterest(center, 127, 0);
    shared::HeightTileInterest const north = shared::makeHeightTileInterest(center, 0, 127);

    std::vector<shared::HeightTileKey> east_keys = east.keys;
    std::vector<shared::HeightTileKey> north_keys = north.keys;
    std::ranges::sort(east_keys, {}, [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; });
    std::ranges::sort(north_keys, {}, [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; });
    EXPECT_EQ(east_keys, north_keys);
    EXPECT_NE(east.keys, north.keys);
    EXPECT_EQ(east.keys.size(), 205'861U);
}

TEST(HeightTileInterestTest, ProgressivePriorityOrdersByDistanceBeforeHeading)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 127, 0);

    int64_t previous_distance_squared = -1;
    for (shared::HeightTileKey const key : interest.keys) {
        int64_t const x = key.x - center.x;
        int64_t const y = key.y - center.y;
        int64_t const distance_squared = x * x + y * y;
        EXPECT_GE(distance_squared, previous_distance_squared);
        previous_distance_squared = distance_squared;
    }
    for (shared::HeightTileKey const key : std::span{interest.keys}.first(29U)) {
        int32_t const x = key.x - center.x;
        int32_t const y = key.y - center.y;
        EXPECT_LE(x * x + y * y, 9);
    }
    for (shared::HeightTileKey const key : std::span{interest.keys}.first(197U)) {
        int32_t const x = key.x - center.x;
        int32_t const y = key.y - center.y;
        EXPECT_LE(x * x + y * y, 64);
    }
}

TEST(HeightTileInterestTest, WrapsAtWorldBoundariesWithoutChangingShape)
{
    shared::HeightTileInterest const interior = shared::makeHeightTileInterest({.x = 2'000, .y = 2'000}, 127, 0);
    shared::HeightTileInterest const boundary = shared::makeHeightTileInterest({.x = 4'095, .y = 4'095}, 127, 0);

    EXPECT_EQ(boundary.keys.size(), interior.keys.size());
    EXPECT_TRUE(contains(boundary, 0, 4'095));
    EXPECT_TRUE(contains(boundary, 4'050, 4'095));
    EXPECT_EQ(boundary.keys.front(), (shared::HeightTileKey{.x = 4'095, .y = 4'095}));
    auto const displacement = [](int32_t const center, int32_t const coordinate) {
        int32_t value = coordinate - center;
        if (value > 2'048) {
            value -= 4'096;
        } else if (value < -2'048) {
            value += 4'096;
        }
        return value;
    };
    for (uint32_t index = 0U; index < interior.keys.size(); ++index) {
        EXPECT_EQ(
            std::pair(displacement(2'000, interior.keys[index].x), displacement(2'000, interior.keys[index].y)),
            std::pair(displacement(4'095, boundary.keys[index].x), displacement(4'095, boundary.keys[index].y))
        ) << "priority order changed at index " << index;
    }
}

TEST(HeightTileInterestTest, KeepsResidencyStableForSmallHeadingJitter)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const first = shared::makeHeightTileInterest(center, 127, 0);
    shared::HeightTileInterest const jittered = shared::makeHeightTileInterest(center, 127, 8);
    shared::HeightTileInterest const turned = shared::makeHeightTileInterest(center, 117, 49);

    EXPECT_EQ(jittered.keys, first.keys);
    EXPECT_NE(turned.keys, first.keys);
    std::vector<shared::HeightTileKey> first_membership = first.keys;
    std::vector<shared::HeightTileKey> turned_membership = turned.keys;
    std::ranges::sort(first_membership, {}, [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; });
    std::ranges::sort(turned_membership, {}, [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; });
    EXPECT_EQ(turned_membership, first_membership);
}

TEST(HeightTileInterestTest, SelectedRadiusOrdersPreserveExactDiskAcrossHeadingsAndWrap)
{
    static constexpr uint32_t RADIUS_128 = 128U;
    static constexpr uint32_t RADIUS_256 = 256U;
    static constexpr uint32_t COUNT_128 = 51'433U;
    static constexpr uint32_t COUNT_256 = 205'861U;
    static constexpr int32_t TILE_EXTENT = 4'096;
    static constexpr shared::HeightTileKey INTERIOR{ .x = 2'000, .y = 2'000 };
    static constexpr shared::HeightTileKey BOUNDARY{ .x = TILE_EXTENT - 1, .y = TILE_EXTENT - 1 };
    static constexpr std::array<uint32_t, 2> RADII{ RADIUS_128, RADIUS_256 };

    std::shared_ptr<shared::HeightTileInterestOrders const> retained_orders;
    for (uint32_t const radius : RADII) {
        auto const orders = shared::prepareHeightTileInterestOrders(radius);
        ASSERT_NE(orders, nullptr);
        EXPECT_EQ(orders->radius, radius);
        if (radius == RADIUS_128) {
            retained_orders = orders;
        }

        uint32_t const expected_count = radius == RADIUS_128 ? COUNT_128 : COUNT_256;
        for (shared::HeightTileKey const center : { INTERIOR, BOUNDARY }) {
            std::vector<shared::HeightTileKey> expected_membership;
            for (uint32_t heading_index = 0U; heading_index < orders->orders.size(); ++heading_index) {
                auto const heading = orders->orders[heading_index];
                auto const interest = shared::makeHeightTileInterest(
                    center, heading.heading_x, heading.heading_y, *orders
                );
                ASSERT_EQ(interest.keys.size(), expected_count);
                EXPECT_EQ(interest.keys.front(), center);
                if (heading_index == 0U) {
                    expected_membership = interest.keys;
                    std::ranges::sort(
                        expected_membership,
                        {},
                        [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; }
                    );
                } else {
                    auto membership = interest.keys;
                    std::ranges::sort(
                        membership,
                        {},
                        [](shared::HeightTileKey const key) { return std::pair{key.x, key.y}; }
                    );
                    EXPECT_EQ(membership, expected_membership);
                }
            }

            auto direct = shared::makeHeightTileInterest(center, 127, 0, radius);
            std::ranges::sort(direct.keys, {}, [](shared::HeightTileKey const key) {
                return std::pair{key.x, key.y};
            });
            EXPECT_EQ(direct.keys, expected_membership);
        }
    }

    ASSERT_NE(retained_orders, nullptr);
    EXPECT_EQ(retained_orders->radius, RADIUS_128);
    for (auto const& order : retained_orders->orders) {
        EXPECT_EQ(order.keys.size(), COUNT_128);
        int64_t previous_distance_squared = -1;
        for (shared::HeightTileKey const offset : order.keys) {
            int64_t const distance_squared = static_cast<int64_t>(offset.x) * offset.x
                + static_cast<int64_t>(offset.y) * offset.y;
            EXPECT_GE(distance_squared, previous_distance_squared);
            previous_distance_squared = distance_squared;
        }
    }
}

TEST(HeightTileInterestTest, ClassifiesFixedNearBandsDirectionalMiddleAndBackground)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    using enum shared::HeightTileGenerationBand;
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, center), Immediate);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'003, .y = 2'000}), Immediate);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'008, .y = 2'000}), Near);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'040, .y = 2'000}), Directional);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'070, .y = 2'000}), Directional);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'080, .y = 2'000}), Directional);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'081, .y = 2'000}), Background);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 1'960, .y = 2'000}), Background);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 0, 127, {.x = 2'000, .y = 2'040}), Directional);
    EXPECT_EQ(shared::heightTileGenerationBand(center, 127, 0, {.x = 2'000, .y = 2'030}), Background);
}

TEST(HeightTileInterestTest, NearestDistanceDominatesHeadingAndHeadingBreaksRingTies)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    EXPECT_LT(
        shared::heightTileInterestPriority(center, 127, 0, {.x = 2'010, .y = 2'000}),
        shared::heightTileInterestPriority(center, 127, 0, {.x = 2'000, .y = 2'010})
    );
    EXPECT_LT(
        shared::heightTileInterestPriority(center, 127, 0, {.x = 2'000, .y = 2'010}),
        shared::heightTileInterestPriority(center, 127, 0, {.x = 2'070, .y = 2'000})
    );
}

TEST(HeightTileInterestTest, SelectsEligibleRemovalsBeyondInflightPriorityPrefix)
{
    static constexpr uint32_t DELIVERY_CAPACITY = 16U;
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileInterest const interest = shared::makeHeightTileInterest(center, 127, 0);
    std::vector<shared::HeightTileKey> pending{
        interest.keys.rbegin(),
        interest.keys.rbegin()
            + static_cast<std::vector<shared::HeightTileKey>::difference_type>(2U) * DELIVERY_CAPACITY
    };
    std::vector<shared::HeightTileKey> const inflight{
        pending.begin(), pending.begin() + DELIVERY_CAPACITY
    };

    std::vector<shared::HeightTileKey> const selected = shared::selectHeightTileRemovalCandidates(
        center,
        127,
        0,
        pending,
        inflight,
        DELIVERY_CAPACITY
    );

    ASSERT_EQ(selected.size(), DELIVERY_CAPACITY);
    for (shared::HeightTileKey const key : selected) {
        EXPECT_EQ(std::ranges::find(inflight, key), inflight.end());
    }
}

TEST(HeightTileInterestTest, RemovalSelectionBreaksEqualPriorityTiesDeterministically)
{
    shared::HeightTileKey constexpr center{.x = 2'000, .y = 2'000};
    shared::HeightTileKey constexpr positive{.x = 2'010, .y = 2'000};
    shared::HeightTileKey constexpr negative{.x = 1'990, .y = 2'000};
    std::array<shared::HeightTileKey, 2> const forward{positive, negative};
    std::array<shared::HeightTileKey, 2> const reverse{negative, positive};

    std::vector<shared::HeightTileKey> const first = shared::selectHeightTileRemovalCandidates(
        center,
        0,
        127,
        forward,
        {},
        1U
    );
    std::vector<shared::HeightTileKey> const second = shared::selectHeightTileRemovalCandidates(
        center,
        0,
        127,
        reverse,
        {},
        1U
    );

    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.front(), positive);
}

TEST(HeightTileInterestTest, DeltaOutputReusesStorageAcrossMovementWrapAndNoChange)
{
    static constexpr uint32_t TRANSITION_COUNT = 8U;
    static constexpr uint32_t OUTPUT_CAPACITY = 256U;
    static constexpr std::array<shared::HeightTileKey, TRANSITION_COUNT + 1U> CENTERS{
        shared::HeightTileKey{ .x = 100, .y = 200 },
        shared::HeightTileKey{ .x = 101, .y = 200 },
        shared::HeightTileKey{ .x = 101, .y = 201 },
        shared::HeightTileKey{ .x = 102, .y = 201 },
        shared::HeightTileKey{ .x = 4'095, .y = 300 },
        shared::HeightTileKey{ .x = 0, .y = 300 },
        shared::HeightTileKey{ .x = 4'095, .y = 300 },
        shared::HeightTileKey{ .x = 4'095, .y = 300 },
        shared::HeightTileKey{ .x = 100, .y = 200 },
    };
    static constexpr std::array<uint32_t, TRANSITION_COUNT> RADII{
        8U, 8U, 8U, 8U, 8U, 8U, 8U, 4U,
    };
    auto const keySet = [](std::vector<shared::HeightTileKey> const& keys) {
        std::set<std::pair<int32_t, int32_t>> result;
        for (shared::HeightTileKey const key : keys) {
            result.emplace(key.x, key.y);
        }
        return result;
    };
    shared::HeightTileInterestDelta reused;
    reused.additions.reserve(OUTPUT_CAPACITY);
    reused.removals.reserve(OUTPUT_CAPACITY);
    shared::HeightTileKey const* const additions_storage = reused.additions.data();
    shared::HeightTileKey const* const removals_storage = reused.removals.data();
    uint32_t const additions_capacity = static_cast<uint32_t>(reused.additions.capacity());
    uint32_t const removals_capacity = static_cast<uint32_t>(reused.removals.capacity());

    for (uint32_t index = 0U; index < TRANSITION_COUNT; ++index) {
        shared::HeightTileKey const previous_center = CENTERS[index];
        shared::HeightTileKey const next_center = CENTERS[index + 1U];
        uint32_t const radius = RADII[index];
        shared::heightTileInterestDelta(previous_center, next_center, radius, reused);
        shared::HeightTileInterestDelta const returned = shared::heightTileInterestDelta(
            previous_center, next_center, radius
        );
        EXPECT_EQ(reused.additions, returned.additions);
        EXPECT_EQ(reused.removals, returned.removals);

        std::vector<shared::HeightTileKey> const previous_keys = shared::makeHeightTileInterest(
            previous_center, 0, 127, radius
        ).keys;
        std::vector<shared::HeightTileKey> const next_keys = shared::makeHeightTileInterest(
            next_center, 0, 127, radius
        ).keys;
        std::set<std::pair<int32_t, int32_t>> const previous_set = keySet(previous_keys);
        std::set<std::pair<int32_t, int32_t>> const next_set = keySet(next_keys);
        std::set<std::pair<int32_t, int32_t>> expected_additions;
        std::set<std::pair<int32_t, int32_t>> expected_removals;
        for (shared::HeightTileKey const key : next_keys) {
            if (!previous_set.contains({ key.x, key.y })) {
                expected_additions.emplace(key.x, key.y);
            }
        }
        for (shared::HeightTileKey const key : previous_keys) {
            if (!next_set.contains({ key.x, key.y })) {
                expected_removals.emplace(key.x, key.y);
            }
        }
        EXPECT_EQ(keySet(reused.additions), expected_additions);
        EXPECT_EQ(keySet(reused.removals), expected_removals);
        EXPECT_EQ(reused.additions.data(), additions_storage);
        EXPECT_EQ(reused.removals.data(), removals_storage);
        EXPECT_EQ(reused.additions.capacity(), additions_capacity);
        EXPECT_EQ(reused.removals.capacity(), removals_capacity);
    }
}

} // namespace
