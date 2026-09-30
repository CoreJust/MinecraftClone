#include <client/Camera.hpp>
#include <client/render/HeightTileDrawIndex.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Index = client::HeightTileDrawIndex;
using Range = client::StoneIndirectDraws::Range;
using PackedRanges = std::vector<std::pair<uint32_t, uint32_t>>;

struct Reference final {
    std::vector<Range> ranges;
    uint64_t plane_tests = 0U;
};

[[nodiscard]]
PackedRanges pack(std::vector<Range> const& ranges)
{
    PackedRanges result;
    result.reserve(ranges.size());
    for (Range const range : ranges) {
        result.emplace_back(range.first_instance, range.instance_count);
    }
    return result;
}

[[nodiscard]]
Reference reference(
    std::vector<Index::Entry> const& entries,
    client::VulkanFrustum const& frustum,
    glm::dvec3 const camera_position
)
{
    Reference result;
    result.ranges.reserve(entries.size());
    for (Index::Entry const& entry : entries) {
        if (entry.coordinate.x < 0 || entry.coordinate.y < 0) {
            continue;
        }
        client::WrappedBounds const wrapped = client::boundsNearestToCamera(
            entry.minimum, entry.maximum, camera_position
        );
        uint32_t plane_tests = 0U;
        bool const visible = frustum.intersects(
            wrapped.minimum, wrapped.maximum, client::VulkanFrustum::ALL_PLANES, plane_tests
        );
        result.plane_tests += plane_tests;
        if (visible) {
            result.ranges.push_back(entry.range);
        }
    }
    std::ranges::sort(result.ranges, {}, &Range::first_instance);
    std::vector<Range> merged;
    merged.reserve(result.ranges.size());
    for (Range const range : result.ranges) {
        if (!merged.empty() && merged.back().first_instance + merged.back().instance_count == range.first_instance) {
            merged.back().instance_count += range.instance_count;
        } else {
            merged.push_back(range);
        }
    }
    result.ranges = std::move(merged);
    return result;
}

[[nodiscard]]
Index::Entry insideEntry(shared::HeightTileCoordinate const coordinate, Range const range)
{
    return { .coordinate = coordinate, .range = range, .minimum = { -0.5F, -0.5F, 0.1F },
        .maximum = { 0.5F, 0.5F, 0.9F } };
}

TEST(HeightTileDrawIndexTest, IncrementalReplacementRemovalResetAndArenaReuseMatchSortedLiveRanges)
{
    static constexpr uint32_t MAXIMUM_UPDATE_SLOTS = 256U;
    std::vector<Index::Entry> entries{
        insideEntry({ .x = 2, .y = 3 }, { 100U, 2U }),
        insideEntry({ .x = 3, .y = 3 }, { 10U, 4U }),
        insideEntry({ .x = 4, .y = 3 }, { 14U, 6U }),
    };
    Index index;
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    std::vector<Range> actual;
    auto const verify = [&] {
        static_cast<void>(index.collect(frustum, {}, actual));
        EXPECT_EQ(pack(actual), pack(reference(entries, frustum, {}).ranges));
        EXPECT_LE(index.lastUpdateSlotsInspected(), MAXIMUM_UPDATE_SLOTS);
    };
    for (Index::Entry const entry : entries) {
        index.upsert(entry);
    }
    verify();
    EXPECT_EQ(pack(actual), (PackedRanges{ { 10U, 10U }, { 100U, 2U } }));
    entries[0].range = { 40U, 8U };
    index.upsert(entries[0]);
    verify();
    entries[1].maximum = { 0.8F, 0.8F, 0.9F };
    index.upsert(entries[1]);
    verify();
    index.remove(entries[2].coordinate);
    entries.pop_back();
    verify();
    EXPECT_EQ(pack(actual), (PackedRanges{ { 10U, 4U }, { 40U, 8U } }));
    entries.push_back(insideEntry({ .x = 5, .y = 3 }, { 14U, 6U }));
    index.upsert(entries.back());
    verify();
    EXPECT_EQ(pack(actual), (PackedRanges{ { 10U, 10U }, { 40U, 8U } }));
    index.remove({ .x = 99, .y = 99 });
    verify();
    index.clear();
    entries.clear();
    verify();
    entries.push_back(insideEntry({ .x = 2, .y = 3 }, { 0U, 3U }));
    index.upsert(entries.back());
    verify();
}

TEST(HeightTileDrawIndexTest, RejectedOccupiedRangeKeepsBothPublishedEntriesUnchanged)
{
    std::vector<Index::Entry> const entries{
        insideEntry({ .x = 1, .y = 1 }, { 10U, 3U }),
        insideEntry({ .x = 2, .y = 1 }, { 20U, 5U }),
    };
    Index index;
    for (Index::Entry const entry : entries) {
        index.upsert(entry);
    }
    try {
        index.upsert(insideEntry({ .x = 99, .y = 99 }, { 10U, 5U }));
        FAIL() << "occupied live range was replaced";
    } catch (std::invalid_argument const& error) {
        EXPECT_STREQ(error.what(), "height tile draw index range is already occupied");
    }
    std::vector<Range> actual;
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    Index::Work const work = index.collect(frustum, {}, actual);
    EXPECT_EQ(work.records, entries.size());
    EXPECT_EQ(work.groups, 1U);
    EXPECT_EQ(pack(actual), pack(reference(entries, frustum, {}).ranges));
}

