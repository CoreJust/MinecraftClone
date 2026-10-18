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
#include <random>
#include <ranges>
#include <span>
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
    std::vector<Index::Entry const*> ordered_entries;
    ordered_entries.reserve(entries.size());
    for (Index::Entry const& entry : entries) {
        ordered_entries.push_back(&entry);
    }
    std::ranges::sort(ordered_entries, {}, [](Index::Entry const* const entry) {
        return entry->range.first_instance;
    });
    for (Index::Entry const* const entry_pointer : ordered_entries) {
        Index::Entry const& entry = *entry_pointer;
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
            Index::appendVisibleRanges(entry.range, entry.directional_spans, camera_position, result.ranges);
        }
    }
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
        first, {
            .negative_x = std::nullopt,
            .positive_x = first.heights,
            .negative_y = std::nullopt,
            .positive_y = std::nullopt,
        }, shared::HeightTileSurfaceDetail::Coarse
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

TEST(HeightTileDrawIndexTest, ReusesVisibilityOnlyForAnUnchangedCameraAndFrustum)
{
    Index index;
    index.upsert(insideEntry({ .x = 0, .y = 0 }, { 10U, 3U }));
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    glm::dvec3 const camera_position{ 0.25, 0.25, 0.5 };
    std::vector<Range> actual;

    Index::Work const first_work = index.collect(frustum, camera_position, actual);
    EXPECT_GT(first_work.group_plane_tests, 0U);
    EXPECT_EQ(pack(actual), (PackedRanges{ { 10U, 3U } }));

    Index::Work const repeated_work = index.collect(frustum, camera_position, actual);
    EXPECT_EQ(repeated_work.group_plane_tests, 0U);
    EXPECT_EQ(repeated_work.tile_plane_tests, 0U);
    EXPECT_EQ(pack(actual), (PackedRanges{ { 10U, 3U } }));

    glm::dvec3 const moved_camera_position{ 0.5, 0.25, 0.5 };
    Index::Work const moved_camera_work = index.collect(frustum, moved_camera_position, actual);
    EXPECT_GT(moved_camera_work.group_plane_tests, 0U);
    EXPECT_EQ(pack(actual), pack(reference(
        std::vector<Index::Entry>{ insideEntry({ .x = 0, .y = 0 }, { 10U, 3U }) },
        frustum,
        moved_camera_position
    ).ranges));

    client::VulkanFrustum const changed_frustum{
        glm::scale(glm::mat4{ 1.0F }, { 0.5F, 1.0F, 1.0F })
    };
    Index::Work const changed_frustum_work = index.collect(changed_frustum, moved_camera_position, actual);
    EXPECT_GT(changed_frustum_work.group_plane_tests, 0U);
    EXPECT_EQ(pack(actual), pack(reference(
        std::vector<Index::Entry>{ insideEntry({ .x = 0, .y = 0 }, { 10U, 3U }) },
        changed_frustum,
        moved_camera_position
    ).ranges));
}

