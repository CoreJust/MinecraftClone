#include <client/RiverBedMesh.hpp>
#include <client/TerrainLodPresentation.hpp>
#include <client/detail/RiverFanQuantization.hpp>
#include <client/render/InstalledShaderAssets.hpp>
#include <client/render/VulkanRenderer.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace client::detail {

bool nodeRiverConstraintCrossings(std::span<std::array<double, 2> const> points,
    std::span<std::array<uint32_t, 2> const> edges, std::vector<std::vector<uint32_t>>& paths);
shared::Block selectRiverSectorMaterial(std::array<double, 3> weights, std::array<shared::Block, 3> materials);
void resampleRiverRenderHeights(std::span<std::array<double, 3> const> original,
    std::span<std::array<double, 3>> positions, std::function<double(double, double)> const& height,
    TerrainLodRiverGeometryAudit& audit);

} // namespace client::detail

namespace {

using Point = std::array<double, 2>;
using Edge = std::array<uint32_t, 2>;

double bed(double x, double y)
{
    return x * x + y * y;
}

} // namespace

TEST(RiverBedMesh, CapturedOffGridCrossingRoutesThroughNearbyIncidentEndpoint)
{
    // Reduced from the reset fan at crop (33040, 32784), retaining both
    // crossing constraints and their actual Q5 coordinates.
    std::array<Point, 4> const points{{{33'040, 32'791}, {33'043.0625, 32'791.375},
        {33'043.03125, 32'791.375}, {33'042.09375, 32'791.125}}};
    std::array<Edge, 2> const edges{{{0, 1}, {2, 3}}};
    std::vector<std::vector<uint32_t>> paths;
    ASSERT_TRUE(client::detail::nodeRiverConstraintCrossings(points, edges, paths));
    ASSERT_EQ(paths.size(), 2U);
    EXPECT_EQ(paths[0], (std::vector<uint32_t>{0, 2, 1}));
    EXPECT_EQ(paths[1], (std::vector<uint32_t>{2, 3}));
}

TEST(RiverBedMesh, OffGridCrossingRejectsDistantIncidentEndpoints)
{
    std::array<Point, 4> const points{{{0, 0}, {10.0 / 32.0, 10.0 / 32.0},
        {0, 9.0 / 32.0}, {10.0 / 32.0, 0}}};
    std::array<Edge, 2> const edges{{{0, 1}, {2, 3}}};
    std::vector<std::vector<uint32_t>> paths;
    EXPECT_FALSE(client::detail::nodeRiverConstraintCrossings(points, edges, paths));
}

TEST(RiverBedMesh, CrossingConstraintsSampleNewVerticesAndCoverCrop)
{
    std::vector<Point> points;
    for (double y = 0; y <= 64; y += 32) {
        for (double x = 0; x <= 64; x += 32) {
            points.push_back({x, y});
        }
    }
    points.insert(points.end(), {{5, 13}, {59, 13}, {19, 3}, {19, 61}, {5, 13}});
    std::vector<Edge> const constraints{{9, 10}, {11, 12}, {10, 9}, {13, 9}};
    auto const mesh = client::buildRiverBedMesh({0, 0}, {64, 64}, points, constraints, bed);
    auto const repeated = client::buildRiverBedMesh({0, 0}, {64, 64}, points, constraints, bed);
    EXPECT_EQ(mesh.vertices, repeated.vertices);
    EXPECT_EQ(mesh.triangles, repeated.triangles);
    std::set<std::pair<uint32_t, uint32_t>> mesh_edges;
    double total_area = 0;
    for (auto const& triangle : mesh.triangles) {
        auto const& a = mesh.vertices.at(triangle[0]);
        auto const& b = mesh.vertices.at(triangle[1]);
        auto const& c = mesh.vertices.at(triangle[2]);
        double const twice_area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        EXPECT_GT(twice_area, 0);
        total_area += twice_area / 2;
        for (size_t i = 0; i < 3; ++i) {
            uint32_t const x = triangle[i];
            uint32_t const y = triangle[(i + 1) % 3];
            mesh_edges.emplace(std::min(x, y), std::max(x, y));
        }
    }
    EXPECT_DOUBLE_EQ(total_area, 64 * 64);
    bool found_crossing = false;
    for (auto const& vertex : mesh.vertices) {
        EXPECT_GE(vertex[0], 0);
        EXPECT_LE(vertex[0], 64);
        EXPECT_GE(vertex[1], 0);
        EXPECT_LE(vertex[1], 64);
        EXPECT_DOUBLE_EQ(vertex[2], bed(vertex[0], vertex[1]));
        if (vertex[0] == 19 && vertex[1] == 13) {
            found_crossing = true;
            EXPECT_NE(vertex[2], (bed(5, 13) * 40 + bed(59, 13) * 14) / 54);
        }
    }
    EXPECT_TRUE(found_crossing);
    // Every edge of each axis-aligned constraint is represented by a connected chain.
    for (auto const& segment : std::array<std::array<double, 4>, 2>{{{5, 13, 59, 13}, {19, 3, 19, 61}}}) {
        std::vector<std::pair<double, uint32_t>> along;
        bool const horizontal = segment[1] == segment[3];
        for (uint32_t i = 0; i < mesh.vertices.size(); ++i) {
            auto const& v = mesh.vertices[i];
            if ((horizontal && v[1] == segment[1] && v[0] >= segment[0] && v[0] <= segment[2])
                || (!horizontal && v[0] == segment[0] && v[1] >= segment[1] && v[1] <= segment[3])) {
                along.emplace_back(horizontal ? v[0] : v[1], i);
            }
        }
        std::sort(along.begin(), along.end());
        ASSERT_GE(along.size(), 2U);
        EXPECT_DOUBLE_EQ(along.front().first, horizontal ? segment[0] : segment[1]);
        EXPECT_DOUBLE_EQ(along.back().first, horizontal ? segment[2] : segment[3]);
        for (size_t i = 1; i < along.size(); ++i) {
            EXPECT_TRUE(mesh_edges.contains(std::minmax(along[i - 1].second, along[i].second)));
        }
    }
}