TEST(HeightTileDrawIndexTest, RejectsInvalidRangeBeforeChangingPublishedVisibility)
{
    static constexpr uint32_t MAXIMUM_RANGE_START = std::numeric_limits<uint32_t>::max();
    Index index;
    index.upsert(insideEntry({ .x = 1, .y = 1 }, { 10U, 3U }));
    std::array const invalid_ranges{ Range{ 20U, 0U }, Range{ MAXIMUM_RANGE_START, 1U } };
    for (Range const range : invalid_ranges) {
        try {
            index.upsert(insideEntry({ .x = 1, .y = 1 }, range));
            FAIL() << "invalid range was published";
        } catch (std::invalid_argument const& error) {
            EXPECT_STREQ(error.what(), "height tile draw index range is invalid");
        }
    }
    std::vector<Range> actual;
    Index::Work const work = index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, actual);
    EXPECT_EQ(work.records, 1U);
    EXPECT_EQ(pack(actual), (PackedRanges{ { 10U, 3U } }));
}

TEST(HeightTileDrawIndexTest, KeepsNegativeCoordinateFallbackSeparateFromNormalLiveRanges)
{
    Index index;
    index.upsert(insideEntry({ .x = -1, .y = 0 }, { 0U, 3U }));
    index.upsert(insideEntry({ .x = 0, .y = 0 }, { 3U, 5U }));
    index.upsert(insideEntry({ .x = 0, .y = -1 }, { 8U, 7U }));
    EXPECT_TRUE(index.hasFallbackRecords());
    std::vector<Range> actual;
    static_cast<void>(index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, actual));
    EXPECT_EQ(pack(actual), (PackedRanges{ { 3U, 5U } }));
    index.remove({ .x = -1, .y = 0 });
    EXPECT_TRUE(index.hasFallbackRecords());
    index.remove({ .x = 0, .y = -1 });
    EXPECT_FALSE(index.hasFallbackRecords());
    index.clear();
    EXPECT_FALSE(index.hasFallbackRecords());
}

TEST(HeightTileDrawIndexTest, AmbiguousNearestWrapCutUsesExactIndividualImages)
{
    static constexpr float WORLD_PERIOD = 65'536.0F;
    static constexpr double CAMERA_X = 32'776.0;
    std::vector<Index::Entry> const entries{
        { .coordinate = { .x = 0, .y = 0 }, .range = { 0U, 3U },
            .minimum = { 0.0F, 0.0F, 0.1F }, .maximum = { 16.0F, 16.0F, 0.9F } },
        { .coordinate = { .x = 1, .y = 0 }, .range = { 10U, 5U },
            .minimum = { 16.0F, 0.0F, 0.1F }, .maximum = { 32.0F, 16.0F, 0.9F } },
    };
    Index index;
    for (Index::Entry const entry : entries) {
        index.upsert(entry);
    }
    client::VulkanFrustum const frustum{ glm::scale(glm::mat4{ 1.0F }, { 1.0F / WORLD_PERIOD,
        1.0F / WORLD_PERIOD, 1.0F }) };
    glm::dvec3 const camera_position{ CAMERA_X, 0.0, 0.0 };
    std::vector<Range> actual;
    Index::Work const work = index.collect(frustum, camera_position, actual);
    EXPECT_EQ(work.wrap_ambiguous_groups, 1U);
    EXPECT_EQ(work.group_plane_tests, 0U);
    EXPECT_EQ(pack(actual), pack(reference(entries, frustum, camera_position).ranges));
    EXPECT_EQ(actual.size(), 2U);
}