TEST(HeightTileDrawIndexTest, SuccessfulMutationsInvalidateVisibilityButRejectedUpdatesDoNot)
{
    Index index;
    Index::Entry entry = insideEntry({ .x = 0, .y = 0 }, { 10U, 3U });
    index.upsert(entry);
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    glm::dvec3 const camera_position{};
    std::vector<Range> actual;
    static_cast<void>(index.collect(frustum, camera_position, actual));

    Index::Entry invalid = entry;
    invalid.range.instance_count = 0U;
    EXPECT_THROW(index.upsert(invalid), std::invalid_argument);
    Index::Work const after_rejection = index.collect(frustum, camera_position, actual);
    EXPECT_EQ(after_rejection.group_plane_tests, 0U);

    entry.minimum = { 4.0F, 0.0F, 0.0F };
    entry.maximum = { 5.0F, 1.0F, 1.0F };
    index.upsert(entry);
    Index::Work const after_replacement = index.collect(frustum, camera_position, actual);
    EXPECT_GT(after_replacement.group_plane_tests, 0U);
    EXPECT_TRUE(actual.empty());

    index.remove(entry.coordinate);
    static_cast<void>(index.collect(frustum, camera_position, actual));
    EXPECT_TRUE(actual.empty());
    index.clear();
    Index::Work const after_clear = index.collect(frustum, camera_position, actual);
    EXPECT_EQ(after_clear.groups, 0U);
    EXPECT_EQ(after_clear.records, 0U);
    EXPECT_TRUE(actual.empty());
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

TEST(HeightTileDrawIndexTest, AcceptedOverlappingRangesRetainOwningTileAndDirectionOrder)
{
    std::vector<Index::Entry> entries{
        insideEntry({ .x = 0, .y = 0 }, { 0U, 6U }),
        insideEntry({ .x = 16, .y = 0 }, { 3U, 1U }),
    };
    entries[0].directional_spans[0] = { .instance_count = 2U, .minimum_origin = 2, .maximum_origin = 2 };
    entries[0].directional_spans[1] = { .instance_count = 2U, .minimum_origin = 2, .maximum_origin = 2 };
    entries[0].directional_spans[4] = { .instance_count = 2U, .minimum_origin = 2, .maximum_origin = 2 };
    Index index;
    index.upsert(entries[1]);
    index.upsert(entries[0]);
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    std::vector<Range> actual;
    static_cast<void>(index.collect(frustum, {}, actual));
    EXPECT_EQ(pack(actual), (PackedRanges{ { 0U, 2U }, { 4U, 2U }, { 3U, 1U } }));
    EXPECT_EQ(pack(actual), pack(reference(entries, frustum, {}).ranges));
}

TEST(HeightTileDrawIndexTest, OccupiedStartRollbackDoesNotPublishANewFallbackGroup)
{
    Index index;
    index.upsert(insideEntry({ .x = 0, .y = 0 }, { 0U, 3U }));
    try {
        index.upsert(insideEntry({ .x = -17, .y = -17 }, { 0U, 5U }));
        FAIL() << "occupied range was published in a new group";
    } catch (std::invalid_argument const& error) {
        EXPECT_STREQ(error.what(), "height tile draw index range is already occupied");
    }
    std::vector<Range> actual;
    Index::Work const work = index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, actual);
    EXPECT_EQ(work.groups, 1U);
    EXPECT_EQ(work.records, 1U);
    EXPECT_FALSE(index.hasFallbackRecords());
    EXPECT_EQ(pack(actual), (PackedRanges{ { 0U, 3U } }));
}

TEST(HeightTileDrawIndexTest, DirectionOrderMatchesOracleAcrossTheEntireArenaAddressSpace)
{
    static constexpr uint32_t MAXIMUM_START = std::numeric_limits<uint32_t>::max() - 6U;
    static constexpr std::array<uint32_t, 11> STARTS{
        0U, 31U, 32U, 8'191U, 8'192U, 2'097'151U, 2'097'152U,
        536'870'911U, 536'870'912U, 2'147'483'648U, MAXIMUM_START,
    };
    std::vector<Index::Entry> entries;
    for (uint32_t ordinal = 0U; ordinal < STARTS.size(); ++ordinal) {
        Index::Entry entry = insideEntry({ .x = static_cast<int32_t>(ordinal * 16U), .y = 0 },
            { STARTS[ordinal], 6U });
        for (Index::DirectionalSpan& span : entry.directional_spans) {
            span = { .instance_count = 1U, .minimum_origin = 2, .maximum_origin = 2 };
        }
        entries.push_back(entry);
    }
    Index index;
    for (Index::Entry const& entry : entries | std::views::reverse) {
        index.upsert(entry);
    }
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    std::vector<Range> actual;
    static_cast<void>(index.collect(frustum, {}, actual));
    EXPECT_EQ(pack(actual), pack(reference(entries, frustum, {}).ranges));
    ASSERT_FALSE(actual.empty());
    EXPECT_EQ(actual.back().first_instance, MAXIMUM_START + 4U);
    EXPECT_EQ(actual.back().instance_count, 1U);
}

