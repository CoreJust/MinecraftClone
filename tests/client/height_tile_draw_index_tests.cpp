#include <client/Camera.hpp>
#include <client/render/HeightTileDrawIndex.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/VulkanFrustum.hpp>

#include <shared/world/HeightTileSurfaceMesher.hpp>
#include <shared/world/WorldGeneration.hpp>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
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

[[nodiscard]]
double facingOracle(shared::HeightTileSurfaceQuad const quad, glm::dvec3 const eye)
{
    static constexpr std::array<glm::dvec3, 6> NORMALS{
        glm::dvec3{ -1.0, 0.0, 0.0 }, glm::dvec3{ 1.0, 0.0, 0.0 },
        glm::dvec3{ 0.0, -1.0, 0.0 }, glm::dvec3{ 0.0, 1.0, 0.0 },
        glm::dvec3{ 0.0, 0.0, -1.0 }, glm::dvec3{ 0.0, 0.0, 1.0 },
    };
    uint32_t const direction = static_cast<uint32_t>(quad.direction);
    glm::dvec3 relative{ static_cast<double>(quad.x), static_cast<double>(quad.y), static_cast<double>(quad.z) };
    glm::dvec3 local_eye{};
    for (int32_t axis = 0; axis < 3; ++axis) {
        double const origin = std::floor(eye[axis]);
        relative[axis] -= origin;
        local_eye[axis] = eye[axis] - origin;
        if (axis < 2) {
            if (relative[axis] > 32'768.0) {
                relative[axis] -= 65'536.0;
            }
            if (relative[axis] < -32'768.0) {
                relative[axis] += 65'536.0;
            }
        }
    }
    if ((direction & 1U) != 0U) {
        relative[static_cast<int32_t>(direction / 2U)] += 1.0;
    }
    glm::dvec3 const difference = local_eye - relative;
    glm::dvec3 const normal = NORMALS[direction];
    return difference.x * normal.x + difference.y * normal.y + difference.z * normal.z;
}

void expectEveryRejectedQuadBackFacing(
    std::vector<shared::HeightTileSurfaceQuad> quads,
    glm::dvec3 const eye
)
{
    Index::DirectionalSpans const spans = Index::directionalSpans(quads);
    std::ranges::stable_sort(quads, {}, &shared::HeightTileSurfaceQuad::direction);
    std::vector<Range> ranges;
    Index::appendVisibleRanges({ 0U, static_cast<uint32_t>(quads.size()) }, spans, eye, ranges);
    for (uint32_t ordinal = 0U; ordinal < quads.size(); ++ordinal) {
        bool const submitted = std::ranges::any_of(ranges, [ordinal](Range const range) {
            return ordinal >= range.first_instance && ordinal - range.first_instance < range.instance_count;
        });
        double const facing = facingOracle(quads[ordinal], eye);
        EXPECT_TRUE(submitted || facing < 0.0)
            << "rejected front/coplanar quad " << ordinal << " at eye " << eye.x << "," << eye.y << "," << eye.z;
    }
}