TEST(RiverBedMesh, RejectsInvalidInputsAndPropagatesCallbackErrors)
{
    std::vector<Point> const nan_points{{std::numeric_limits<double>::quiet_NaN(), 1}};
    std::vector<Point> const outside{{65, 1}};
    std::vector<Point> const points{{1, 1}};
    std::vector<Edge> const invalid_edges{{0, 1}};
    EXPECT_THROW(client::buildRiverBedMesh({0, 0}, {64, 64}, nan_points, {}, bed), std::invalid_argument);
    EXPECT_THROW(client::buildRiverBedMesh({0, 0}, {64, 64}, outside, {}, bed), std::invalid_argument);
    EXPECT_THROW(client::buildRiverBedMesh({0, 0}, {64, 64}, points, invalid_edges, bed), std::invalid_argument);
    EXPECT_THROW(client::buildRiverBedMesh({0, 0}, {0, 64}, {}, {}, bed), std::invalid_argument);
    EXPECT_THROW(client::buildRiverBedMesh({0, 0}, {64, 64}, {}, {}, [](double, double) {
        return std::numeric_limits<double>::infinity();
    }), std::invalid_argument);
    EXPECT_THROW(client::buildRiverBedMesh({0, 0}, {64, 64}, {}, {}, [](double, double) -> double {
        throw std::runtime_error("height failure");
    }), std::runtime_error);
}

TEST(RiverBedMesh, ObliqueCrossingSamplesIntersectionHeight)
{
    std::vector<Point> const points{{-8, -6}, {8, 6}, {-7, 5}, {7, -5}};
    std::vector<Edge> const constraints{{0, 1}, {2, 3}};
    auto const mesh = client::buildRiverBedMesh({-10, -10}, {10, 10}, points, constraints, bed);
    bool found_crossing = false;
    for (auto const& vertex : mesh.vertices) {
        EXPECT_DOUBLE_EQ(vertex[2], bed(vertex[0], vertex[1]));
        if (vertex[0] == 0 && vertex[1] == 0) {
            found_crossing = true;
            EXPECT_NE(vertex[2], (bed(-8, -6) + bed(8, 6)) / 2);
        }
    }
    ASSERT_TRUE(found_crossing);
    std::set<std::pair<uint32_t, uint32_t>> mesh_edges;
    for (auto const& triangle : mesh.triangles) {
        for (size_t i = 0; i < 3; ++i) {
            mesh_edges.emplace(std::minmax(triangle[i], triangle[(i + 1) % 3]));
        }
    }
    auto index_of = [&](double x, double y) {
        for (uint32_t i = 0; i < mesh.vertices.size(); ++i) {
            if (mesh.vertices[i][0] == x && mesh.vertices[i][1] == y) {
                return i;
            }
        }
        return static_cast<uint32_t>(mesh.vertices.size());
    };
    uint32_t const crossing = index_of(0, 0);
    for (auto const& point : points) {
        uint32_t const endpoint = index_of(point[0], point[1]);
        ASSERT_LT(endpoint, mesh.vertices.size());
        EXPECT_TRUE(mesh_edges.contains(std::minmax(endpoint, crossing)));
    }
}

