#include <client/RiverBedMesh.hpp>

#include <CDT.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace client {

RiverBedMesh buildRiverBedMesh(
    std::array<double, 2> const& minimum,
    std::array<double, 2> const& maximum,
    std::span<std::array<double, 2> const> anchors,
    std::span<std::array<uint32_t, 2> const> edges,
    std::function<double(double, double)> const& height_at)
{
    if (!std::isfinite(minimum[0]) || !std::isfinite(minimum[1])
        || !std::isfinite(maximum[0]) || !std::isfinite(maximum[1])
        || minimum[0] >= maximum[0] || minimum[1] >= maximum[1] || !height_at) {
        throw std::invalid_argument("invalid river mesh crop or height callback");
    }
    if (anchors.size() > std::numeric_limits<uint32_t>::max() - 7U) {
        throw std::invalid_argument("too many river mesh anchors");
    }
    std::array<double, 2> const origin{
        std::clamp(0.0, minimum[0], maximum[0]), std::clamp(0.0, minimum[1], maximum[1])};
    std::vector<CDT::V2d<double>> points;
    std::map<std::array<double, 2>, uint32_t> indices;
    auto add_point = [&](std::array<double, 2> const& point) {
        if (!std::isfinite(point[0]) || !std::isfinite(point[1])
            || point[0] < minimum[0] || point[0] > maximum[0]
            || point[1] < minimum[1] || point[1] > maximum[1]) {
            throw std::invalid_argument("river mesh point outside crop or non-finite");
        }
        auto const [it, inserted] = indices.emplace(point, static_cast<uint32_t>(points.size()));
        if (inserted) {
            points.emplace_back(point[0] - origin[0], point[1] - origin[1]);
        }
        return it->second;
    };
    std::array<uint32_t, 4> const corners{
        add_point(minimum), add_point({maximum[0], minimum[1]}),
        add_point(maximum), add_point({minimum[0], maximum[1]})};
    std::vector<uint32_t> mapping;
    mapping.reserve(anchors.size());
    for (auto const& point : anchors) {
        mapping.push_back(add_point(point));
    }
    std::set<std::pair<uint32_t, uint32_t>> unique_edges;
    auto add_edge = [&](uint32_t a, uint32_t b) {
        if (a != b) {
            unique_edges.emplace(std::min(a, b), std::max(a, b));
        }
    };
    for (size_t i = 0; i < corners.size(); ++i) {
        add_edge(corners[i], corners[(i + 1U) % corners.size()]);
    }
    for (auto const& edge : edges) {
        if (edge[0] >= anchors.size() || edge[1] >= anchors.size()) {
            throw std::invalid_argument("invalid river mesh edge index");
        }
        add_edge(mapping[edge[0]], mapping[edge[1]]);
    }
    std::vector<CDT::Edge> constraints;
    constraints.reserve(unique_edges.size());
    for (auto const& [a, b] : unique_edges) {
        constraints.emplace_back(a, b);
    }
    // Rounded intersections of concurrent edges must reuse the same incident vertex.
    // Keep CDT arithmetic crop-local and its incidence tolerance at floating-point roundoff scale.
    CDT::Triangulation<double> triangulation(CDT::VertexInsertionOrder::AsProvided,
        CDT::IntersectingConstraintEdges::TryResolve,
        64.0 * std::numeric_limits<double>::epsilon()
            * std::max(maximum[0] - minimum[0], maximum[1] - minimum[1]));
    triangulation.insertVertices(points);
    triangulation.insertEdges(constraints);
    triangulation.eraseSuperTriangle();
    if (triangulation.vertices.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("too many river mesh output vertices");
    }
    RiverBedMesh result;
    result.vertices.reserve(triangulation.vertices.size());
    for (auto const& local_point : triangulation.vertices) {
        CDT::V2d<double> const point(local_point.x + origin[0], local_point.y + origin[1]);
        if (!std::isfinite(point.x) || !std::isfinite(point.y)
            || point.x < minimum[0] || point.x > maximum[0]
            || point.y < minimum[1] || point.y > maximum[1]) {
            throw std::invalid_argument("invalid river mesh output geometry");
        }
        double const height = height_at(point.x, point.y);
        if (!std::isfinite(height)) {
            throw std::invalid_argument("non-finite river mesh height");
        }
        result.vertices.push_back({point.x, point.y, height});
    }
    result.triangles.reserve(triangulation.triangles.size());
    for (auto const& triangle : triangulation.triangles) {
        auto const& v = triangle.vertices;
        auto const& a = result.vertices.at(v[0]);
        auto const& b = result.vertices.at(v[1]);
        auto const& c = result.vertices.at(v[2]);
        double const area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        if (!std::isfinite(area) || area <= 0.0) {
            throw std::invalid_argument("invalid river mesh triangle");
        }
        result.triangles.push_back({v[0], v[1], v[2]});
    }
    return result;
}

} // namespace client