TEST(HeightTileDrawIndexTest, ExactOracleCoversMovingRotatingWrappedCamerasAndWideRenderDistances)
{
    static constexpr uint32_t RECORD_COUNT = 12'000U;
    static constexpr int32_t WORLD_TILE_SIDE = 4'096;
    static constexpr uint32_t WIDTH = 1'600U;
    static constexpr uint32_t HEIGHT = 900U;
    static constexpr std::array<int32_t, 2> RADII{ 256, 1'024 };
    static constexpr std::array<double, 3> FOVS{ 40.0, 100.0, 150.0 };
    static constexpr std::array<double, 4> HEADINGS{ 0.0, 91.0, 183.0, 277.0 };
    static constexpr std::array<glm::dvec3, 3> POSITIONS{
        glm::dvec3{ 65'520.0, 48.0, 120.0 },
        glm::dvec3{ 65'550.0, 16.0, 40.0 },
        glm::dvec3{ -16.0, 80.0, 280.0 },
    };
    for (int32_t const radius : RADII) {
        Index index;
        std::vector<Index::Entry> entries;
        entries.reserve(RECORD_COUNT);
        uint32_t const diameter = static_cast<uint32_t>(radius * 2 + 1);
        for (uint32_t ordinal = 0U; ordinal < RECORD_COUNT; ++ordinal) {
            uint32_t const position = (ordinal * 7'919U) % (diameter * diameter);
            int32_t const tile_x = (WORLD_TILE_SIDE - 1 + static_cast<int32_t>(position % diameter)
                - radius + WORLD_TILE_SIDE) % WORLD_TILE_SIDE;
            int32_t const tile_y = (3 + static_cast<int32_t>(position / diameter)
                - radius + WORLD_TILE_SIDE) % WORLD_TILE_SIDE;
            float const x = static_cast<float>(tile_x * shared::HeightTile::SIDE_LENGTH);
            float const y = static_cast<float>(tile_y * shared::HeightTile::SIDE_LENGTH);
            float const z = static_cast<float>((ordinal * 37U) % 250U);
            entries.push_back({ .coordinate = { .x = tile_x, .y = tile_y }, .range = { ordinal * 5U, 3U },
                .minimum = { x, y, z }, .maximum = { x + 16.0F, y + 16.0F, z + 32.0F } });
            index.upsert(entries.back());
        }
        for (double const fov : FOVS) {
            for (glm::dvec3 const position : POSITIONS) {
                for (double const heading : HEADINGS) {
                    SCOPED_TRACE(radius);
                    SCOPED_TRACE(fov);
                    SCOPED_TRACE(heading);
                    client::Camera const camera{ { .position = position, .angles = { .yaw_degrees = heading,
                        .pitch_degrees = -17.0 } }, { .vertical_fov_degrees = fov, .far_plane = 20'000.0 } };
                    client::VulkanFrustum const frustum{ *camera.projectionMatrix(WIDTH, HEIGHT) * camera.viewMatrix() };
                    std::vector<Range> actual;
                    static_cast<void>(index.collect(frustum, position, actual));
                    EXPECT_EQ(pack(actual), pack(reference(entries, frustum, position).ranges));
                }
            }
        }
    }
}

TEST(HeightTileDrawIndexTest, FullResidencyMatchesOracleWithBoundedUpdateAndReducedPlaneWorkAtDifferentViews)
{
    static constexpr int32_t RADIUS = 256;
    static constexpr int32_t CENTER_TILE = 2'048;
    static constexpr uint32_t RESIDENT_COUNT = 205'861U;
    static constexpr uint32_t MAXIMUM_UPDATE_SLOTS = 256U;
    static constexpr uint32_t WIDTH = 1'600U;
    static constexpr uint32_t HEIGHT = 900U;
    static constexpr std::array<double, 4> HEADINGS{ 0.0, 45.0, 137.0, 271.0 };
    Index index;
    std::vector<Index::Entry> entries;
    entries.reserve(RESIDENT_COUNT);
    for (int32_t y = -RADIUS; y <= RADIUS; ++y) {
        for (int32_t x = -RADIUS; x <= RADIUS; ++x) {
            if (x * x + y * y > RADIUS * RADIUS) {
                continue;
            }
            uint32_t const ordinal = static_cast<uint32_t>(entries.size());
            int32_t const tile_x = CENTER_TILE + x;
            int32_t const tile_y = CENTER_TILE + y;
            float const origin_x = static_cast<float>(tile_x * shared::HeightTile::SIDE_LENGTH);
            float const origin_y = static_cast<float>(tile_y * shared::HeightTile::SIDE_LENGTH);
            float const z = static_cast<float>((x * x + y * y) % 64);
            entries.push_back({ .coordinate = { .x = tile_x, .y = tile_y }, .range = { ordinal * 3U, 3U },
                .minimum = { origin_x, origin_y, z }, .maximum = { origin_x + 16.0F, origin_y + 16.0F, z + 16.0F } });
            index.upsert(entries.back());
        }
    }
    ASSERT_EQ(entries.size(), RESIDENT_COUNT);
    EXPECT_LE(index.lastUpdateSlotsInspected(), MAXIMUM_UPDATE_SLOTS);
    for (double const heading : HEADINGS) {
        SCOPED_TRACE(heading);
        client::Camera const camera{ { .position = { 32'768.0, 32'768.0, 200.0 },
            .angles = { .yaw_degrees = heading, .pitch_degrees = -20.0 } }, { .far_plane = 8'192.0 } };
        client::VulkanFrustum const frustum{ *camera.projectionMatrix(WIDTH, HEIGHT) * camera.viewMatrix() };
        std::vector<Range> actual;
        Index::Work const work = index.collect(frustum, camera.pose().position, actual);
        Reference const expected = reference(entries, frustum, camera.pose().position);
        EXPECT_EQ(pack(actual), pack(expected.ranges));
        EXPECT_EQ(work.records, RESIDENT_COUNT);
        EXPECT_LT(work.group_plane_tests + work.tile_plane_tests, expected.plane_tests / 4U);
        RecordProperty("plane_tests_" + std::to_string(static_cast<uint32_t>(heading)),
            work.group_plane_tests + work.tile_plane_tests);
        RecordProperty("reference_plane_tests_" + std::to_string(static_cast<uint32_t>(heading)), expected.plane_tests);
    }
}

} // namespace