TEST(HeightTileDrawIndexTest, FragmentedSparseAndFullGroupsMatchScalarOracleThroughoutChurn)
{
    static constexpr uint32_t RECORD_COUNT = 256U;
    static constexpr uint32_t UPDATE_COUNT = 512U;
    static constexpr uint32_t MAXIMUM_UPDATE_SLOTS = 256U;
    static constexpr uint32_t RANGE_STRIDE = 16U;
    static constexpr uint32_t WIDTH = 1'600U;
    static constexpr uint32_t HEIGHT = 900U;
    static constexpr uint32_t RANDOM_SEED = 4'521U;
    static constexpr int32_t WORLD_TILE_SIDE = 4'096;
    static constexpr std::array<glm::dvec3, 3> POSITIONS{
        glm::dvec3{ 32'768.3125, 32'768.3125, 96.0 },
        glm::dvec3{ 65'535.3125, 65'535.3125, 96.0 },
        glm::dvec3{ 32'776.3125, 32'776.3125, 96.0 },
    };
    for (bool const sparse : { false, true }) {
        SCOPED_TRACE(sparse);
        auto const make_entry = [sparse](uint32_t const ordinal, uint32_t const revision) {
            int32_t const stride = sparse ? 17 : 1;
            int32_t const origin = sparse ? 4'080 : 2'048;
            int32_t const tile_x = (origin + static_cast<int32_t>(ordinal % 16U) * stride) % WORLD_TILE_SIDE;
            int32_t const tile_y = (origin + static_cast<int32_t>(ordinal / 16U) * stride) % WORLD_TILE_SIDE;
            int32_t const x = tile_x * shared::HeightTile::SIDE_LENGTH;
            int32_t const y = tile_y * shared::HeightTile::SIDE_LENGTH;
            int32_t const z = static_cast<int32_t>((ordinal + revision * 7U) % 64U);
            Index::Entry entry{
                .coordinate = { .x = tile_x, .y = tile_y },
                .range = { (revision * RECORD_COUNT + ordinal) * RANGE_STRIDE, 12U },
                .minimum = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) },
                .maximum = { static_cast<float>(x + 16), static_cast<float>(y + 16), static_cast<float>(z + 16) },
            };
            for (uint32_t direction = 0U; direction < entry.directional_spans.size(); ++direction) {
                int32_t const plane_origin = direction < 2U ? x : direction < 4U ? y : z;
                entry.directional_spans[direction] = {
                    .instance_count = 2U,
                    .minimum_origin = plane_origin,
                    .maximum_origin = plane_origin + 15,
                };
            }
            return entry;
        };
        Index index;
        std::vector<std::optional<Index::Entry>> slots(RECORD_COUNT);
        std::vector<uint32_t> insertion_order;
        insertion_order.reserve(RECORD_COUNT);
        for (uint32_t ordinal = 0U; ordinal < RECORD_COUNT; ++ordinal) {
            insertion_order.push_back(ordinal);
        }
        std::seed_seq seed{ RANDOM_SEED };
        std::minstd_rand random{ seed };
        std::shuffle(insertion_order.begin(), insertion_order.end(), random);
        for (uint32_t const ordinal : insertion_order) {
            Index::Entry const entry = make_entry(ordinal, 0U);
            slots[ordinal] = entry;
            index.upsert(entry);
        }
        std::vector<Range> actual;
        auto const verify = [&] {
            std::vector<Index::Entry> live_entries;
            for (std::optional<Index::Entry> const& slot : slots) {
                if (slot.has_value()) {
                    live_entries.push_back(*slot);
                }
            }
            for (glm::dvec3 const position : POSITIONS) {
                client::Camera const camera{ { .position = position,
                    .angles = { .yaw_degrees = 37.0, .pitch_degrees = -17.0 } },
                    { .vertical_fov_degrees = 100.0, .far_plane = 65'536.0 } };
                auto const projection = camera.projectionMatrix(WIDTH, HEIGHT);
                ASSERT_TRUE(projection.has_value());
                client::VulkanFrustum const frustum{ *projection * camera.viewMatrix() };
                Index::Work const work = index.collect(frustum, position, actual);
                EXPECT_EQ(work.records, live_entries.size());
                EXPECT_EQ(pack(actual), pack(reference(live_entries, frustum, position).ranges));
                for (uint32_t ordinal = 1U; ordinal < actual.size(); ++ordinal) {
                    EXPECT_LT(actual[ordinal - 1U].first_instance + actual[ordinal - 1U].instance_count,
                        actual[ordinal].first_instance);
                }
            }
            EXPECT_LE(index.lastUpdateSlotsInspected(), MAXIMUM_UPDATE_SLOTS);
        };
        Index::Work const initial_work = index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, actual);
        // The 16x16 dense fixture spans four 8x8 index groups.
        EXPECT_EQ(initial_work.groups, sparse ? RECORD_COUNT : 4U);
        verify();
        for (uint32_t update = 0U; update < UPDATE_COUNT; ++update) {
            uint32_t const ordinal = random() % RECORD_COUNT;
            Index::Entry entry = make_entry(ordinal, update + 1U);
            if (update % 4U == 0U) {
                index.remove(entry.coordinate);
                slots[ordinal].reset();
            } else {
                auto const& slot = slots[ordinal];
                if (update % 4U == 2U && slot.has_value()) {
                    entry.range = slot->range;
                }
                if (update % 4U == 3U) {
                    index.remove(entry.coordinate);
                    entry = make_entry(ordinal, 0U);
                }
                index.upsert(entry);
                slots[ordinal] = entry;
            }
            if (update % 16U == 0U) {
                verify();
            }
        }
        verify();
        for (uint32_t const ordinal : insertion_order) {
            index.remove(make_entry(ordinal, 0U).coordinate);
            slots[ordinal].reset();
        }
        verify();
        EXPECT_EQ(index.collect(client::VulkanFrustum{ glm::mat4{ 1.0F } }, {}, actual).groups, 0U);
        for (uint32_t const ordinal : insertion_order) {
            Index::Entry const entry = make_entry(ordinal, 0U);
            slots[ordinal] = entry;
            index.upsert(entry);
        }
        verify();
        index.clear();
        for (std::optional<Index::Entry>& slot : slots) {
            slot.reset();
        }
        verify();
    }
}