TEST(RiverBedMesh, OverlappingAndBoundaryAnchoredConstraintsFormEdges)
{
    std::vector<Point> const points{{-10, 2}, {10, 2}, {-4, 2}, {6, 2}, {-10, -3}, {10, 7}};
    std::vector<Edge> const constraints{{0, 1}, {2, 3}, {4, 5}};
    auto const mesh = client::buildRiverBedMesh({-10, -10}, {10, 10}, points, constraints, bed);
    std::set<std::pair<uint32_t, uint32_t>> mesh_edges;
    for (auto const& triangle : mesh.triangles) {
        for (size_t i = 0; i < 3; ++i) {
            mesh_edges.emplace(std::minmax(triangle[i], triangle[(i + 1) % 3]));
        }
    }
    for (auto const& vertex : mesh.vertices) {
        EXPECT_DOUBLE_EQ(vertex[2], bed(vertex[0], vertex[1]));
    }
    for (auto const& edge : constraints) {
        auto const& a = points[edge[0]];
        auto const& b = points[edge[1]];
        std::vector<std::pair<double, uint32_t>> along;
        for (uint32_t i = 0; i < mesh.vertices.size(); ++i) {
            auto const& v = mesh.vertices[i];
            double const cross = (v[0] - a[0]) * (b[1] - a[1]) - (v[1] - a[1]) * (b[0] - a[0]);
            double const dot = (v[0] - a[0]) * (b[0] - a[0]) + (v[1] - a[1]) * (b[1] - a[1]);
            double const length_squared = (b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]);
            if (std::abs(cross) < 1e-9 && dot >= 0 && dot <= length_squared) {
                along.emplace_back(dot, i);
            }
        }
        std::sort(along.begin(), along.end());
        ASSERT_GE(along.size(), 2U);
        EXPECT_DOUBLE_EQ(along.front().first, 0);
        EXPECT_DOUBLE_EQ(along.back().first,
            (b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
        for (size_t i = 1; i < along.size(); ++i) {
            EXPECT_TRUE(mesh_edges.contains(std::minmax(along[i - 1].second, along[i].second)));
        }
    }
}

TEST(RiverBedMesh, ConcurrentRealRiverConstraintsPreserveWorldSamplingAndCropCoverage)
{
    // Captured confluence cell: both rails meet the fan radial at x = 4314773/86.
    std::vector<Point> const original_points{{50'048, 1'920}, {50'176, 2'048},
        {50'171.1875, 2'044.343'75}, {50'171.875, 2'043.6875},
        {50'171.6875, 2'043.875}, {50'172.343'75, 2'043.1875}};
    std::vector<Edge> const constraints{{0, 1}, {2, 3}, {4, 5}};
    for (Point const shift : std::array<Point, 3>{{{0, 0}, {-49'920, -1'792}, {15'360, 63'488}}}) {
        Point const minimum{49'920 + shift[0], 1'792 + shift[1]};
        Point const maximum{50'176 + shift[0], 2'048 + shift[1]};
        auto points = original_points;
        for (auto& point : points) {
            point[0] += shift[0];
            point[1] += shift[1];
        }
        std::vector<Point> callback_points;
        auto const mesh = client::buildRiverBedMesh(minimum, maximum, points, constraints,
            [&](double x, double y) {
                callback_points.push_back({x, y});
                return bed(x, y);
            });
        ASSERT_EQ(callback_points.size(), mesh.vertices.size());
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> mesh_edges;
        long double total_area = 0;
        for (auto const& triangle : mesh.triangles) {
            auto const& a = mesh.vertices.at(triangle[0]);
            auto const& b = mesh.vertices.at(triangle[1]);
            auto const& c = mesh.vertices.at(triangle[2]);
            double const twice_area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            ASSERT_GT(twice_area, 0);
            total_area += static_cast<long double>(twice_area) / 2;
            for (size_t i = 0; i < 3; ++i) {
                ++mesh_edges[std::minmax(triangle[i], triangle[(i + 1) % 3])];
            }
        }
        EXPECT_NEAR(static_cast<double>(total_area), 256 * 256, 1e-8);
        bool found_non_grid_intersection = false;
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            auto const& v = mesh.vertices[i];
            EXPECT_EQ(callback_points[i], (Point{v[0], v[1]}));
            EXPECT_DOUBLE_EQ(v[2], bed(v[0], v[1]));
            EXPECT_GE(v[0], minimum[0]);
            EXPECT_LE(v[0], maximum[0]);
            EXPECT_GE(v[1], minimum[1]);
            EXPECT_LE(v[1], maximum[1]);
            found_non_grid_intersection |= v[0] * 32 != std::round(v[0] * 32);
        }
        EXPECT_TRUE(found_non_grid_intersection);
        for (auto const& [edge, count] : mesh_edges) {
            EXPECT_LE(count, 2U);
            if (count == 1) {
                auto const& a = mesh.vertices[edge.first];
                auto const& b = mesh.vertices[edge.second];
                EXPECT_TRUE((a[0] == minimum[0] && b[0] == minimum[0])
                    || (a[0] == maximum[0] && b[0] == maximum[0])
                    || (a[1] == minimum[1] && b[1] == minimum[1])
                    || (a[1] == maximum[1] && b[1] == maximum[1]));
            }
        }
        for (auto const& constraint : constraints) {
            auto const& a = points[constraint[0]];
            auto const& b = points[constraint[1]];
            double const dx = b[0] - a[0], dy = b[1] - a[1];
            double const length = std::hypot(dx, dy);
            std::vector<bool> reachable(mesh.vertices.size());
            uint32_t end = static_cast<uint32_t>(mesh.vertices.size());
            for (uint32_t i = 0; i < mesh.vertices.size(); ++i) {
                auto const& v = mesh.vertices[i];
                if (v[0] == a[0] && v[1] == a[1]) {
                    reachable[i] = true;
                }
                if (v[0] == b[0] && v[1] == b[1]) {
                    end = i;
                }
            }
            ASSERT_LT(end, mesh.vertices.size());
            auto const on_segment = [&](uint32_t i) {
                auto const& v = mesh.vertices[i];
                double const cross = (v[0] - a[0]) * dy - (v[1] - a[1]) * dx;
                double const along = ((v[0] - a[0]) * dx + (v[1] - a[1]) * dy) / length;
                return std::abs(cross) <= 1e-8 * length && along >= -1e-8 && along <= length + 1e-8;
            };
            bool changed = true;
            while (changed) {
                changed = false;
                for (auto const& [edge, count] : mesh_edges) {
                    if (reachable[edge.first] != reachable[edge.second]
                        && on_segment(edge.first) && on_segment(edge.second)) {
                        reachable[edge.first] = reachable[edge.second] = true;
                        changed = true;
                    }
                }
            }
            EXPECT_TRUE(reachable[end]);
        }
    }
}

TEST(RiverBedMesh, PreservesSmallNonGridAnchorsInSignedCrop)
{
    std::vector<Point> const points{{1e-16, -1e-16}};
    auto const mesh = client::buildRiverBedMesh({-10, -10}, {10, 10}, points, {}, bed);
    bool found_anchor = false;
    for (auto const& vertex : mesh.vertices) {
        EXPECT_DOUBLE_EQ(vertex[2], bed(vertex[0], vertex[1]));
        found_anchor |= vertex[0] == points[0][0] && vertex[1] == points[0][1];
    }
    EXPECT_TRUE(found_anchor);
}

namespace {

struct ActualRiverOwnerCase final {
    Point minimum{}, maximum{};
    client::RiverBedMesh native;
    std::vector<Point> anchors;
    std::vector<Edge> constraints, radials;
    std::vector<std::array<double, 3>> positions, canonical;
    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<size_t> owners;
    std::vector<std::array<int64_t, 2>> seeds;
};

ActualRiverOwnerCase actualRiverOwnerCase()
{
    std::ifstream input(std::string(MC_V10_FIXTURE_DIRECTORY) + "/river_q5_owner_quotient.txt");
    ActualRiverOwnerCase c;
    input >> c.minimum[0] >> c.minimum[1] >> c.maximum[0] >> c.maximum[1];
    auto const read = [&](auto& values) {
        size_t count = 0U;
        input >> count;
        values.resize(count);
        for (auto& row : values) {
            for (auto& item : row) {
                input >> item;
            }
        }
    };
    read(c.native.vertices);
    read(c.native.triangles);
    read(c.anchors);
    read(c.constraints);
    read(c.radials);
    read(c.positions);
    read(c.triangles);
    size_t count = 0U;
    input >> count;
    c.owners.resize(count);
    for (auto& owner : c.owners) {
        input >> owner;
    }
    read(c.seeds);
    read(c.canonical);
    if (!input) {
        throw std::runtime_error("invalid actual river owner fixture");
    }
    return c;
}

std::array<double, 3> actualRiverWeights(ActualRiverOwnerCase const& c, Point const xy, size_t const owner)
{
    auto const edge = c.constraints.at(2U * owner + 1U);
    auto a = c.anchors.at(edge[0]), b = c.anchors.at(edge[1]);
    Point const center{(c.minimum[0] + c.maximum[0]) / 2.0, (c.minimum[1] + c.maximum[1]) / 2.0};
    for (size_t k = 0U; k < 2U; ++k) {
        a[k] -= center[k];
        b[k] -= center[k];
    }
    double const area = a[0] * b[1] - a[1] * b[0];
    double const u = ((xy[0] - center[0]) * b[1] - (xy[1] - center[1]) * b[0]) / area;
    double const v = (a[0] * (xy[1] - center[1]) - a[1] * (xy[0] - center[0])) / area;
    return {1.0 - u - v, u, v};
}

bool repairActualRiverOwnerCase(ActualRiverOwnerCase& c, bool const different_height,
    bool const different_material, std::string& failure,
    std::set<std::tuple<Point, size_t, size_t>>& calls)
{
    std::map<Point, double> canonical;
    for (auto const& p : c.canonical) {
        canonical.emplace(Point{p[0], p[1]}, p[2]);
    }
    auto const material = [&](Point const xy, size_t const owner) {
        std::array<shared::Block, 3> const inputs{
            different_material && owner == 3U ? shared::Block::Stone : shared::Block::Grass,
            owner % 2U == 0U ? shared::Block::Dirt : shared::Block::Stone,
            owner % 2U == 0U ? shared::Block::Stone : shared::Block::Dirt};
        return client::detail::selectRiverSectorMaterial(actualRiverWeights(c, xy, owner), inputs);
    };
    auto const equivalent = [&](Point const xy, size_t const a, size_t const b) {
        calls.emplace(xy, std::min(a, b), std::max(a, b));
        double const base = canonical.at(xy);
        double const za = base + (different_height && a == 3U ? 1.0 / 32.0 : 0.0);
        double const zb = base + (different_height && b == 3U ? 1.0 / 32.0 : 0.0);
        return za == zb && material(xy, a) == material(xy, b);
    };
    client::TerrainLodRiverGeometryAudit audit;
    bool const result = client::detail::repairRiverFanQuantization(c.native, c.anchors, c.constraints, c.radials,
        c.minimum, c.maximum, c.positions, c.triangles, c.owners, c.seeds, equivalent, audit, failure, nullptr);
    if (result) {
        client::detail::resampleRiverRenderHeights(c.native.vertices, c.positions,
            [&](double const x, double const y) { return canonical.at({x, y}); }, audit);
    }
    return result;
}

} // namespace

TEST(RiverBedMesh, ActualCollapsedOwnerFanOffscreenCoverage)
{
    char const* const output_directory = std::getenv("MC_Q5_CAPTURE_DIR");
    if (output_directory == nullptr || output_directory[0] == '\0') {
        GTEST_SKIP() << "Set MC_Q5_CAPTURE_DIR for the offscreen geometry diagnostic";
    }
    auto fixture = actualRiverOwnerCase();
    auto const native_positions = fixture.native.vertices;
    auto const native_triangles = fixture.native.triangles;
    std::set<std::tuple<Point, size_t, size_t>> calls;
    std::string failure;
    ASSERT_TRUE(repairActualRiverOwnerCase(fixture, false, false, failure, calls)) << failure;
    auto const scene = [](std::vector<std::array<double, 3>> const& positions,
                          std::vector<std::array<uint32_t, 3>> const& triangles) {
        std::vector<client::StoneFaceInstance> faces;
        faces.reserve(triangles.size());
        for (auto const& triangle : triangles) {
            std::array<std::array<double, 3>, 3> vertices{};
            for (size_t index = 0U; index < 3U; ++index) {
                vertices[index] = positions.at(triangle[index]);
                for (double const value : vertices[index]) {
                    if (!std::isfinite(value)) throw std::runtime_error("nonfinite captured river vertex");
                }
            }
            // Uniform material isolates coverage; geometry and canonical heights stay unchanged.
            constexpr std::array materials{shared::Block::Grass, shared::Block::Grass, shared::Block::Grass};
            faces.push_back(client::StoneFaceInstance::fromMetricTerrainMaterialTriangle(vertices, materials));
        }
        auto result = std::make_shared<client::TerrainLodScene>();
        result->logical_face_count = faces.size();
        client::TerrainLodDrawGroup group{
            .range = {0U, static_cast<uint32_t>(faces.size())}, .origin_x = 32'528U, .origin_y = 33'008U,
            .extent = 16U,
        };
        group.face_storage = std::make_shared<std::vector<client::StoneFaceInstance> const>(std::move(faces));
        result->groups.push_back(std::move(group));
        return result;
    };
    client::InstalledShaderAssets assets;
    client::VulkanOffscreenRenderer renderer{assets, true};
    ASSERT_TRUE(renderer.validationEnabled());
    renderer.setCamera({.position = {32'536.0, 33'000.0, 680.0}, .angles = {.pitch_degrees = -65.0}});
    auto const capture = [&] {
        return renderer.render({}, std::chrono::steady_clock::now() + std::chrono::seconds{10});
    };
    renderer.setTerrainLodScene(std::make_shared<client::TerrainLodScene>());
    auto const background = capture();
    renderer.setTerrainLodScene(scene(native_positions, native_triangles));
    auto const native = capture();
    renderer.setTerrainLodScene(scene(fixture.positions, fixture.triangles));
    auto const repaired = capture();
    auto const write = [&](char const* name, client::RendererFrameCapture const& frame) {
        std::ofstream output(std::filesystem::path{output_directory} / name, std::ios::binary);
        if (!output) return false;
        output << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
        for (size_t i = 0U; i + 3U < frame.rgba8.size(); i += 4U) {
            output.write(reinterpret_cast<char const*>(frame.rgba8.data() + i), 3);
        }
        return static_cast<bool>(output);
    };
    ASSERT_TRUE(write("background.ppm", background));
    ASSERT_TRUE(write("native.ppm", native));
    ASSERT_TRUE(write("repaired.ppm", repaired));
    ASSERT_EQ(native.width, repaired.width);
    ASSERT_EQ(native.height, repaired.height);
    ASSERT_EQ(native.rgba8.size(), repaired.rgba8.size());
    ASSERT_EQ(native.rgba8.size(), background.rgba8.size());
    ASSERT_GT(native.width, 4U);
    ASSERT_GT(native.height, 4U);
    ASSERT_EQ(native.rgba8.size(), static_cast<size_t>(native.width) * native.height * 4U);
    auto const visible = [&](client::RendererFrameCapture const& frame, uint32_t x, uint32_t y) {
        size_t const offset = (static_cast<size_t>(y) * frame.width + x) * 4U;
        for (size_t channel = 0U; channel < 3U; ++channel) {
            if (std::abs(static_cast<int>(frame.rgba8[offset + channel])
                    - static_cast<int>(background.rgba8[offset + channel])) > 3) return true;
        }
        return false;
    };
    uint64_t interior = 0U;
    uint64_t holes = 0U;
    for (uint32_t y = 2U; y + 2U < native.height; ++y) {
        for (uint32_t x = 2U; x + 2U < native.width; ++x) {
            bool inside = true;
            for (int dy = -2; dy <= 2 && inside; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (!visible(native, static_cast<uint32_t>(static_cast<int>(x) + dx),
                            static_cast<uint32_t>(static_cast<int>(y) + dy))) {
                        inside = false;
                        break;
                    }
                }
            }
            if (inside) {
                ++interior;
                holes += visible(repaired, x, y) ? 0U : 1U;
            }
        }
    }
    RecordProperty("native_interior_pixels", std::to_string(interior));
    RecordProperty("repaired_interior_holes", std::to_string(holes));
    EXPECT_GT(interior, 10'000U);
    EXPECT_EQ(holes, 0U);
    EXPECT_EQ(renderer.validationErrorCount(), 0U);
}

TEST(RiverBedMesh, ActualCollapsedOwnerFanRetainsGeometryAndCanonicalStates)
{
    auto c = actualRiverOwnerCase();
    auto const native_faces = c.native.triangles;
    auto const old_owners = c.owners;
    std::set<std::tuple<Point, size_t, size_t>> calls;
    std::string failure;
    ASSERT_TRUE(repairActualRiverOwnerCase(c, false, false, failure, calls)) << failure;
    ASSERT_EQ(c.triangles.size(), 126U);
    std::map<Edge, std::vector<size_t>> mesh_edges, native_edges;
    std::set<std::array<uint32_t, 3>> unique_faces;
    std::vector<double> actual_area(c.seeds.size()), expected_area(c.seeds.size());
    auto const area = [&](auto const& t) {
        auto const& a = c.positions[t[0]];
        auto const& b = c.positions[t[1]];
        auto const& d = c.positions[t[2]];
        return (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);
    };
    for (size_t i = 0U; i < c.triangles.size(); ++i) {
        auto t = c.triangles[i];
        EXPECT_GT(area(t), 0.0);
        actual_area[c.owners[i]] += area(t);
        std::sort(t.begin(), t.end());
        EXPECT_TRUE(unique_faces.insert(t).second);
        for (size_t k = 0U; k < 3U; ++k) {
            uint32_t const a = c.triangles[i][k], b = c.triangles[i][(k + 1U) % 3U];
            mesh_edges[{std::min(a, b), std::max(a, b)}].push_back(i);
        }
    }
    for (size_t i = 0U; i < native_faces.size(); ++i) {
        expected_area[old_owners[i]] += area(native_faces[i]);
        for (size_t k = 0U; k < 3U; ++k) {
            uint32_t const a = native_faces[i][k], b = native_faces[i][(k + 1U) % 3U];
            native_edges[{std::min(a, b), std::max(a, b)}].push_back(i);
        }
    }
    // The recorded radial 59->13 is explicitly noded through input45.
    double const swept = area(std::array<uint32_t, 3>{59U, 45U, 13U});
    EXPECT_EQ(swept, 0.028'320'312'5);
    expected_area[10] += swept;
    expected_area[9] -= swept;
    EXPECT_EQ(actual_area, expected_area);
    // The center legs collapse locally; none of the 34 semantic owners has
    // zero total area. A seed on a neighboring component must not relabel it.
    for (size_t owner = 0U; owner < c.seeds.size(); ++owner) {
        EXPECT_GT(actual_area[owner], 0.0) << owner;
        size_t containing_faces = 0U;
        for (size_t f = 0U; f < c.triangles.size(); ++f) {
            bool contains = true;
            for (size_t k = 0U; k < 3U; ++k) {
                auto const& a = c.positions[c.triangles[f][k]];
                auto const& b = c.positions[c.triangles[f][(k + 1U) % 3U]];
                int64_t const ax = 3 * std::llround(a[0] * 32.0), ay = 3 * std::llround(a[1] * 32.0);
                int64_t const bx = 3 * std::llround(b[0] * 32.0), by = 3 * std::llround(b[1] * 32.0);
                contains = contains && (bx - ax) * (c.seeds[owner][1] - ay)
                    - (by - ay) * (c.seeds[owner][0] - ax) >= 0;
            }
            if (contains) {
                ++containing_faces;
                EXPECT_EQ(c.owners[f], owner);
            }
        }
        EXPECT_GT(containing_faces, 0U) << owner;
    }
    double total_area = 0.0;
    for (double const a : actual_area) {
        total_area += a;
    }
    EXPECT_EQ(total_area, 512.0);
    std::map<Point, double> canonical;
    for (auto const& p : c.canonical) {
        canonical.emplace(Point{p[0], p[1]}, p[2]);
    }
    for (auto const& p : c.positions) {
        EXPECT_EQ(p[0] * 32.0, std::round(p[0] * 32.0));
        EXPECT_EQ(p[1] * 32.0, std::round(p[1] * 32.0));
        EXPECT_EQ(p[2], canonical.at({p[0], p[1]}));
    }
    for (auto const& [edge, faces] : mesh_edges) {
        EXPECT_LE(faces.size(), 2U);
    }
    for (auto const pair : {std::array<size_t, 3>{71U, 2U, 3U}, {71U, 3U, 4U},
        {70U, 27U, 28U}, {70U, 28U, 29U}, {76U, 30U, 31U}, {76U, 31U, 32U}}) {
        for (size_t const id : {size_t{4U}, pair[0]}) {
            Point const xy{c.positions[id][0], c.positions[id][1]};
            EXPECT_TRUE(calls.contains({xy, pair[1], pair[2]}));
        }
    }
    // Exact geometric chains recover every protected native subedge. Only the
    // declared radial reroute changes a straight segment into a recorded path.
    auto const on_grid = [&](uint32_t const p, uint32_t const a, uint32_t const b) {
        auto const& x = c.positions[p];
        auto const& u = c.positions[a];
        auto const& v = c.positions[b];
        return (x[0] - u[0]) * (v[1] - u[1]) == (x[1] - u[1]) * (v[0] - u[0])
            && x[0] >= std::min(u[0], v[0]) && x[0] <= std::max(u[0], v[0])
            && x[1] >= std::min(u[1], v[1]) && x[1] <= std::max(u[1], v[1]);
    };
    auto const chain = [&](uint32_t const a, uint32_t const b) {
        std::set<uint32_t> reached;
        for (uint32_t i = 0U; i < c.positions.size(); ++i) {
            if (c.positions[i][0] == c.positions[a][0] && c.positions[i][1] == c.positions[a][1]) {
                reached.insert(i);
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto const& [edge, faces] : mesh_edges) {
                static_cast<void>(faces);
                if (on_grid(edge[0], a, b) && on_grid(edge[1], a, b)
                    && reached.contains(edge[0]) != reached.contains(edge[1])) {
                    changed = reached.insert(edge[0]).second || changed;
                    changed = reached.insert(edge[1]).second || changed;
                }
            }
        }
        // Coincident input IDs retain a zero-length geometric subchain.
        return std::any_of(reached.begin(), reached.end(), [&](uint32_t const i) {
            return c.positions[i][0] == c.positions[b][0] && c.positions[i][1] == c.positions[b][1];
        });
    };
    auto const native_on = [](auto const& p, auto const& a, auto const& b) {
        double const dx = b[0] - a[0], dy = b[1] - a[1], length = std::hypot(dx, dy);
        double const along = (p[0] - a[0]) * dx + (p[1] - a[1]) * dy;
        return length != 0.0 && along >= -1e-8 * length && along <= length * length + 1e-8 * length
            && std::abs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) <= 1e-8 * length;
    };
    size_t required = 0U;
    for (auto const& [edge, faces] : native_edges) {
        bool protected_edge = faces.size() == 1U;
        for (auto const& constraint : c.constraints) {
            protected_edge = protected_edge || (native_on(c.native.vertices[edge[0]], c.anchors[constraint[0]],
                c.anchors[constraint[1]]) && native_on(c.native.vertices[edge[1]], c.anchors[constraint[0]],
                c.anchors[constraint[1]]));
        }
        if (!protected_edge) {
            continue;
        }
        ++required;
        if (edge == Edge{13U, 59U}) {
            EXPECT_TRUE(chain(59U, 45U));
            EXPECT_TRUE(chain(45U, 13U));
        } else {
            EXPECT_TRUE(chain(edge[0], edge[1])) << edge[0] << ',' << edge[1];
        }
    }
    EXPECT_EQ(required, 147U);
    for (size_t i = 0U; i < c.native.vertices.size(); ++i) {
        auto const& p = c.native.vertices[i];
        Point const xy{p[0], p[1]};
        bool const fixed_input = std::find(c.anchors.begin(), c.anchors.end(), xy) != c.anchors.end()
            || p[0] == c.minimum[0] || p[0] == c.maximum[0]
            || p[1] == c.minimum[1] || p[1] == c.maximum[1];
        if (fixed_input) {
            EXPECT_EQ(c.positions[i][0], p[0]);
            EXPECT_EQ(c.positions[i][1], p[1]);
        }
    }
    using FixedPoint = std::array<int64_t, 3>;
    auto const fixed = [&](uint32_t const id) {
        auto const& p = c.positions[id];
        return FixedPoint{std::llround(p[0] * 32.0), std::llround(p[1] * 32.0), std::llround(p[2] * 32.0)};
    };
    std::map<std::array<FixedPoint, 2>, size_t> old_exterior, new_exterior;
    for (auto const& [edge, faces] : native_edges) {
        if (faces.size() == 1U) {
            auto a = fixed(edge[0]), b = fixed(edge[1]);
            old_exterior[{std::min(a, b), std::max(a, b)}] = old_owners[faces[0]];
        }
    }
    for (auto const& [edge, faces] : mesh_edges) {
        if (faces.size() == 1U) {
            auto a = fixed(edge[0]), b = fixed(edge[1]);
            new_exterior[{std::min(a, b), std::max(a, b)}] = c.owners[faces[0]];
        }
    }
    EXPECT_EQ(old_exterior, new_exterior);
    std::map<std::array<FixedPoint, 2>, int> oriented;
    for (auto const& t : c.triangles) {
        for (size_t k = 0U; k < 3U; ++k) {
            auto a = fixed(t[k]), b = fixed(t[(k + 1U) % 3U]);
            if (b < a) {
                --oriented[{b, a}];
            } else {
                ++oriented[{a, b}];
            }
        }
    }
    for (auto const& [edge, count] : oriented) {
        bool const exterior = (edge[0][0] == edge[1][0]
            && (edge[0][0] == c.minimum[0] * 32 || edge[0][0] == c.maximum[0] * 32))
            || (edge[0][1] == edge[1][1]
            && (edge[0][1] == c.minimum[1] * 32 || edge[0][1] == c.maximum[1] * 32));
        if (!exterior) {
            EXPECT_EQ(count, 0);
        }
    }
}

TEST(RiverBedMesh, ActualCollapsedOwnerFanRejectsDifferentHeightState)
{
    auto c = actualRiverOwnerCase();
    std::set<std::tuple<Point, size_t, size_t>> calls;
    std::string failure;
    EXPECT_FALSE(repairActualRiverOwnerCase(c, true, false, failure, calls));
    EXPECT_EQ(failure, "renderer PSLG collapses distinct emitted owner states");
}

TEST(RiverBedMesh, ActualCollapsedOwnerFanRejectsDifferentMaterialState)
{
    auto c = actualRiverOwnerCase();
    std::set<std::tuple<Point, size_t, size_t>> calls;
    std::string failure;
    EXPECT_FALSE(repairActualRiverOwnerCase(c, false, true, failure, calls));
    EXPECT_EQ(failure, "renderer PSLG collapses distinct emitted owner states");
}


TEST(RiverBedMesh, CrossedRiverRadialsPreserveTheRetiredExactCrop)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    configuration.use_v10_fine_terrain = true;
    shared::TerrainWorld const terrain{generation::prepareAtlas(configuration)};
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain.identity()));

    // The retained column changes the east neighbor's segmentation while the
    // visual disk excludes the formerly exact crop at (2055, 2042).
    constexpr client::TerrainLodColumnCoordinate RETAINED{2'058U, 2'042U};
    constexpr client::TerrainLodColumnCoordinate VISUAL_CENTER{2'097U, 2'048U};
    for (auto const offset : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
            std::array{0, -1}, std::array{0, 1}}) {
        ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain.sampleSurface(
            {static_cast<int32_t>(RETAINED.x) + offset[0], static_cast<int32_t>(RETAINED.y) + offset[1], 0})));
    }
    auto input = client::captureTerrainLodColumn(sparse, RETAINED.x, RETAINED.y);
    ASSERT_TRUE(input);
    std::array const columns{std::make_shared<client::TerrainLodColumnInput const>(std::move(*input))};
    auto const scene = client::buildTerrainLodSceneFromColumns(
        terrain, 32'768U, 32'768U, columns, {}, nullptr, {}, true, VISUAL_CENTER);
    ASSERT_EQ(scene->actual_column_count, 1U);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*scene, sparse));
    auto const crop = std::ranges::find_if(scene->groups, [](auto const& group) {
        return group.origin_x == 32'880U && group.origin_y == 32'672U && group.extent == 16U;
    });
    ASSERT_NE(crop, scene->groups.end());

    int64_t doubled_area_q5 = 0;
    for (auto const& face : client::terrainLodGroupFaces(*crop)) {
        ASSERT_EQ(face.faceDirection(), client::StoneFaceInstance::TERRAIN_TRIANGLE);
        int64_t const q5 = (face.direction & client::StoneFaceInstance::TERRAIN_TRIANGLE_XY_Q5) != 0U ? 1 : 32;
        int64_t const ux = static_cast<int16_t>(face.u_extent) * q5;
        int64_t const uy = static_cast<int16_t>(face.u_extent >> 16U) * q5;
        int64_t const vx = static_cast<int16_t>(face.v_extent) * q5;
        int64_t const vy = static_cast<int16_t>(face.v_extent >> 16U) * q5;
        int64_t const doubled_area = ux * vy - uy * vx;
        EXPECT_GE(doubled_area, 0);
        doubled_area_q5 += doubled_area;
    }
    EXPECT_EQ(doubled_area_q5, 2 * 512 * 512);
}