TEST(HeightTileDrawIndexTest, DirectionalSpansRejectOnlyExhaustivelyProvenBackFacingQuads)
{
    static constexpr std::array<int32_t, 9> ORIGINS{ -65'536, -1, 0, 1, 16, 32'768, 32'769, 65'535, 65'536 };
    static constexpr std::array<double, 16> EYE_COORDINATES{
        -65'536.5, -32'769.0, -1.5, -1.0, -0.5, 0.0, 0.000'000'01, 0.5,
        1.0, 1.5, 16.0, 32'768.0, 32'768.5, 65'535.5, 65'536.0, 65'550.5,
    };
    for (uint32_t direction = 0U; direction < 6U; ++direction) {
        for (int32_t const origin : ORIGINS) {
            std::vector<shared::HeightTileSurfaceQuad> const quads{
                { .x = origin, .y = origin, .z = origin,
                    .direction = static_cast<shared::HeightTileSurfaceDirection>(direction),
                    .u_extent = 16U, .v_extent = 65U },
                { .x = origin + 1, .y = origin + 1, .z = origin + 1,
                    .direction = static_cast<shared::HeightTileSurfaceDirection>(direction) },
            };
            for (double const coordinate : EYE_COORDINATES) {
                expectEveryRejectedQuadBackFacing(quads, { coordinate, coordinate, coordinate });
            }
        }
    }
    std::vector<shared::HeightTileSurfaceQuad> mixed;
    for (int32_t const origin : ORIGINS) {
        for (uint32_t direction = 0U; direction < 6U; ++direction) {
            mixed.push_back({ .x = origin, .y = origin, .z = origin,
                .direction = static_cast<shared::HeightTileSurfaceDirection>(direction) });
        }
    }
    for (double const x : EYE_COORDINATES) {
        for (double const y : EYE_COORDINATES) {
            for (double const z : EYE_COORDINATES) {
                expectEveryRejectedQuadBackFacing(mixed, { x, y, z });
            }
        }
    }
}

TEST(HeightTileDrawIndexTest, DirectionalSpansIncludeAppendedSeamBridgesAndRetainWrapCutAmbiguity)
{
    shared::HeightTile first{ .coordinate = { .x = 4'095, .y = 0 } };
    shared::HeightTile second{ .coordinate = { .x = 0, .y = 0 } };
    first.heights.fill(8U);
    second.heights.fill(3U);
    shared::HeightTileSurfaceMesher const mesher;
    shared::HeightTileSurfaceMesh mesh = mesher.build(
        first, { .positive_x = first.heights }, shared::HeightTileSurfaceDetail::Coarse
    );
    shared::HeightTileSurfaceMesh const neighbor = mesher.build(second);
    shared::HeightTileSurfaceSeamBridge const bridge = shared::heightTileSurfaceSeamBridge(
        mesh, neighbor, shared::HeightTileSurfaceDirection::PositiveX
    );
    ASSERT_FALSE(bridge.first.empty());
    mesh.quads.insert(mesh.quads.end(), bridge.first.begin(), bridge.first.end());
    for (double const x : { -8.5, 0.0, 65'520.0, 65'535.5, 65'536.0, 98'288.5 }) {
        for (double const z : { 2.0, 3.0, 7.0, 8.0, 12.0 }) {
            expectEveryRejectedQuadBackFacing(mesh.quads, { x, 8.0, z });
        }
    }
    Index::DirectionalSpans spans{};
    spans[1] = { .instance_count = 2U, .minimum_origin = 0, .maximum_origin = 16 };
    std::vector<Range> ranges;
    Index::appendVisibleRanges({ 10U, 2U }, spans, { 32'776.5, 0.0, 0.0 }, ranges);
    EXPECT_EQ(pack(ranges), (PackedRanges{ { 10U, 2U } }));
}

TEST(HeightTileDrawIndexTest, DirectionSelectionKeepsCoplanarAndMixedPlanesAndMergesLiveRanges)
{
    Index::Entry entry = insideEntry({ .x = 0, .y = 0 }, { 10U, 5U });
    entry.directional_spans[0] = { .instance_count = 1U, .minimum_origin = -2, .maximum_origin = -2 };
    entry.directional_spans[1] = { .instance_count = 2U, .minimum_origin = -1, .maximum_origin = 1 };
    entry.directional_spans[4] = { .instance_count = 1U, .minimum_origin = 1, .maximum_origin = 1 };
    entry.directional_spans[5] = { .instance_count = 1U, .minimum_origin = -1, .maximum_origin = -1 };
    Index index;
    index.upsert(entry);
    std::vector<Range> ranges;
    static_cast<void>(index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, ranges));
    EXPECT_EQ(pack(ranges), (PackedRanges{ { 11U, 4U } }));
    std::vector<Range> disabled;
    Index::appendVisibleRanges(entry.range, {}, {}, disabled);
    EXPECT_EQ(pack(disabled), (PackedRanges{ { 10U, 5U } }));
    Index::Entry invalid = entry;
    invalid.directional_spans[0].instance_count = 2U;
    EXPECT_THROW(index.upsert(invalid), std::invalid_argument);
    invalid = entry;
    invalid.directional_spans[0].minimum_origin = 3;
    EXPECT_THROW(index.upsert(invalid), std::invalid_argument);
    static_cast<void>(index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, ranges));
    EXPECT_EQ(pack(ranges), (PackedRanges{ { 11U, 4U } }));
    index.remove(entry.coordinate);
    static_cast<void>(index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, ranges));
    EXPECT_TRUE(ranges.empty());
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
    for (Index::Entry const& entry : entries) {
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
    for (Index::Entry const& entry : entries) {
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
    for (Index::Entry const& entry : entries) {
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
                    auto const projection = camera.projectionMatrix(WIDTH, HEIGHT);
                    ASSERT_TRUE(projection.has_value());
                    client::VulkanFrustum const frustum{ *projection * camera.viewMatrix() };
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
        auto const projection = camera.projectionMatrix(WIDTH, HEIGHT);
        ASSERT_TRUE(projection.has_value());
        client::VulkanFrustum const frustum{ *projection * camera.viewMatrix() };
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