TEST(HeightTileDrawIndexTest, CertifiedGroupFacingMasksMatchScalarOracleAcrossWrapsAndChurn)
{
    static constexpr uint32_t RECORDS_PER_GROUP = 32U;
    static constexpr uint32_t GROUP_COUNT = 3U;
    static constexpr uint32_t RECORD_COUNT = RECORDS_PER_GROUP * GROUP_COUNT;
    static constexpr uint32_t UPDATE_COUNT = 192U;
    static constexpr uint32_t RANDOM_SEED = 4'580U;
    static constexpr uint32_t CAMERA_SAMPLE_COUNT = 18U;
    static constexpr std::array<glm::dvec3, 11> EDGE_CAMERAS{
        glm::dvec3{ 0.0, 0.0, 0.5 },
        glm::dvec3{ 0.25, 1.25, 0.5 },
        glm::dvec3{ -2.75, -1.25, 0.5 },
        glm::dvec3{ 65'536.25, 65'537.25, 0.5 },
        glm::dvec3{ 32'768.5, 32'768.5, 0.5 },
        glm::dvec3{ 32'768.0, 32'768.0, 0.0 },
        glm::dvec3{ 65'536.0, 65'536.0, 0.0 },
        glm::dvec3{ 65'536.5, 65'536.5, 1.0 },
        glm::dvec3{ 33'024.999'999, 33'024.000'001, 0.5 },
        glm::dvec3{ 32'768.000'000'1, 32'768.999'999'9, 0.5 },
        glm::dvec3{ 33'524.5, 32'768.5, 0.5 },
    };
    static constexpr std::array<glm::dvec3, 4> CAMERA_ANCHORS{
        glm::dvec3{ 0.0, 0.0, 0.5 },
        glm::dvec3{ 32'768.0, 32'768.0, 0.5 },
        glm::dvec3{ 65'536.0, 65'536.0, 0.5 },
        glm::dvec3{ 33'524.0, 32'768.0, 0.5 },
    };
    static constexpr double CAMERA_JITTER_SCALE = 0.001;
    static constexpr float HORIZONTAL_SCALE = 1.0F / 200'000.0F;

    struct EntryFixture final {
        uint32_t slot;
        uint32_t revision;
        bool mismatched_bounds;
    };
    auto const make_entry = [](EntryFixture const fixture) {
        uint32_t const group = fixture.slot / RECORDS_PER_GROUP;
        uint32_t const ordinal = fixture.slot % RECORDS_PER_GROUP;
        int32_t const tile_x = group == 0U ? 2'048 + static_cast<int32_t>(ordinal % 16U)
            : group == 1U ? 2'064 + static_cast<int32_t>(ordinal % 16U)
            : static_cast<int32_t>(ordinal % 16U);
        int32_t const tile_y = group == 2U ? static_cast<int32_t>(ordinal / 16U)
            : 2'048 + static_cast<int32_t>(ordinal / 16U);
        int32_t const x = tile_x * shared::HeightTile::SIDE_LENGTH;
        int32_t const y = tile_y * shared::HeightTile::SIDE_LENGTH;
        Index::Entry entry{
            .coordinate = { .x = tile_x, .y = tile_y },
            .range = {},
            .minimum = { static_cast<float>(x), static_cast<float>(y), 0.0F },
            .maximum = { static_cast<float>(x + 16), static_cast<float>(y + 16), 1.0F },
        };
        uint32_t instance_count = 0U;
        for (uint32_t direction = 0U; direction < entry.directional_spans.size(); ++direction) {
            int32_t const axis = static_cast<int32_t>(direction / 2U);
            int32_t const plane_origin = axis == 0 ? x : axis == 1 ? y : 0;
            int32_t const extent = axis == 2 ? 1 : 16;
            int32_t const minimum_offset = static_cast<int32_t>(
                (ordinal * 3U + fixture.revision + direction) % static_cast<uint32_t>(extent + 1)
            );
            int32_t minimum_origin = plane_origin + minimum_offset;
            int32_t maximum_origin = minimum_origin + static_cast<int32_t>(
                (ordinal + fixture.revision + direction) % static_cast<uint32_t>(extent - minimum_offset + 1)
            );
            if (group == 1U && ordinal == 0U && fixture.mismatched_bounds && direction == 1U) {
                minimum_origin = x + 1'000;
                maximum_origin = minimum_origin;
            }
            uint32_t const direction_count = 1U + (ordinal + fixture.revision + direction) % 2U;
            entry.directional_spans[direction] = {
                .instance_count = direction_count,
                .minimum_origin = minimum_origin,
                .maximum_origin = maximum_origin,
            };
            instance_count += direction_count;
        }
        entry.range = {
            .first_instance = group * 20'000U + (fixture.revision % 2U) * 1'000U + ordinal * 7U,
            .instance_count = instance_count,
        };
        return entry;
    };

    Index index;
    std::vector<std::optional<Index::Entry>> slots(RECORD_COUNT);
    std::vector<uint32_t> insertion_order;
    insertion_order.reserve(RECORD_COUNT);
    for (uint32_t slot = 0U; slot < RECORD_COUNT; ++slot) {
        insertion_order.push_back(slot);
    }
    std::seed_seq update_seed{ RANDOM_SEED };
    std::minstd_rand update_random{ update_seed };
    std::shuffle(insertion_order.begin(), insertion_order.end(), update_random);
    for (uint32_t const slot : insertion_order) {
        Index::Entry const entry = make_entry({ .slot = slot, .revision = 0U,
            .mismatched_bounds = slot == RECORDS_PER_GROUP });
        slots[slot] = entry;
        index.upsert(entry);
    }

    client::VulkanFrustum const frustum{
        glm::translate(glm::mat4{ 1.0F }, { 0.0F, 0.0F, 0.5F })
            * glm::scale(glm::mat4{ 1.0F }, { HORIZONTAL_SCALE, HORIZONTAL_SCALE, 0.1F })
    };
    std::vector<Range> actual;
    auto const verify = [&] {
        std::vector<Index::Entry> live_entries;
        live_entries.reserve(RECORD_COUNT);
        for (std::optional<Index::Entry> const& slot : slots) {
            if (slot.has_value()) {
                live_entries.push_back(*slot);
            }
        }
        std::seed_seq camera_seed{ RANDOM_SEED, static_cast<uint32_t>(live_entries.size()) };
        std::minstd_rand camera_random{ camera_seed };
        for (uint32_t sample = 0U; sample < CAMERA_SAMPLE_COUNT; ++sample) {
            glm::dvec3 position{};
            if (sample < EDGE_CAMERAS.size()) {
                position = EDGE_CAMERAS[sample];
            } else {
                glm::dvec3 const anchor = CAMERA_ANCHORS[camera_random() % CAMERA_ANCHORS.size()];
                int32_t const jitter_x = static_cast<int32_t>(camera_random() % 2'001U) - 1'000;
                int32_t const jitter_y = static_cast<int32_t>(camera_random() % 2'001U) - 1'000;
                position = { anchor.x + jitter_x * CAMERA_JITTER_SCALE,
                    anchor.y + jitter_y * CAMERA_JITTER_SCALE, anchor.z };
            }
            Index::Work const work = index.collect(frustum, position, actual);
            EXPECT_EQ(work.records, live_entries.size());
            EXPECT_EQ(pack(actual), pack(reference(live_entries, frustum, position).ranges))
                << "camera sample " << sample;
        }
    };
    uint32_t const mismatched_slot = RECORDS_PER_GROUP;
    auto const verify_mismatched_span_falls_back_to_exact_facing = [&] {
        ASSERT_TRUE(slots[mismatched_slot].has_value());
        Index::Entry const& entry = *slots[mismatched_slot];
        glm::dvec3 const position = EDGE_CAMERAS[10U];
        static_cast<void>(index.collect(frustum, position, actual));
        auto const contains_instance = [](std::vector<Range> const& ranges, uint32_t const instance) {
            return std::ranges::any_of(ranges, [instance](Range const range) {
                return instance >= range.first_instance && instance - range.first_instance < range.instance_count;
            });
        };
        uint32_t const positive_x_first_instance = entry.range.first_instance
            + entry.directional_spans[0].instance_count;
        uint32_t const negative_y_first_instance = positive_x_first_instance
            + entry.directional_spans[1].instance_count;
        EXPECT_TRUE(contains_instance(actual, negative_y_first_instance));
        EXPECT_FALSE(contains_instance(actual, positive_x_first_instance));
    };
    verify();
    verify_mismatched_span_falls_back_to_exact_facing();
    for (uint32_t const revision : { 2U, 4U }) {
        bool const mismatched_bounds = revision == 4U;
        Index::Entry const entry = make_entry({ .slot = mismatched_slot, .revision = revision,
            .mismatched_bounds = mismatched_bounds });
        index.upsert(entry);
        slots[mismatched_slot] = entry;
        verify();
        if (mismatched_bounds) {
            verify_mismatched_span_falls_back_to_exact_facing();
        }
    }
    for (uint32_t update = 0U; update < UPDATE_COUNT; ++update) {
        uint32_t const slot = update_random() % RECORD_COUNT;
        if (update % 5U == 0U) {
            index.remove(make_entry({ .slot = slot, .revision = 0U, .mismatched_bounds = false }).coordinate);
            slots[slot].reset();
        } else {
            bool const mismatched_bounds = slot == mismatched_slot && update % 2U == 0U;
            Index::Entry const entry = make_entry({ .slot = slot, .revision = update + 1U,
                .mismatched_bounds = mismatched_bounds });
            index.upsert(entry);
            slots[slot] = entry;
        }
        if (update % 12U == 0U) {
            verify();
        }
    }
    verify();
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

TEST(HeightTileDrawIndexTest, BatchRemovalMatchesRepeatedSingleRemovalAcrossGroups)
{
    std::vector<Index::Entry> const entries{
        insideEntry({ .x = 2, .y = 3 }, { 100U, 2U }),
        insideEntry({ .x = 3, .y = 3 }, { 10U, 4U }),
        insideEntry({ .x = 8, .y = 3 }, { 14U, 6U }),
        insideEntry({ .x = 9, .y = 3 }, { 20U, 2U }),
        insideEntry({ .x = -1, .y = 3 }, { 30U, 1U }),
        insideEntry({ .x = 0, .y = -1 }, { 40U, 1U }),
    };
    static constexpr std::array<shared::HeightTileCoordinate, 5> REMOVALS{
        shared::HeightTileCoordinate{ 2, 3 },
        shared::HeightTileCoordinate{ 8, 3 },
        shared::HeightTileCoordinate{ 2, 3 },
        shared::HeightTileCoordinate{ 50, 50 },
        shared::HeightTileCoordinate{ -1, 3 },
    };
    Index batched;
    Index repeated;
    for (Index::Entry const& entry : entries) {
        batched.upsert(entry);
        repeated.upsert(entry);
    }

    batched.removeBatch(REMOVALS);
    for (shared::HeightTileCoordinate const coordinate : REMOVALS) {
        repeated.remove(coordinate);
    }
    EXPECT_TRUE(batched.hasFallbackRecords());
    EXPECT_TRUE(repeated.hasFallbackRecords());

    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    std::vector<Range> batched_ranges;
    std::vector<Range> repeated_ranges;
    Index::Work const batched_work = batched.collect(frustum, {}, batched_ranges);
    Index::Work const repeated_work = repeated.collect(frustum, {}, repeated_ranges);
    EXPECT_EQ(pack(batched_ranges), pack(repeated_ranges));
    EXPECT_EQ(batched_work.groups, repeated_work.groups);
    EXPECT_EQ(batched_work.records, repeated_work.records);

    batched.upsert(entries[0]);
    repeated.upsert(entries[0]);
    batched.upsert(entries[2]);
    repeated.upsert(entries[2]);
    Index::Work const after_range_reuse = batched.collect(frustum, {}, batched_ranges);
    Index::Work const repeated_after_range_reuse = repeated.collect(frustum, {}, repeated_ranges);
    EXPECT_EQ(after_range_reuse.records, repeated_after_range_reuse.records);
    EXPECT_EQ(pack(batched_ranges), pack(repeated_ranges));

    static constexpr std::array<shared::HeightTileCoordinate, 1> LAST_FALLBACK{
        shared::HeightTileCoordinate{ 0, -1 },
    };
    batched.removeBatch(LAST_FALLBACK);
    repeated.remove(LAST_FALLBACK.front());
    EXPECT_FALSE(batched.hasFallbackRecords());
    EXPECT_FALSE(repeated.hasFallbackRecords());
}

TEST(HeightTileDrawIndexTest, ReplacementReleasesTheOldRangeStartImmediately)
{
    Index index;
    Index::Entry const original = insideEntry({ .x = 0, .y = 0 }, { 100U, 2U });
    Index::Entry replacement = original;
    replacement.range = { 200U, 2U };
    index.upsert(original);
    index.upsert(replacement);

    Index::Entry const reused_range = insideEntry({ .x = 8, .y = 0 }, original.range);
    index.upsert(reused_range);

    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    std::vector<Range> actual;
    Index::Work const work = index.collect(frustum, {}, actual);
    EXPECT_EQ(work.records, 2U);
    EXPECT_EQ(pack(actual), pack(reference({ replacement, reused_range }, frustum, {}).ranges));
}

TEST(HeightTileDrawIndexTest, DestructionDrainsRetiredStorageAfterLogicalRemoval)
{
    static constexpr uint32_t ENTRY_COUNT = 64U;
    static constexpr int32_t GROUP_SIDE = 8;
    Index::Work work;
    std::vector<Range> actual;
    client::VulkanFrustum const frustum{ glm::mat4{ 1.0F } };
    {
        Index index;
        std::vector<Index::Entry> entries;
        std::vector<shared::HeightTileCoordinate> coordinates;
        entries.reserve(ENTRY_COUNT);
        coordinates.reserve(ENTRY_COUNT);
        for (uint32_t ordinal = 0U; ordinal < ENTRY_COUNT; ++ordinal) {
            shared::HeightTileCoordinate const coordinate{
                .x = static_cast<int32_t>(ordinal % static_cast<uint32_t>(GROUP_SIDE)),
                .y = static_cast<int32_t>(ordinal / static_cast<uint32_t>(GROUP_SIDE)),
            };
            entries.push_back(insideEntry(coordinate, { ordinal * 2U, 1U }));
            coordinates.push_back(coordinate);
            index.upsert(entries.back());
        }

        index.removeBatch(coordinates);
        work = index.collect(frustum, {}, actual);
        EXPECT_EQ(work.groups, 0U);
        EXPECT_EQ(work.records, 0U);
        EXPECT_TRUE(actual.empty());
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