TEST(RiverBedMesh, RiverTransitionUsesCapturedTopMaterialAfterSameHeightEdit)
{
    generation::GenerationConfiguration configuration;
    configuration.seed = 42U;
    configuration.model_tag = generation::TerrainModelTag::V10;
    shared::TerrainWorld const terrain{generation::prepareAtlas(configuration)};
    shared::SparseWorld sparse{{}, shared::SparseWorldGenerationMode::MaterializedOnly};
    ASSERT_TRUE(sparse.setTerrainIdentity(terrain.identity()));

    constexpr int32_t COLUMN_X = 2'174;
    constexpr int32_t COLUMN_Y = 1'889;
    constexpr int32_t EDGE_X = 34'793;
    constexpr int32_t EDGE_Y = 30'240;
    auto const publish_halo = [&](int32_t const x) {
        for (auto const [dx, dy] : {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0},
                std::array{0, -1}, std::array{0, 1}}) {
            ASSERT_TRUE(sparse.publishAuthoritativeSurface(terrain.sampleSurface({(x + dx + 4096) % 4096,
                COLUMN_Y + dy, 0})));
        }
    };
    publish_halo(COLUMN_X);
    publish_halo(0);
    auto const wrapped = client::captureTerrainLodColumn(sparse, 0U, COLUMN_Y);
    ASSERT_TRUE(wrapped);
    auto const wrapped_top = sparse.authoritativeColumn(0, COLUMN_Y * 16);
    ASSERT_TRUE(wrapped_top);
    EXPECT_EQ(wrapped->edge_materials[0][0], wrapped_top->materialAt(wrapped_top->highest_solid_z));

    using Input = std::shared_ptr<client::TerrainLodColumnInput const>;
    auto const capture = [&] {
        auto input = client::captureTerrainLodColumn(sparse, COLUMN_X, COLUMN_Y);
        return input ? std::make_shared<client::TerrainLodColumnInput const>(std::move(*input)) : Input{};
    };
    auto const original_input = capture();
    ASSERT_TRUE(original_input);
    auto const top = sparse.authoritativeColumn(EDGE_X, EDGE_Y - 1);
    ASSERT_TRUE(top);
    shared::Block const original_material = top->materialAt(top->highest_solid_z);
    ASSERT_NE(original_material, shared::Block::Air);
    ASSERT_NE(original_material, shared::Block::Stone);
    constexpr size_t EDGE_INDEX = EDGE_X - COLUMN_X * 16;
    EXPECT_EQ(original_input->edge_materials[3][EDGE_INDEX], original_material);
    auto const build = [&](Input const& input, client::TerrainLodScene const* const previous = nullptr) {
        std::array const inputs{input};
        return client::buildTerrainLodSceneFromColumns(terrain, 34'784U, 30'212U, inputs, {}, previous);
    };
    auto const transition_group = [&](client::TerrainLodScene const& scene) {
        return std::ranges::find_if(scene.groups, [&](auto const& group) {
            return group.origin_x == COLUMN_X * 16U && group.origin_y == EDGE_Y && group.extent == 16U;
        });
    };
    auto const expect_edge_material = [&](
        std::span<client::StoneFaceInstance const> const faces,
        shared::Block const expected) {
        size_t matched = 0U;
        for (auto const& face : faces) {
            if (face.faceDirection() != client::StoneFaceInstance::TERRAIN_TRIANGLE
                || (face.direction & client::StoneFaceInstance::TERRAIN_TRIANGLE_XY_Q5) == 0U) {
                continue;
            }
            auto const sx = [](uint32_t const word) { return static_cast<int16_t>(word); };
            int32_t const ux = sx(face.u_extent), uy = sx(face.u_extent >> 16U);
            int32_t const vx = sx(face.v_extent), vy = sx(face.v_extent >> 16U);
            if (static_cast<int64_t>(ux) * vy <= static_cast<int64_t>(uy) * vx) {
                continue;
            }
            std::array const xs{face.x, face.x + ux, face.x + vx};
            std::array const ys{face.y, face.y + uy, face.y + vy};
            std::array<bool, 3> on_edge{};
            for (size_t i = 0U; i < on_edge.size(); ++i) {
                on_edge[i] = ys[i] == EDGE_Y * 32 && xs[i] >= EDGE_X * 32 && xs[i] <= (EDGE_X + 1) * 32;
            }
            if (std::ranges::count(on_edge, true) != 2) {
                continue;
            }
            ++matched;
            for (size_t i = 0U; i < on_edge.size(); ++i) {
                if (on_edge[i]) {
                    EXPECT_EQ(static_cast<shared::Block>((face.material_id >> (i * 8U)) & 255U), expected);
                }
            }
        }
        EXPECT_GT(matched, 0U);
    };
    auto const original = build(original_input);
    auto const original_group = transition_group(*original);
    ASSERT_NE(original_group, original->groups.end());
    expect_edge_material(client::terrainLodGroupFaces(*original_group), original_material);

    ASSERT_TRUE(sparse.applyEditDelta({EDGE_X, EDGE_Y - 1, top->highest_solid_z},
        shared::Block::Stone, 0U, 1U, 1U));
    auto const edited_input = capture();
    ASSERT_TRUE(edited_input);
    EXPECT_EQ(edited_input->edge_heights, original_input->edge_heights);
    EXPECT_EQ(edited_input->edge_materials[3][EDGE_INDEX], shared::Block::Stone);
    auto const edited = build(edited_input, original.get());
    auto const fresh = build(edited_input);
    EXPECT_TRUE(client::terrainLodSourcesCurrent(*edited, sparse));
    EXPECT_EQ(edited->authority_sources, fresh->authority_sources);
    auto const edited_group = transition_group(*edited);
    auto const fresh_group = transition_group(*fresh);
    ASSERT_NE(edited_group, edited->groups.end());
    ASSERT_NE(fresh_group, fresh->groups.end());
    auto const edited_faces = client::terrainLodGroupFaces(*edited_group);
    EXPECT_TRUE(std::ranges::equal(edited_faces, client::terrainLodGroupFaces(*fresh_group)));
    expect_edge_material(edited_faces, shared::Block::Stone);
}
