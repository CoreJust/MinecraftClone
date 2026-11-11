#include <client/TerrainLodPresentation.hpp>

#include <client/RiverBedMesh.hpp>
#include <client/TerrainLodSelection.hpp>
#include <client/detail/RiverFanQuantization.hpp>
#include <client/render/StoneIndirectDraws.hpp>
#include <client/render/TerrainLodDrawIndex.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/ChunkMesher.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/WorldCoverage.hpp>
#include <generation/Generation.hpp>
#include <generation/V10Provider.hpp>
#include <core/IO/Log.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace client {

struct TerrainLodNormalContext final {
    struct Group final {
        std::weak_ptr<std::vector<StoneFaceInstance> const> storage;
        bool empty = false;
        std::shared_ptr<std::vector<uint32_t> const> vertex_buckets;
    };
    std::vector<Group> groups;
};

namespace detail {
std::shared_ptr<TerrainLodNormalContext> prepareTerrainLodMeshNormalsImpl(
    TerrainLodScene& scene, std::stop_token const& cancellation, std::span<StoneFaceInstance> staged_faces);
void prepareTerrainLodMeshNormals(TerrainLodScene& scene, std::stop_token const& cancellation,
    std::span<StoneFaceInstance> staged_faces = {});
} // namespace detail

namespace {

struct RiverFanOutputCache;
struct RiverFanQueryMemo;
struct OrdinaryFanEdge final {
    std::array<uint16_t, 4> points;
    std::array<shared::Block, 2> materials;
    bool operator==(OrdinaryFanEdge const&) const = default;
};
struct OrdinaryFanInput final {
    int32_t minimum_height;
    int32_t maximum_height;
    shared::Block center_material;
    std::vector<OrdinaryFanEdge> edges;
    bool operator==(OrdinaryFanInput const&) const = default;
};
struct OrdinaryFanEntry final {
    std::shared_ptr<OrdinaryFanInput const> input;
    std::weak_ptr<std::vector<StoneFaceInstance> const> faces;
};
using OrdinaryFanInputs = std::unordered_map<uint64_t, OrdinaryFanEntry>;

bool terrainLodPhaseTraceEnabled()
{
    char const* const trace = std::getenv("MC_DIAGNOSTIC_STREAMING_TRACE");
    return trace != nullptr && std::string_view(trace) == "1";
}

auto terrainLodPhaseTime(bool const enabled)
{
    return enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
}

} // namespace

struct TerrainLodRiverConstraints final {
    std::shared_ptr<generation::GlobalAtlas const> atlas;
    std::shared_ptr<river_field_candidate::PreparedRiverField const> field;
    std::unordered_map<uint64_t, std::shared_ptr<river_field_candidate::RiverConstraintExport const>> regions;
    std::unordered_set<uint64_t> empty_regions;
    river_field_candidate::RiverConstraintExport empty_region{};
    std::shared_ptr<RiverFanOutputCache const> fan_outputs;
    std::shared_ptr<RiverFanQueryMemo const> query_memo;
    std::shared_ptr<OrdinaryFanInputs const> ordinary_fans;
};


struct LocalCell final {
    uint32_t level, x, y, extent;
    std::array<int32_t, 4> heights, temperatures, materials;
    bool operator==(LocalCell const&) const = default;
};
struct LocalCorner final {
    uint32_t x, y, owner_level, owner_x, owner_y;
    std::array<int32_t, 3> value;
    bool operator==(LocalCorner const&) const = default;
};
struct LocalCapture final {
    uint32_t x, y;
    std::array<std::array<uint16_t, 16>, 4> heights;
    std::array<std::array<shared::Block, 16>, 4> materials;
    std::vector<TerrainLodChunkSource> chunks;
    std::vector<TerrainLodAuthoritySource> authority;
    std::vector<TerrainLodEditSource> edits;
    bool operator==(LocalCapture const&) const = default;
};
struct TerrainLodLocalLayout final {
    struct Samples final {
        std::vector<LocalCell> cells;
        std::vector<uint32_t> traversal_order;
        std::vector<shared::Block> center_materials;
        std::vector<size_t> root_subtree_offsets;
    };
    std::shared_ptr<Samples const> samples;
    std::vector<uint64_t> forcing;
    std::weak_ptr<generation::GlobalAtlas const> field;
    std::weak_ptr<shared::TerrainLodArtifact const> root;
    uint32_t center_x = 0U, center_y = 0U;
    std::optional<TerrainLodColumnCoordinate> visual;
};
struct TerrainLodLocalUnit final {
    shared::TerrainIdentity identity;
    std::shared_ptr<generation::GlobalAtlas const> field;
    uint32_t root_extent = 0U;
    std::vector<TerrainLodColumnCoordinate> targets, halo;
    std::vector<LocalCell> cells;
    std::vector<LocalCorner> corners;
    std::vector<LocalCapture> dependencies;
    std::vector<LocalCapture> used;
};
namespace {
constexpr uint32_t TRANSITION_WIDTH = 32U;
struct LocalUnsupported final { char const* reason; };
struct LocalBuild final {
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> base_columns, columns;
    std::span<TerrainLodColumnCoordinate const> targets;
    std::span<std::shared_ptr<TerrainLodLocalUnit const> const> constraints;
    std::shared_ptr<TerrainLodLocalUnit> unit;
    bool transient_recenter = false;
    std::unordered_set<uint64_t> dirty_groups;
};
bool localContains(std::span<TerrainLodColumnCoordinate const> coordinates, uint32_t x, uint32_t y)
{
    return std::ranges::any_of(coordinates, [=](auto q) { return q.x == x && q.y == y; });
}
bool localTouches(std::span<TerrainLodColumnCoordinate const> targets, uint32_t x, uint32_t y, uint32_t extent)
{
    auto axis = [](uint32_t a, uint32_t size, uint32_t q) {
        int64_t const lo = static_cast<int64_t>(q) * 16 - TRANSITION_WIDTH;
        int64_t const hi = static_cast<int64_t>(q) * 16 + 16 + TRANSITION_WIDTH;
        for (int64_t shift : {-65'536LL, 0LL, 65'536LL}) {
            if (static_cast<int64_t>(a) + shift <= hi && static_cast<int64_t>(a) + shift + size >= lo) {
                return true;
            }
        }
        return false;
    };
    return std::ranges::any_of(targets, [&](auto q) { return axis(x, extent, q.x) && axis(y, extent, q.y); });
}
LocalCapture localCapture(TerrainLodColumnInput const& input)
{
    return {input.x, input.y, input.edge_heights, input.edge_materials,
        input.sources, input.authority_sources, input.edit_sources};
}
std::vector<LocalCapture> localDependencies(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::span<TerrainLodColumnCoordinate const> halo,
    std::span<TerrainLodColumnCoordinate const> excluded,
    std::optional<TerrainLodColumnCoordinate> visual)
{
    std::vector<LocalCapture> result;
    for (auto const& input : columns) {
        if (localContains(halo, input->x, input->y) && !localContains(excluded, input->x, input->y)
            && (!visual || terrainLodVisualColumn(input->x, input->y, visual->x, visual->y))) {
            result.push_back(localCapture(*input));
        }
    }
    std::ranges::sort(result, [](auto const& a, auto const& b) { return std::tie(a.x, a.y) < std::tie(b.x, b.y); });
    return result;
}
std::vector<TerrainLodColumnCoordinate> localCaptureHalo(std::vector<TerrainLodColumnCoordinate> const& writes)
{
    constexpr int32_t WIDTH = static_cast<int32_t>(TRANSITION_WIDTH), SIDE = 16, REACH = WIDTH / SIDE + 1;
    std::set<std::pair<uint32_t, uint32_t>> keys;
    auto add = [&](int32_t x, int32_t y) {
        keys.emplace(static_cast<uint32_t>((x + 4'096) % 4'096), static_cast<uint32_t>((y + 4'096) % 4'096));
    };
    for (auto w : writes) {
        for (auto n : {std::pair<int32_t, int32_t>{0, 0}, std::pair<int32_t, int32_t>{-1, 0},
                std::pair<int32_t, int32_t>{1, 0}, std::pair<int32_t, int32_t>{0, -1},
                std::pair<int32_t, int32_t>{0, 1}}) {
            for (int32_t dy = -2; dy <= 2; ++dy) {
                for (int32_t dx = -2; dx <= 2; ++dx) {
                    add(static_cast<int32_t>(w.x) + n.first + dx, static_cast<int32_t>(w.y) + n.second + dy);
                }
            }
        }
        for (int32_t cy = -REACH; cy <= REACH; ++cy) {
            for (int32_t cx = -REACH; cx <= REACH; ++cx) {
                for (int32_t edge = 0; edge < 4; ++edge) {
                    bool contributes = false;
                    for (int32_t along = 0; along <= SIDE; ++along) {
                        int32_t x = cx * SIDE + (edge < 2 ? (edge == 1 ? SIDE : 0) : along);
                        int32_t y = cy * SIDE + (edge < 2 ? along : (edge == 3 ? SIDE : 0));
                        int32_t dx = std::max({-x, 0, x - SIDE}), dy = std::max({-y, 0, y - SIDE});
                        // <= is conservative for the production strict<32m test and the exact-boundary branch.
                        if (dx * dx + dy * dy <= WIDTH * WIDTH) {
                            contributes = true;
                            break;
                        }
                    }
                    if (contributes) {
                        add(static_cast<int32_t>(w.x) + cx, static_cast<int32_t>(w.y) + cy);
                        add(static_cast<int32_t>(w.x) + cx + (edge == 0 ? -1 : edge == 1 ? 1 : 0),
                            static_cast<int32_t>(w.y) + cy + (edge == 2 ? -1 : edge == 3 ? 1 : 0));
                    }
                }
            }
        }
    }
    std::vector<TerrainLodColumnCoordinate> result;
    for (auto [x, y] : keys) {
        result.push_back({x, y});
    }
    return result;
}

std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneImpl(
    shared::TerrainWorld const&, uint32_t, uint32_t, std::span<TerrainLodActualColumn const>,
    std::stop_token const&, TerrainLodScene const*, std::span<TerrainLodColumnCoordinate const>,
    bool, std::optional<TerrainLodColumnCoordinate>, LocalBuild*,
    std::shared_ptr<shared::TerrainLodArtifact const> const& prepared_root = {});
} // namespace

namespace detail {

std::optional<RiverBedMesh> tryBuildRiverStarMesh(
    std::array<double, 2> const minimum,
    std::array<double, 2> const maximum,
    std::span<std::array<double, 2> const> const anchors,
    std::span<std::array<uint32_t, 2> const> const edges,
    std::function<double(double, double)> const& height)
{
    size_t const count = edges.size() / 2U;
    if (count < 4U || edges.size() % 2U != 0U || count > (UINT32_MAX - 1U) / 4U
        || anchors.size() != count * 4U + 1U || !height) {
        return std::nullopt;
    }
    auto const grid = [](std::array<double, 2> const point) -> std::optional<std::array<int64_t, 2>> {
        std::array<int64_t, 2> result{};
        for (size_t i = 0U; i < 2U; ++i) {
            double const scaled = point[i] * 32.0;
            if (!std::isfinite(scaled) || scaled != std::round(scaled)
                || scaled < INT32_MIN || scaled > INT32_MAX) {
                return std::nullopt;
            }
            result[i] = static_cast<int64_t>(scaled);
        }
        return result;
    };
    auto const lo = grid(minimum), hi = grid(maximum), center = grid(anchors[0]);
    if (!lo || !hi || !center || (*hi)[0] <= (*lo)[0] || (*hi)[1] <= (*lo)[1]
        || (*hi)[0] - (*lo)[0] > 1'000'000 || (*hi)[1] - (*lo)[1] > 1'000'000
        || (*center)[0] <= (*lo)[0] || (*center)[0] >= (*hi)[0]
        || (*center)[1] <= (*lo)[1] || (*center)[1] >= (*hi)[1]) {
        return std::nullopt;
    }
    // Local Q5 deltas <= 10^6 bound every determinant by 2*10^12.
    int64_t const target_area = 2 * ((*hi)[0] - (*lo)[0]) * ((*hi)[1] - (*lo)[1]);
    int64_t total_area = 0;
    std::vector<std::array<double, 2>> perimeter;
    perimeter.reserve(count);
    for (size_t i = 0U; i < count; ++i) {
        uint32_t const first = static_cast<uint32_t>(1U + 4U * i);
        auto const& a = anchors[first + 1U];
        auto const& b = anchors[first + 3U];
        if (edges[2U * i] != std::array{first, first + 1U}
            || edges[2U * i + 1U] != std::array{first + 2U, first + 3U}
            || anchors[first] != anchors[0] || anchors[first + 2U] != a
            || b != anchors[1U + 4U * ((i + 1U) % count) + 1U]
            || std::find(perimeter.begin(), perimeter.end(), a) != perimeter.end()) {
            return std::nullopt;
        }
        auto const qa = grid(a), qb = grid(b);
        if (!qa || !qb) {
            return std::nullopt;
        }
        bool const side = ((*qa)[1] == (*lo)[1] && (*qb)[1] == (*lo)[1] && (*qa)[0] < (*qb)[0])
            || ((*qa)[0] == (*hi)[0] && (*qb)[0] == (*hi)[0] && (*qa)[1] < (*qb)[1])
            || ((*qa)[1] == (*hi)[1] && (*qb)[1] == (*hi)[1] && (*qa)[0] > (*qb)[0])
            || ((*qa)[0] == (*lo)[0] && (*qb)[0] == (*lo)[0] && (*qa)[1] > (*qb)[1]);
        for (size_t k = 0U; k < 2U; ++k) {
            if ((*qa)[k] < (*lo)[k] || (*qa)[k] > (*hi)[k]
                || (*qb)[k] < (*lo)[k] || (*qb)[k] > (*hi)[k]) {
                return std::nullopt;
            }
        }
        int64_t const area = ((*qa)[0] - (*center)[0]) * ((*qb)[1] - (*center)[1])
            - ((*qa)[1] - (*center)[1]) * ((*qb)[0] - (*center)[0]);
        if (!side || area <= 0 || area > target_area - total_area) {
            return std::nullopt;
        }
        total_area += area;
        perimeter.push_back(a);
    }
    if (total_area != target_area) {
        return std::nullopt;
    }
    RiverBedMesh result;
    result.vertices.reserve(count + 1U);
    result.triangles.reserve(count);
    auto const add = [&](std::array<double, 2> const point) {
        for (uint32_t i = 0U; i < result.vertices.size(); ++i) {
            if (result.vertices[i][0] == point[0] && result.vertices[i][1] == point[1]) {
                return i;
            }
        }
        double const z = height(point[0], point[1]);
        if (!std::isfinite(z)) {
            throw std::invalid_argument("non-finite river mesh height");
        }
        result.vertices.push_back({point[0], point[1], z});
        return static_cast<uint32_t>(result.vertices.size() - 1U);
    };
    // Match the utility's vertex/sampling order; triangle order is immaterial
    // only for fans whose shared radial state cannot emit a bridge.
    for (auto const point : {minimum, std::array{maximum[0], minimum[1]}, maximum,
             std::array{minimum[0], maximum[1]}}) {
        add(point);
    }
    uint32_t const center_id = add(anchors[0]);
    std::vector<uint32_t> indices;
    for (auto const point : perimeter) {
        indices.push_back(add(point));
    }
    for (size_t i = 0U; i < count; ++i) {
        result.triangles.push_back({center_id, indices[i], indices[(i + 1U) % count]});
    }
    return result;
}

enum class LatticeConstraintKind : uint8_t { Boundary, Radial, Interior };

// Renderer-only path noding. Inputs stay fixed; each original edge retains its
// ordered path. Integer Q5 predicates distinguish proper crossings from roundoff.
bool nodeRiverConstraintCrossingsImpl(
    std::span<std::array<double, 2> const> const points,
    std::span<std::array<uint32_t, 2> const> const edges,
    std::vector<std::vector<uint32_t>>& paths,
    std::span<LatticeConstraintKind const> constraint_kinds,
    std::vector<std::array<double, 2>>* additions)
{
    if (points.empty() || points.size() > std::numeric_limits<uint32_t>::max()
        || (additions != nullptr && constraint_kinds.size() != edges.size())) {
        return false;
    }
    std::array<int64_t, 2> minimum{
        std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()};
    std::vector<std::array<int64_t, 2>> grid;
    std::map<std::array<int64_t, 2>, uint32_t> indices;
    for (uint32_t i = 0U; i < points.size(); ++i) {
        std::array<int64_t, 2> q{};
        for (size_t k = 0U; k < 2U; ++k) {
            double const scaled = points[i][k] * 32.0;
            if (!std::isfinite(scaled) || scaled != std::round(scaled)
                || scaled < std::numeric_limits<int32_t>::min()
                || scaled > std::numeric_limits<int32_t>::max()) {
                return false;
            }
            q[k] = static_cast<int64_t>(scaled);
            minimum[k] = std::min(minimum[k], q[k]);
        }
        grid.push_back(q);
        if (!indices.emplace(q, i).second) {
            return false;
        }
    }
    // With local components <= 10^6, all determinants and intersection
    // numerators below are bounded by 4*10^18 and fit signed 64-bit arithmetic.
    for (auto& point : grid) {
        for (size_t k = 0U; k < 2U; ++k) {
            point[k] -= minimum[k];
            if (point[k] > 1'000'000) {
                return false;
            }
        }
    }
    auto const cross = [](auto const& a, auto const& b, auto const& c) {
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    };
    auto const opposite = [](int64_t const a, int64_t const b) {
        return a != 0 && b != 0 && (a < 0) != (b < 0);
    };
    paths.clear();
    for (auto const& edge : edges) {
        if (edge[0] >= points.size() || edge[1] >= points.size()) {
            return false;
        }
        paths.push_back({edge[0], edge[1]});
    }
    bool changed = false;
    for (size_t i = 0U; i < edges.size(); ++i) {
        auto const a = grid[edges[i][0]];
        auto const b = grid[edges[i][1]];
        for (size_t j = i + 1U; j < edges.size(); ++j) {
            auto const c = grid[edges[j][0]];
            auto const d = grid[edges[j][1]];
            if (!opposite(cross(a, b, c), cross(a, b, d))
                || !opposite(cross(c, d, a), cross(c, d, b))) {
                continue;
            }
            int64_t const rx = b[0] - a[0], ry = b[1] - a[1];
            int64_t const sx = d[0] - c[0], sy = d[1] - c[1];
            int64_t denominator = rx * sy - ry * sx;
            int64_t parameter = (c[0] - a[0]) * sy - (c[1] - a[1]) * sx;
            if (denominator < 0) {
                denominator = -denominator;
                parameter = -parameter;
            }
            std::array<int64_t, 2> pixel{};
            for (size_t k = 0U; k < 2U; ++k) {
                int64_t const numerator = a[k] * denominator + (b[k] - a[k]) * parameter;
                int64_t const floor = minimum[k] + numerator / denominator;
                int64_t const remainder = numerator % denominator;
                pixel[k] = floor + (2 * remainder > denominator
                    || (2 * remainder == denominator && floor >= 0) ? 1 : 0);
            }
            auto const found = indices.find(pixel);
            std::optional<uint32_t> node;
            if (found != indices.end()) {
                node = found->second;
            } else {
                // A snapped intersection can fall next to an existing endpoint.
                // Only reuse an incident endpoint within one Q5 cell of the
                // exact crossing; never move an input or introduce a new node.
                long double best_distance = 1.0L;
                for (uint32_t const candidate : {edges[i][0], edges[i][1], edges[j][0], edges[j][1]}) {
                    long double distance = 0.0L;
                    for (size_t k = 0U; k < 2U; ++k) {
                        int64_t const numerator = a[k] * denominator + (b[k] - a[k]) * parameter;
                        long double const delta = static_cast<long double>(numerator)
                            / static_cast<long double>(denominator) - static_cast<long double>(grid[candidate][k]);
                        distance += delta * delta;
                    }
                    if (distance < best_distance || (node && distance == best_distance && candidate < *node)) {
                        node = candidate;
                        best_distance = distance;
                    }
                }
                if (!node) {
                    if (additions == nullptr || constraint_kinds[i] == LatticeConstraintKind::Boundary
                        || constraint_kinds[j] == LatticeConstraintKind::Boundary
                        || (constraint_kinds[i] != LatticeConstraintKind::Interior
                            && constraint_kinds[j] != LatticeConstraintKind::Interior)
                        || grid.size() == std::numeric_limits<uint32_t>::max()) {
                        return false;
                    }
                    node = static_cast<uint32_t>(grid.size());
                    grid.push_back({pixel[0] - minimum[0], pixel[1] - minimum[1]});
                    additions->push_back({pixel[0] / 32.0, pixel[1] / 32.0});
                    indices.emplace(pixel, *node);
                }
            }
            paths[i].push_back(*node);
            paths[j].push_back(*node);
            changed = true;
        }
    }
    for (size_t i = 0U; i < paths.size(); ++i) {
        auto const& a = grid[edges[i][0]];
        auto const& b = grid[edges[i][1]];
        auto const projection = [&](uint32_t const vertex) {
            return (grid[vertex][0] - a[0]) * (b[0] - a[0])
                + (grid[vertex][1] - a[1]) * (b[1] - a[1]);
        };
        int64_t const length_squared = projection(edges[i][1]);
        for (uint32_t const vertex : paths[i]) {
            if (projection(vertex) < 0 || projection(vertex) > length_squared) {
                return false;
            }
        }
        std::sort(paths[i].begin(), paths[i].end(), [&](uint32_t const x, uint32_t const y) {
            return std::pair{projection(x), x} < std::pair{projection(y), y};
        });
        paths[i].erase(std::unique(paths[i].begin(), paths[i].end()), paths[i].end());
        if (paths[i].front() != edges[i][0] || paths[i].back() != edges[i][1]) {
            return false;
        }
    }
    // Routing can bend a path. Reject any new proper crossing before CDT;
    // its intersection vertices must not silently change the declared paths.
    for (size_t i = 0U; i < paths.size(); ++i) {
        for (size_t a = 1U; a < paths[i].size(); ++a) {
            for (size_t j = i + 1U; j < paths.size(); ++j) {
                for (size_t b = 1U; b < paths[j].size(); ++b) {
                    auto const& p = grid[paths[i][a - 1U]];
                    auto const& q = grid[paths[i][a]];
                    auto const& r = grid[paths[j][b - 1U]];
                    auto const& s = grid[paths[j][b]];
                    if (opposite(cross(p, q, r), cross(p, q, s))
                        && opposite(cross(r, s, p), cross(r, s, q))) {
                        return false;
                    }
                }
            }
        }
    }
    return changed;
}

bool nodeRiverConstraintCrossings(
    std::span<std::array<double, 2> const> const points,
    std::span<std::array<uint32_t, 2> const> const edges,
    std::vector<std::vector<uint32_t>>& paths)
{
    return nodeRiverConstraintCrossingsImpl(points, edges, paths, {}, nullptr);
}

// Attest a candidate independently of CDT: it must lie strictly inside two
// nonparallel retained segments, with every predicate exact in integer Q5.
bool isRiverConstraintIntersection(
    std::span<std::array<double, 2> const> const points,
    std::span<std::array<uint32_t, 2> const> const edges,
    std::array<double, 2> const point)
{
    auto const grid = [](std::array<double, 2> const p, std::array<int64_t, 2>& q) {
        for (size_t k = 0U; k < 2U; ++k) {
            double const scaled = p[k] * 32.0;
            if (!std::isfinite(scaled) || scaled != std::round(scaled)
                || scaled < std::numeric_limits<int32_t>::min()
                || scaled > std::numeric_limits<int32_t>::max()) {
                return false;
            }
            q[k] = static_cast<int64_t>(scaled);
        }
        return true;
    };
    std::array<int64_t, 2> origin{};
    if (!grid(point, origin)) {
        return false;
    }
    std::vector<std::array<int64_t, 2>> local;
    local.reserve(points.size());
    for (auto const& p : points) {
        std::array<int64_t, 2> q{};
        if (!grid(p, q)) {
            return false;
        }
        for (size_t k = 0U; k < 2U; ++k) {
            q[k] -= origin[k];
            // Products and differences below are bounded by 2*10^12.
            if (q[k] < -1'000'000 || q[k] > 1'000'000) {
                return false;
            }
        }
        local.push_back(q);
    }
    std::optional<std::array<int64_t, 2>> direction;
    bool crossing = false;
    for (auto const& edge : edges) {
        if (edge[0] >= local.size() || edge[1] >= local.size()) {
            return false;
        }
        auto const& a = local[edge[0]];
        auto const& b = local[edge[1]];
        if (a[0] * b[1] - a[1] * b[0] != 0 || a[0] * b[0] + a[1] * b[1] >= 0) {
            continue;
        }
        if (direction) {
            crossing = crossing || (*direction)[0] * a[1] - (*direction)[1] * a[0] != 0;
        } else {
            direction = a;
        }
    }
    return crossing;
}

void resampleRiverRenderHeights(
    std::span<std::array<double, 3> const> const original,
    std::span<std::array<double, 3>> const positions,
    std::function<double(double, double)> const& height,
    TerrainLodRiverGeometryAudit& audit)
{
    for (size_t i = 0U; i < positions.size(); ++i) {
        auto& point = positions[i];
        if (i < original.size()) {
            audit.maximum_xy_quantization_error_m = std::max(audit.maximum_xy_quantization_error_m,
                std::hypot(point[0] - original[i][0], point[1] - original[i][1]));
            if (point[0] == original[i][0] && point[1] == original[i][1]) {
                continue;
            }
        }
        // New exact intersections have no native Z; sample the same field as
        // moved native vertices before applying the owning sector's correction.
        point[2] = height(point[0], point[1]);
    }
}

shared::Block selectRiverSectorMaterial(std::array<double, 3> const weights,
    std::array<shared::Block, 3> const materials)
{
    size_t selected = 0U;
    for (size_t j = 1U; j < 3U; ++j) {
        if (weights[j] > weights[selected] || (weights[j] == weights[selected]
            && materials[j] < materials[selected])) {
            selected = j;
        }
    }
    return materials[selected];
}

} // namespace detail

namespace {

struct RiverFanSector final {
    std::array<double, 2> a;
    std::array<double, 2> b;
    std::array<double, 3> correction;
    std::array<shared::Block, 3> materials;
    std::optional<double> authority_height;
};

static constexpr size_t CACHED_FAN_SECTORS = 8U;
using RiverFanCrop = std::array<double, 6>;
using RiverFanInputs = std::array<uint64_t, 6U + CACHED_FAN_SECTORS * 12U>;

struct RiverFanOutput final {
    RiverFanInputs inputs;
    // A null source represents an empty export under the cache's field owner.
    std::shared_ptr<river_field_candidate::RiverConstraintExport const> source;
    std::vector<StoneFaceInstance> faces;
    // Local contributions use the emitter's Q5 height units.
    int32_t minimum_height;
    int32_t maximum_height;
};

struct RiverFanOutputCache final {
    std::shared_ptr<generation::GlobalAtlas const> atlas;
    std::shared_ptr<river_field_candidate::PreparedRiverField const> field;
    std::map<RiverFanCrop, std::shared_ptr<RiverFanOutput const>> entries;
};

struct RiverFanQueryMemo final {
    struct Entry final {
        std::optional<double> height;
        std::optional<bool> wet_positive;
    };

    [[nodiscard]] bool canRetain() const
    {
        return entries.size() <= 16'384U;
    }

    template<class Value, class Query>
    Value sample(double const x, double const y, std::optional<Value> Entry::* const field,
        RiverFanQueryMemo const* const predecessor, Query const& query)
    {
        // Keep fractional coordinates and the two provider results distinct.
        std::array const key{std::bit_cast<uint64_t>(x), std::bit_cast<uint64_t>(y)};
        auto found = entries.find(key);
        if (found == entries.end()) {
            // Keep the same memory bound while allowing later neighboring
            // fans to reuse samples after a large build fills the memo.
            if (entries.size() >= 65'536U) {
                entries.clear();
            }
            found = entries.try_emplace(key).first;
        }
        auto& value = found->second.*field;
        if (!value && predecessor != nullptr) {
            auto const old = predecessor->entries.find(key);
            if (old != predecessor->entries.end()) {
                value = old->second.*field;
            }
        }
        if (!value) {
            value = query();
        }
        return *value;
    }

    double sampleHeightAndWet(double const x, double const y, generation::GlobalAtlas const& atlas,
        RiverFanQueryMemo const* const predecessor)
    {
        return sample(x, y, &Entry::height, predecessor, [&] {
            auto const water = generation::sampleV10WaterGeometry(atlas, x, y, 3U);
            auto& entry = entries.at({std::bit_cast<uint64_t>(x), std::bit_cast<uint64_t>(y)});
            entry.wet_positive = (water.ocean || water.lake || water.river) && water.elevation_m > 0.0;
            return water.elevation_m + 513.0;
        });
    }

private:
    struct KeyHash final {
        size_t operator()(std::array<uint64_t, 2> const& key) const noexcept
        {
            uint64_t mixed = key[0] ^ std::rotl(key[1], 19);
            mixed ^= mixed >> 33U;
            mixed *= 0xff51'afd7'ed55'8ccdULL;
            return static_cast<size_t>(mixed ^ (mixed >> 33U));
        }
    };

    std::unordered_map<std::array<uint64_t, 2>, Entry, KeyHash> entries;
};

RiverFanInputs riverFanInputs(RiverFanCrop const& crop, std::span<RiverFanSector const> const sectors)
{
    RiverFanInputs result{};
    size_t word = 0U;
    for (double const component : crop) {
        result[word++] = std::bit_cast<uint64_t>(component);
    }
    for (auto const& sector : sectors) {
        for (auto const& point : {sector.a, sector.b}) {
            for (double const component : point) {
                result[word++] = std::bit_cast<uint64_t>(component);
            }
        }
        for (double const component : sector.correction) {
            result[word++] = std::bit_cast<uint64_t>(component);
        }
        for (auto const material : sector.materials) {
            result[word++] = static_cast<uint64_t>(material);
        }
        result[word++] = sector.authority_height.has_value();
        result[word++] = std::bit_cast<uint64_t>(sector.authority_height.value_or(0.0));
    }
    return result;
}

bool riverSceneDiagnosticCensus()
{
    char const* const value = std::getenv("MC_RIVER_SCENE_DIAGNOSTIC_CENSUS");
    return value != nullptr && std::string_view(value) == "1";
}

bool riverSceneFirstFailureOnly()
{
    char const* const value = std::getenv("MC_RIVER_SCENE_FIRST_FAILURE_ONLY");
    return value != nullptr && std::string_view(value) == "1";
}

bool claimRiverSceneFailureCapture()
{
    static std::atomic<bool> captured{false};
    return !riverSceneFirstFailureOnly() || !captured.exchange(true);
}

} // namespace

namespace detail {

struct RiverPslgFailure final {
    bool capture_claimed = false;
    std::vector<std::array<double, 2>> points;
    std::vector<std::array<uint32_t, 2>> edges;
    std::vector<std::array<uint32_t, 2>> required;
    std::vector<std::array<uint64_t, 4>> radials;
    std::vector<uint32_t> representative;
    RiverBedMesh rebuilt;
};

bool repairRiverFanQuantization(
    RiverBedMesh const& original,
    std::span<std::array<double, 2> const> const anchors,
    std::span<std::array<uint32_t, 2> const> const constraints,
    std::span<std::array<uint32_t, 2> const> const fan_radials,
    std::array<double, 2> const minimum,
    std::array<double, 2> const maximum,
    std::vector<std::array<double, 3>>& positions,
    std::vector<std::array<uint32_t, 3>>& triangles,
    std::vector<size_t>& sector_ids,
    std::span<std::array<int64_t, 2> const> const sector_seeds,
    std::function<bool(std::array<double, 2>, size_t, size_t)> const& equivalent_state,
    TerrainLodRiverGeometryAudit& audit,
    std::string& failure_reason,
    RiverPslgFailure* failure_capture)
{
    static constexpr double MAXIMUM_DISPLACEMENT_SQUARED = 2.0 / (64.0 * 64.0);
    auto const area = [&](std::array<uint32_t, 3> const& triangle) {
        auto const& a = positions[triangle[0]];
        auto const& b = positions[triangle[1]];
        auto const& c = positions[triangle[2]];
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    };
    std::vector<bool> negative(triangles.size());
    auto const classify = [&] {
        size_t count = 0U;
        negative.resize(triangles.size());
        for (size_t i = 0U; i < triangles.size(); ++i) {
            negative[i] = area(triangles[i]) < 0.0;
            count += negative[i] ? 1U : 0U;
        }
        return count;
    };
    size_t remaining = classify();
    audit.nearest_snap_top_triangle_reversal_count += remaining;
    std::set<std::array<double, 2>> const input_points(anchors.begin(), anchors.end());
    std::vector<bool> fixed(positions.size());
    for (size_t i = 0U; i < positions.size(); ++i) {
        auto const& point = original.vertices[i];
        fixed[i] = input_points.contains({point[0], point[1]})
            || point[0] == minimum[0] || point[0] == maximum[0]
            || point[1] == minimum[1] || point[1] == maximum[1];
    }
    auto const on_segment = [](auto const& point, auto const& a, auto const& b) {
        double const dx = b[0] - a[0], dy = b[1] - a[1];
        double const length = std::hypot(dx, dy);
        if (length == 0.0) {
            return false;
        }
        double const projection = ((point[0] - a[0]) * dx + (point[1] - a[1]) * dy) / length;
        return projection >= -1e-8 && projection <= length + 1e-8
            && std::abs((point[0] - a[0]) * dy - (point[1] - a[1]) * dx) <= 1e-8 * length;
    };
    // Native radial intersections can have two equally near lattice choices.
    // Prefer a qualified exact-on-role choice before independent rounding can
    // turn a radial into an exterior edge and change its height ownership.
    for (uint32_t vertex = 0U; vertex < positions.size(); ++vertex) {
        if (fixed[vertex]) {
            continue;
        }
        auto const& native = original.vertices[vertex];
        std::vector<std::array<uint32_t, 2>> roles;
        for (auto const& radial : fan_radials) {
            if (on_segment(native, anchors[radial[0]], anchors[radial[1]])) {
                roles.push_back(radial);
            }
        }
        if (roles.empty()) {
            continue;
        }
        auto const on_roles = [&](std::array<double, 2> const point) {
            for (auto const& role : roles) {
                auto const& a = anchors[role[0]];
                auto const& b = anchors[role[1]];
                double const dx = b[0] - a[0], dy = b[1] - a[1];
                double const px = point[0] - a[0], py = point[1] - a[1];
                if (px * dy - py * dx != 0.0 || px * dx + py * dy < 0.0
                    || px * dx + py * dy > dx * dx + dy * dy) {
                    return false;
                }
            }
            return true;
        };
        if (on_roles({positions[vertex][0], positions[vertex][1]})) {
            continue;
        }
        std::array<double, 2> const xs{std::floor(native[0] * 32.0) / 32.0, std::ceil(native[0] * 32.0) / 32.0};
        std::array<double, 2> const ys{std::floor(native[1] * 32.0) / 32.0, std::ceil(native[1] * 32.0) / 32.0};
        std::optional<std::array<double, 2>> selected;
        double best_distance = std::numeric_limits<double>::infinity();
        bool best_input = false;
        for (double const x : xs) {
            for (double const y : ys) {
                std::array<double, 2> const candidate{x, y};
                double const dx = x - native[0], dy = y - native[1];
                double const distance = dx * dx + dy * dy;
                if (distance > MAXIMUM_DISPLACEMENT_SQUARED || !on_roles(candidate)) {
                    continue;
                }
                bool const input = input_points.contains(candidate);
                if ((x == minimum[0] || x == maximum[0] || y == minimum[1] || y == maximum[1]) && !input) {
                    continue;
                }
                if (!selected || distance < best_distance || (distance == best_distance
                    && ((input && !best_input) || (input == best_input && candidate < *selected)))) {
                    selected = candidate;
                    best_distance = distance;
                    best_input = input;
                }
            }
        }
        if (selected) {
            positions[vertex][0] = (*selected)[0];
            positions[vertex][1] = (*selected)[1];
            ++audit.radial_steiner_lattice_adjustment_count;
        }
    }
    remaining = classify();
    audit.role_aware_top_triangle_reversal_count += remaining;
    if (remaining == 0U) {
        return true;
    }
    std::map<std::array<uint32_t, 2>, bool> constraint_cache;
    auto const constrained = [&](uint32_t const a, uint32_t const b) {
        std::array<uint32_t, 2> const key{std::min(a, b), std::max(a, b)};
        auto const found = constraint_cache.find(key);
        if (found != constraint_cache.end()) {
            return found->second;
        }
        bool result = false;
        for (auto const& edge : constraints) {
            if (on_segment(original.vertices[a], anchors[edge[0]], anchors[edge[1]])
                && on_segment(original.vertices[b], anchors[edge[0]], anchors[edge[1]])) {
                result = true;
                break;
            }
        }
        constraint_cache.emplace(key, result);
        return result;
    };
    auto const on_grid_segment = [](auto const& point, auto const& a, auto const& b) {
        double const dx = b[0] - a[0], dy = b[1] - a[1];
        double const length_squared = dx * dx + dy * dy;
        double const along = (point[0] - a[0]) * dx + (point[1] - a[1]) * dy;
        return length_squared != 0.0 && along >= 0.0 && along <= length_squared
            && (point[0] - a[0]) * dy == (point[1] - a[1]) * dx;
    };
    auto const rebuild_renderer_pslg = [&] {
        auto const reject = [&](char const* const reason) {
            failure_reason = reason;
            return false;
        };
        struct GraphEdge final {
            uint32_t a;
            uint32_t b;
            std::vector<size_t> faces;
        };
        std::map<std::array<uint32_t, 2>, GraphEdge> old_edges;
        std::vector<std::set<size_t>> expected_roles(positions.size());
        std::vector<double> expected_sector_area(sector_seeds.size(), 0.0);
        for (size_t i = 0U; i < triangles.size(); ++i) {
            expected_sector_area[sector_ids[i]] += area(triangles[i]);
            for (size_t j = 0U; j < 3U; ++j) {
                uint32_t const a = triangles[i][j], b = triangles[i][(j + 1U) % 3U];
                expected_roles[a].insert(sector_ids[i]);
                auto const [edge, inserted] = old_edges.emplace(std::array<uint32_t, 2>{std::min(a, b), std::max(a,
                    b)}, GraphEdge{a, b, {}});
                static_cast<void>(inserted);
                edge->second.faces.push_back(i);
            }
        }
        std::map<std::array<double, 2>, std::vector<uint32_t>> coincident;
        for (uint32_t i = 0U; i < positions.size(); ++i) {
            coincident[{positions[i][0], positions[i][1]}].push_back(i);
        }
        std::vector<uint32_t> representative(positions.size());
        std::map<uint32_t, uint32_t> local_index;
        std::map<std::array<double, 2>, uint32_t> original_id;
        std::vector<std::array<double, 2>> points;
        std::set<uint32_t> representatives;
        for (auto const& [point, group] : coincident) {
            uint32_t selected = group.front();
            auto const input = std::find_if(group.begin(), group.end(),
                [&](uint32_t const vertex) { return fixed[vertex]; });
            if (input != group.end()) {
                selected = *input;
            }
            for (uint32_t const vertex : group) {
                representative[vertex] = selected;
            }
            original_id.emplace(point, selected);
            representatives.insert(selected);
            local_index.emplace(selected, static_cast<uint32_t>(points.size()));
            points.push_back(point);
        }
        struct Radial final {
            uint32_t a;
            uint32_t b;
            std::array<size_t, 2> owners;
            size_t left_owner;
        };
        std::vector<std::array<uint32_t, 2>> required, local_edges;
        std::vector<Radial> radials;
        std::map<std::array<uint32_t, 2>, size_t> old_perimeter;
        for (auto const& [key, edge] : old_edges) {
            if (edge.faces.empty() || edge.faces.size() > 2U) {
                return reject("nonmanifold native fan");
            }
            uint32_t const a = representative[edge.a], b = representative[edge.b];
            bool const boundary = edge.faces.size() == 1U;
            bool const barrier = !boundary && sector_ids[edge.faces[0]] != sector_ids[edge.faces[1]];
            bool const protected_edge = constrained(key[0], key[1]);
            if (barrier && !protected_edge) {
                return reject("native sector barrier is unconstrained");
            }
            if (boundary || protected_edge) {
                required.push_back({a, b});
                local_edges.push_back({local_index.at(a), local_index.at(b)});
            }
            if (boundary && a != b) {
                std::array<uint32_t, 2> const mapped{std::min(a, b), std::max(a, b)};
                auto const [found, inserted] = old_perimeter.emplace(mapped, sector_ids[edge.faces[0]]);
                if (!inserted && found->second != sector_ids[edge.faces[0]]) {
                    return reject("conflicting perimeter ownership");
                }
            }
            if (barrier) {
                std::array<size_t, 2> owners{sector_ids[edge.faces[0]], sector_ids[edge.faces[1]]};
                std::sort(owners.begin(), owners.end());
                radials.push_back({a, b, owners, sector_ids[edge.faces.front()]});
            }
        }
        auto const route_radial_prefixes = [&](std::vector<std::vector<uint32_t>>& paths) {
            if (anchors.empty()) {
                return false;
            }
            auto const center = original_id.find(anchors.front());
            if (center == original_id.end()) {
                return false;
            }
            auto const same_edge = [](auto const& edge, uint32_t const a, uint32_t const b) {
                return std::minmax(edge[0], edge[1]) == std::minmax(a, b);
            };
            auto const path_index = [&](uint32_t const a, uint32_t const b) -> std::optional<size_t> {
                auto const matches = [&](auto const& edge) { return same_edge(edge, a, b); };
                auto const first = std::ranges::find_if(required, matches);
                if (first == required.end() || std::find_if(first + 1, required.end(), matches) != required.end()) {
                    return std::nullopt;
                }
                return static_cast<size_t>(first - required.begin());
            };
            auto const on_perimeter = [&](uint32_t const id) {
                auto const& point = positions[id];
                return point[0] == minimum[0] || point[0] == maximum[0]
                    || point[1] == minimum[1] || point[1] == maximum[1];
            };
            auto const proper_crossing = [&](uint32_t const a, uint32_t const b, uint32_t const c, uint32_t const d) {
                auto const opposite = [](double const x, double const y) {
                    return (x > 0.0 && y < 0.0) || (x < 0.0 && y > 0.0);
                };
                return opposite(area({a, b, c}), area({a, b, d}))
                    && opposite(area({c, d, a}), area({c, d, b}));
            };
            struct Spoke final {
                uint32_t inner;
                uint32_t outer;
                size_t prefix;
                size_t suffix;
                std::array<size_t, 2> owners;
            };
            std::vector<Spoke> spokes;
            for (auto const& radial : radials) {
                bool const a_on_perimeter = on_perimeter(radial.a);
                if (a_on_perimeter == on_perimeter(radial.b)) {
                    continue;
                }
                uint32_t const inner = a_on_perimeter ? radial.b : radial.a;
                uint32_t const outer = a_on_perimeter ? radial.a : radial.b;
                auto const prefix = path_index(center->second, inner);
                auto const suffix = path_index(inner, outer);
                if (prefix && suffix && inner != center->second && std::ranges::any_of(radials, [&](auto const& r) {
                        return r.owners == radial.owners
                            && std::minmax(r.a, r.b) == std::minmax(center->second, inner);
                    })) {
                    spokes.push_back({inner, outer, *prefix, *suffix, radial.owners});
                }
            }
            auto const try_route = [&](Spoke const& first, Spoke const& second) {
                // Share the center-to-inner prefix and use the already protected
                // bridge before diverging to the original perimeter endpoints.
                std::vector<std::array<uint32_t, 2>> segments = required;
                segments[first.prefix] = {second.inner, center->second};
                segments[second.suffix] = {first.inner, second.outer};
                for (size_t i = 0U; i < segments.size(); ++i) {
                    for (size_t j = i + 1U; j < segments.size(); ++j) {
                        if (proper_crossing(segments[i][0], segments[i][1], segments[j][0], segments[j][1])) {
                            return false;
                        }
                    }
                }
                paths.clear();
                for (auto const& edge : local_edges) {
                    paths.push_back({edge[0], edge[1]});
                }
                paths[first.prefix].insert(paths[first.prefix].begin() + 1, local_index.at(second.inner));
                paths[second.suffix].insert(paths[second.suffix].begin() + 1, local_index.at(first.inner));
                return true;
            };
            for (size_t i = 0U; i < spokes.size(); ++i) {
                for (size_t j = i + 1U; j < spokes.size(); ++j) {
                    auto const& a = spokes[i];
                    auto const& b = spokes[j];
                    if (a.owners == b.owners || !proper_crossing(a.inner, a.outer, b.inner, b.outer)
                        || std::ranges::none_of(a.owners, [&](size_t const owner) {
                            return std::ranges::find(b.owners, owner) != b.owners.end();
                        })) {
                        continue;
                    }
                    double const dx = positions[a.inner][0] - positions[b.inner][0];
                    double const dy = positions[a.inner][1] - positions[b.inner][1];
                    if (dx * dx + dy * dy != 1.0 / (32.0 * 32.0) || !path_index(a.inner, b.inner)
                        || std::ranges::any_of(radials, [&](auto const& radial) {
                            return std::minmax(radial.a, radial.b) == std::minmax(a.inner, b.inner);
                        })) {
                        continue;
                    }
                    if (try_route(a, b) || try_route(b, a)) {
                        return true;
                    }
                }
            }
            return false;
        };
        RiverBedMesh rebuilt;
        try {
            // This second triangulation is geometry only. Its shared Q5 nodes
            // get canonical heights and separate sector attributes below.
            rebuilt = buildRiverBedMesh(minimum, maximum, points, local_edges, [](double, double) { return 0.0; });
        } catch (std::exception const& error) {
            failure_reason = error.what();
            return false;
        }
        std::vector<std::vector<uint32_t>> constraint_paths;
        bool const off_grid = std::any_of(rebuilt.vertices.begin(), rebuilt.vertices.end(), [](auto const& point) {
            return point[0] * 32.0 != std::round(point[0] * 32.0) || point[1] * 32.0 != std::round(point[1] * 32.0);
        });
        if (off_grid) {
            std::vector<std::vector<uint32_t>> local_paths;
            std::vector<detail::LatticeConstraintKind> constraint_kinds;
            constraint_kinds.reserve(local_edges.size());
            for (auto const& edge : required) {
                auto const& a = positions[edge[0]];
                auto const& b = positions[edge[1]];
                bool const boundary = (a[0] == b[0] && (a[0] == minimum[0] || a[0] == maximum[0]))
                    || (a[1] == b[1] && (a[1] == minimum[1] || a[1] == maximum[1]));
                bool const radial = std::ranges::any_of(radials, [&](auto const& r) {
                    return std::minmax(r.a, r.b) == std::minmax(edge[0], edge[1]);
                });
                constraint_kinds.push_back(boundary ? detail::LatticeConstraintKind::Boundary
                    : radial ? detail::LatticeConstraintKind::Radial : detail::LatticeConstraintKind::Interior);
            }
            if (constraint_kinds.size() != local_edges.size()) {
                return reject("renderer lattice edge ownership differs");
            }
            std::vector<std::array<double, 2>> added;
            bool const crossing_noded = detail::nodeRiverConstraintCrossingsImpl(
                points, local_edges, local_paths, constraint_kinds, &added);
            if (!crossing_noded) added.clear();
            if (crossing_noded || route_radial_prefixes(local_paths)) {
                std::vector<uint32_t> point_ids(points.size());
                for (auto const& [vertex, index] : local_index) {
                    point_ids[index] = vertex;
                }
                for (auto const& point : added) {
                    if (point[0] <= minimum[0] || point[0] >= maximum[0]
                        || point[1] <= minimum[1] || point[1] >= maximum[1]) {
                        return reject("renderer lattice crossing touches fan boundary");
                    }
                }
                for (auto const& point : added) {
                    uint32_t const id = static_cast<uint32_t>(positions.size());
                    uint32_t const index = static_cast<uint32_t>(points.size());
                    positions.push_back({point[0], point[1], 0.0});
                    representative.push_back(id);
                    representatives.insert(id);
                    expected_roles.emplace_back();
                    fixed.push_back(false);
                    original_id.emplace(point, id);
                    local_index.emplace(id, index);
                    points.push_back(point);
                    point_ids.push_back(id);
                }
                std::vector<std::array<uint32_t, 2>> noded_edges;
                for (auto const& path : local_paths) {
                    std::vector<uint32_t> mapped_path;
                    for (uint32_t const vertex : path) {
                        mapped_path.push_back(point_ids[vertex]);
                    }
                    constraint_paths.push_back(std::move(mapped_path));
                    for (size_t j = 1U; j < path.size(); ++j) {
                        noded_edges.push_back({path[j - 1U], path[j]});
                    }
                }
                std::vector<Radial> noded_radials;
                for (auto const& radial : radials) {
                    bool found = false;
                    for (size_t i = 0U; i < required.size(); ++i) {
                        if (std::minmax(required[i][0], required[i][1]) != std::minmax(radial.a, radial.b)) {
                            continue;
                        }
                        auto const& path = constraint_paths[i];
                        bool const forward = path.front() == radial.a;
                        size_t const right_owner = radial.owners[0] == radial.left_owner
                            ? radial.owners[1] : radial.owners[0];
                        double swept_area = 0.0;
                        auto const& origin = positions[radial.a];
                        for (size_t j = 1U; j < path.size(); ++j) {
                            auto const& a = positions[path[j - 1U]];
                            auto const& b = positions[path[j]];
                            swept_area += (a[0] - origin[0]) * (b[1] - origin[1]) - (a[1] - origin[1]) * (b[0] - origin[0]);
                            size_t const left_owner = forward ? radial.left_owner : right_owner;
                            noded_radials.push_back({path[j - 1U], path[j], radial.owners, left_owner});
                        }
                        // These are exact Q5 signed areas. The target follows
                        // the explicitly retained radial path, including its bend.
                        swept_area = forward ? swept_area : -swept_area;
                        expected_sector_area[radial.left_owner] += swept_area;
                        expected_sector_area[right_owner] -= swept_area;
                        found = true;
                        break;
                    }
                    if (!found) {
                        return reject("renderer lattice noding loses radial provenance");
                    }
                }
                radials = std::move(noded_radials);
                local_edges = std::move(noded_edges);
                try {
                    // One explicit lattice-path rebuild, not output-vertex snapping.
                    rebuilt = buildRiverBedMesh(minimum, maximum, points, local_edges,
                        [](double, double) { return 0.0; });
                } catch (std::exception const& error) {
                    failure_reason = error.what();
                    return false;
                }
            }
        }
        size_t const duplicate_count = positions.size() - points.size();
        std::vector<uint32_t> mapped_vertices;
        for (auto const& point : rebuilt.vertices) {
            if (point[0] * 32.0 != std::round(point[0] * 32.0) || point[1] * 32.0 != std::round(point[1] * 32.0)) {
                if (failure_capture != nullptr && claimRiverSceneFailureCapture()) {
                    failure_capture->capture_claimed = true;
                    try {
                        failure_capture->points = points;
                        failure_capture->edges = local_edges;
                        failure_capture->required = required;
                        failure_capture->representative = representative;
                        for (auto const& radial : radials) {
                            failure_capture->radials.push_back({radial.a, radial.b, radial.owners[0], radial.owners[1]});
                        }
                        failure_capture->rebuilt = rebuilt;
                    } catch (...) {
                        // Evidence allocation cannot replace the original geometry rejection.
                    }
                }
                return reject("renderer PSLG creates off-grid XY");
            }
            auto const found = original_id.find({point[0], point[1]});
            if (found == original_id.end()) {
                if (positions.size() >= std::numeric_limits<uint32_t>::max()
                    || !detail::isRiverConstraintIntersection(points, local_edges, {point[0], point[1]})) {
                    return reject("renderer PSLG creates new XY");
                }
                uint32_t const id = static_cast<uint32_t>(positions.size());
                positions.push_back(point);
                representative.push_back(id);
                representatives.insert(id);
                expected_roles.emplace_back();
                fixed.push_back(false);
                original_id.emplace(std::array<double, 2>{point[0], point[1]}, id);
                mapped_vertices.push_back(id);
                continue;
            }
            mapped_vertices.push_back(found->second);
        }
        std::vector<std::array<uint32_t, 3>> replacement;
        std::map<std::array<uint32_t, 2>, GraphEdge> new_edges;
        std::set<uint32_t> used;
        double coverage = 0.0;
        for (auto const& triangle : rebuilt.triangles) {
            std::array<uint32_t, 3> const mapped{mapped_vertices[triangle[0]], mapped_vertices[triangle[1]],
                mapped_vertices[triangle[2]]};
            if (area(mapped) <= 0.0) {
                return reject("nonpositive renderer PSLG face");
            }
            coverage += area(mapped);
            size_t const face = replacement.size();
            replacement.push_back(mapped);
            for (size_t i = 0U; i < 3U; ++i) {
                uint32_t const a = mapped[i], b = mapped[(i + 1U) % 3U];
                used.insert(a);
                auto const [edge, inserted] = new_edges.emplace(std::array<uint32_t, 2>{std::min(a, b), std::max(a,
                    b)}, GraphEdge{a, b, {}});
                static_cast<void>(inserted);
                edge->second.faces.push_back(face);
            }
        }
        double const full_area = 2.0 * (maximum[0] - minimum[0]) * (maximum[1] - minimum[1]);
        if (coverage != full_area || used != representatives) {
            return reject("renderer PSLG coverage or node loss");
        }
        std::map<uint32_t, std::vector<uint32_t>> adjacency;
        std::vector<std::vector<size_t>> neighbors(replacement.size());
        for (auto const& [key, edge] : new_edges) {
            adjacency[edge.a].push_back(edge.b);
            adjacency[edge.b].push_back(edge.a);
            if (edge.faces.size() > 2U) {
                return reject("nonmanifold renderer PSLG");
            }
            if (edge.faces.size() != 2U) {
                continue;
            }
            bool barrier = false;
            for (auto const& radial : radials) {
                if (on_grid_segment(positions[key[0]], positions[radial.a], positions[radial.b])
                    && on_grid_segment(positions[key[1]], positions[radial.a],
                        positions[radial.b])) {
                    barrier = true;
                    break;
                }
            }
            if (!barrier) {
                neighbors[edge.faces[0]].push_back(edge.faces[1]);
                neighbors[edge.faces[1]].push_back(edge.faces[0]);
            }
        }
        auto const chain_present = [&](uint32_t const a, uint32_t const b) {
            std::set<uint32_t> visited;
            std::vector<uint32_t> queue{a};
            while (!queue.empty()) {
                uint32_t const vertex = queue.back();
                queue.pop_back();
                if (!visited.insert(vertex).second) {
                    continue;
                }
                if (vertex == b) {
                    return true;
                }
                for (uint32_t const next : adjacency[vertex]) {
                    if (on_grid_segment(positions[next], positions[a], positions[b])) {
                        queue.push_back(next);
                    }
                }
            }
            return false;
        };
        for (size_t i = 0U; i < required.size(); ++i) {
            if (constraint_paths.empty()) {
                if (!chain_present(required[i][0], required[i][1])) {
                    return reject("renderer PSLG loses protected chain");
                }
                continue;
            }
            auto const& path = constraint_paths[i];
            for (size_t j = 1U; j < path.size(); ++j) {
                if (!chain_present(path[j - 1U], path[j])) {
                    return reject("renderer PSLG loses noded protected chain");
                }
            }
        }
        auto const grid = [](double const value) { return static_cast<int64_t>(std::llround(value * 32.0)); };
        std::vector<size_t> owners(replacement.size(), sector_seeds.size());
        for (size_t owner = 0U; owner < sector_seeds.size(); ++owner) {
            size_t seed_face = replacement.size();
            for (size_t i = 0U; i < replacement.size(); ++i) {
                bool contains = true;
                for (size_t j = 0U; j < 3U; ++j) {
                    auto const& a = positions[replacement[i][j]];
                    auto const& b = positions[replacement[i][(j + 1U) % 3U]];
                    int64_t const ax = 3 * grid(a[0]), ay = 3 * grid(a[1]);
                    int64_t const bx = 3 * grid(b[0]), by = 3 * grid(b[1]);
                    if ((bx - ax) * (sector_seeds[owner][1] - ay)
                        - (by - ay) * (sector_seeds[owner][0] - ax) < 0) {
                        contains = false;
                        break;
                    }
                }
                if (contains) {
                    seed_face = i;
                    break;
                }
            }
            if (seed_face == replacement.size()) {
                return reject("renderer PSLG loses sector seed");
            }
            std::vector<size_t> queue{seed_face};
            while (!queue.empty()) {
                size_t const face = queue.back();
                queue.pop_back();
                if (owners[face] == owner) {
                    continue;
                }
                if (owners[face] != sector_seeds.size()) {
                    return reject("renderer PSLG joins semantic sectors");
                }
                owners[face] = owner;
                queue.insert(queue.end(), neighbors[face].begin(), neighbors[face].end());
            }
        }
        std::map<std::array<uint32_t, 2>, size_t> new_perimeter;
        std::vector<std::set<size_t>> roles(positions.size());
        std::vector<double> sector_area(sector_seeds.size(), 0.0);
        for (size_t i = 0U; i < replacement.size(); ++i) {
            if (owners[i] == sector_seeds.size()) {
                return reject("renderer PSLG has unowned face");
            }
            sector_area[owners[i]] += area(replacement[i]);
            for (uint32_t const vertex : replacement[i]) {
                roles[vertex].insert(owners[i]);
            }
        }
        for (auto const& [key, edge] : new_edges) {
            if (edge.faces.size() == 1U) {
                new_perimeter.emplace(key, owners[edge.faces.front()]);
            }
        }
        if (new_perimeter.size() != old_perimeter.size()) {
            return reject("renderer PSLG changes outer boundary segmentation");
        }
        for (auto const& [edge, owner] : old_perimeter) {
            auto const found = new_perimeter.find(edge);
            if (found == new_perimeter.end()) {
                return reject("renderer PSLG changes outer boundary segmentation");
            }
            if (found->second != owner) {
                return reject("renderer PSLG changes outer boundary height ownership");
            }
        }
        if (sector_area != expected_sector_area) {
            return reject("renderer PSLG changes sector coverage");
        }
        std::map<std::array<uint32_t, 2>, std::vector<size_t>> owner_paths;
        std::vector<std::map<size_t, std::set<size_t>>> logical_roles(positions.size());
        for (auto const& [key, edge] : new_edges) {
            std::map<size_t, std::set<size_t>> adjacent;
            for (auto const& radial : radials) {
                if (radial.a == radial.b
                    || !on_grid_segment(positions[key[0]], positions[radial.a], positions[radial.b])
                    || !on_grid_segment(positions[key[1]], positions[radial.a], positions[radial.b])) {
                    continue;
                }
                adjacent[radial.owners[0]].insert(radial.owners[1]);
                adjacent[radial.owners[1]].insert(radial.owners[0]);
            }
            if (adjacent.empty()) {
                continue;
            }
            if (edge.faces.size() != 2U) {
                return reject("renderer PSLG opens radial barrier");
            }
            size_t const first_owner = owners[edge.faces[0]], last_owner = owners[edge.faces[1]];
            if (first_owner == last_owner || adjacent[first_owner].size() != 1U
                || adjacent[last_owner].size() != 1U) {
                return reject("renderer PSLG changes radial height ownership");
            }
            std::vector<size_t> path{first_owner};
            while (path.back() != last_owner) {
                auto const& next = adjacent[path.back()];
                if (next.empty() || next.size() > 2U) {
                    return reject("renderer PSLG branches semantic owner path");
                }
                auto found = std::find_if(next.begin(), next.end(), [&](size_t const owner) {
                    return path.size() == 1U || owner != path[path.size() - 2U];
                });
                if (found == next.end() || std::find(path.begin(), path.end(), *found) != path.end()) {
                    return reject("renderer PSLG cycles semantic owner path");
                }
                path.push_back(*found);
            }
            if (path.size() != adjacent.size()) {
                return reject("renderer PSLG loses semantic owner path");
            }
            if (path.size() > 2U) {
                for (uint32_t const vertex : key) {
                    std::array<double, 2> const xy{positions[vertex][0], positions[vertex][1]};
                    for (size_t j = 1U; j < path.size(); ++j) {
                        if (!equivalent_state(xy, path[j - 1U], path[j])) {
                            return reject("renderer PSLG collapses distinct emitted owner states");
                        }
                    }
                }
            }
            for (uint32_t const vertex : key) {
                for (size_t const owner : path) {
                    logical_roles[vertex][owner].insert(first_owner);
                    logical_roles[vertex][owner].insert(last_owner);
                }
            }
            owner_paths.emplace(key, std::move(path));
        }
        std::vector<std::array<uint64_t, 3>> role_quotients;
        for (uint32_t i = 0U; i < positions.size(); ++i) {
            uint32_t const vertex = representative[i];
            for (size_t const owner : expected_roles[i]) {
                if (roles[vertex].contains(owner)) {
                    continue;
                }
                if (!logical_roles[vertex].contains(owner)) {
                    return reject("renderer PSLG loses semantic vertex role");
                }
                std::array<double, 2> const xy{positions[vertex][0], positions[vertex][1]};
                auto const retained = std::find_if(roles[vertex].begin(), roles[vertex].end(),
                    [&](size_t const other) {
                        return logical_roles[vertex].at(owner).contains(other)
                            && equivalent_state(xy, owner, other);
                    });
                if (retained == roles[vertex].end()) {
                    return reject("renderer PSLG loses distinct emitted vertex state");
                }
                role_quotients.push_back({i, owner, *retained});
            }
        }
        if (char const* const evidence = std::getenv("MC_RIVER_SCENE_EVIDENCE");
            evidence != nullptr && !riverSceneFirstFailureOnly()) {
            std::ofstream dump(std::string(evidence) + ".fan-node-quotient-"
                + std::to_string(static_cast<int64_t>(minimum[0])) + "-"
                + std::to_string(static_cast<int64_t>(minimum[1])) + ".json");
            dump << std::setprecision(17) << "{\"old_face_count\":" << triangles.size() << ",\"new_face_count\":"
                << replacement.size()
                << ",\"mapping\":[";
            for (uint32_t i = 0U; i < positions.size(); ++i) {
                if (i != 0U) {
                    dump << ',';
                }
                dump << '[' << i << ',' << representative[i] << ',' << positions[i][0] << ',' << positions[i][1]
                    << ',' << (fixed[i] ? 1 : 0) << ']';
            }
            auto const write_triangles = [&](auto const& faces) {
                for (size_t i = 0U; i < faces.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << '[' << faces[i][0] << ',' << faces[i][1] << ',' << faces[i][2] << ']';
                }
            };
            auto const write_owners = [&](auto const& values) {
                for (size_t i = 0U; i < values.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << values[i];
                }
            };
            dump << "],\"old_triangles\":[";
            write_triangles(triangles);
            dump << "],\"old_owners\":[";
            write_owners(sector_ids);
            dump << "],\"new_triangles\":[";
            write_triangles(replacement);
            dump << "],\"new_owners\":[";
            write_owners(owners);
            dump << "],\"required_chains\":[";
            for (size_t i = 0U; i < required.size(); ++i) {
                if (i != 0U) {
                    dump << ',';
                }
                dump << '[' << required[i][0] << ',' << required[i][1] << ']';
            }
            dump << "],\"renoded_constraint_paths\":[";
            for (size_t i = 0U; i < constraint_paths.size(); ++i) {
                if (i != 0U) {
                    dump << ',';
                }
                dump << '[';
                for (size_t j = 0U; j < constraint_paths[i].size(); ++j) {
                    if (j != 0U) {
                        dump << ',';
                    }
                    dump << constraint_paths[i][j];
                }
                dump << ']';
            }
            dump << "],\"semantic_owner_paths\":[";
            bool first_path = true;
            for (auto const& [edge, path] : owner_paths) {
                if (!first_path) {
                    dump << ',';
                }
                first_path = false;
                dump << '[' << edge[0] << ',' << edge[1] << ",[";
                for (size_t i = 0U; i < path.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << path[i];
                }
                dump << "]]";
            }
            dump << "],\"role_quotients\":[";
            for (size_t i = 0U; i < role_quotients.size(); ++i) {
                if (i != 0U) {
                    dump << ',';
                }
                auto const& q = role_quotients[i];
                dump << '[' << q[0] << ',' << q[1] << ',' << q[2] << ']';
            }
            dump << "],\"expected_sector_double_area\":[";
            for (size_t i = 0U; i < expected_sector_area.size(); ++i) {
                if (i != 0U) {
                    dump << ',';
                }
                dump << expected_sector_area[i];
            }
            dump << "]}\n";
        }
        ++audit.constrained_cavity_retriangulation_count;
        audit.cavity_retriangulated_triangle_count += triangles.size();
        audit.cavity_output_triangle_count += replacement.size();
        audit.duplicate_renderer_node_quotient_count += duplicate_count;
        audit.top_face_count_change += static_cast<int64_t>(replacement.size())
            - static_cast<int64_t>(triangles.size());
        triangles = std::move(replacement);
        sector_ids = std::move(owners);
        return true;
    };
    struct Assignment final {
        uint32_t vertex;
        std::array<double, 2> point;
        size_t gain;
        double distance_squared;
    };
    // Each accepted assignment/flip strictly shrinks this set. The maximum
    // number of successful steps is therefore the initial reversal count.
    while (remaining != 0U) {
        std::vector<std::vector<size_t>> incident(positions.size());
        for (size_t i = 0U; i < triangles.size(); ++i) {
            for (uint32_t const vertex : triangles[i]) {
                incident[vertex].push_back(i);
            }
        }
        auto const count_candidate_gain = [&](uint32_t const vertex) {
            size_t gain = 0U;
            bool valid = true;
            for (size_t const face : incident[vertex]) {
                bool const new_negative = area(triangles[face]) < 0.0;
                if (new_negative && !negative[face]) {
                    valid = false;
                    break;
                }
                gain += negative[face] && !new_negative ? 1U : 0U;
            }
            return std::pair{gain, valid};
        };
        std::optional<Assignment> best;
        for (size_t i = 0U; i < triangles.size(); ++i) {
            if (!negative[i]) {
                continue;
            }
            for (uint32_t const vertex : triangles[i]) {
                if (fixed[vertex]) {
                    continue;
                }
                auto const& native = original.vertices[vertex];
                std::array<double, 2> const xs{std::floor(native[0] * 32.0) / 32.0, std::ceil(native[0] * 32.0) / 32.0};
                std::array<double, 2> const ys{std::floor(native[1] * 32.0) / 32.0, std::ceil(native[1] * 32.0) / 32.0};
                auto const saved = positions[vertex];
                for (double const x : xs) {
                    for (double const y : ys) {
                        // Boundary nodes are shared with another fan; an
                        // internal repair cannot add a new perimeter node.
                        if (x <= minimum[0] || x >= maximum[0] || y <= minimum[1] || y >= maximum[1]
                            || (x == saved[0] && y == saved[1])) {
                            continue;
                        }
                        double const dx = x - native[0], dy = y - native[1];
                        double const distance_squared = dx * dx + dy * dy;
                        if (distance_squared > MAXIMUM_DISPLACEMENT_SQUARED) {
                            continue;
                        }
                        positions[vertex][0] = x;
                        positions[vertex][1] = y;
                        auto const [gain, valid] = count_candidate_gain(vertex);
                        positions[vertex] = saved;
                        if (!valid || gain == 0U) {
                            continue;
                        }
                        Assignment const candidate{vertex, {x, y}, gain, distance_squared};
                        if (!best || candidate.gain > best->gain
                            || (candidate.gain == best->gain && (candidate.distance_squared < best->distance_squared
                            || (candidate.distance_squared == best->distance_squared
                            && (candidate.vertex < best->vertex
                            || (candidate.vertex == best->vertex && candidate.point < best->point)))))) {
                            best = candidate;
                        }
                    }
                }
            }
        }
        if (best) {
            positions[best->vertex][0] = best->point[0];
            positions[best->vertex][1] = best->point[1];
            ++audit.steiner_node_reassignment_count;
        } else {
            bool flipped = false;
            for (size_t i = 0U; i < triangles.size() && !flipped; ++i) {
                if (!negative[i]) {
                    continue;
                }
                auto const triangle = triangles[i];
                for (size_t edge = 0U; edge < 3U && !flipped; ++edge) {
                    uint32_t const a = triangle[edge], b = triangle[(edge + 1U) % 3U], c = triangle[(edge + 2U) % 3U];
                    if (constrained(a, b)) {
                        continue;
                    }
                    for (size_t const other : incident[a]) {
                        if (other == i || sector_ids[other] != sector_ids[i]
                            || std::find(triangles[other].begin(), triangles[other].end(),
                            b) == triangles[other].end()) {
                            continue;
                        }
                        bool opposite_edge = false;
                        for (size_t j = 0U; j < 3U; ++j) {
                            opposite_edge = opposite_edge || (triangles[other][j] == b
                            && triangles[other][(j + 1U) % 3U] == a);
                        }
                        if (!opposite_edge) {
                            continue;
                        }
                        auto const opposite = std::find_if(triangles[other].begin(), triangles[other].end(),
                            [&](uint32_t const vertex) { return vertex != a && vertex != b; });
                        if (opposite == triangles[other].end() || *opposite == c) {
                            continue;
                        }
                        auto const has_opposite = [&](size_t const face) {
                            auto const& triangle = triangles[face];
                            return std::ranges::find(triangle, *opposite) != triangle.end();
                        };
                        bool const diagonal_exists = std::ranges::any_of(incident[c], has_opposite);
                        if (diagonal_exists) {
                            continue;
                        }
                        std::array<uint32_t, 3> const first{c, a, *opposite}, second{*opposite, b, c};
                        if (area(first) <= 0.0 || area(second) <= 0.0) {
                            continue;
                        }
                        triangles[i] = first;
                        triangles[other] = second;
                        ++audit.unconstrained_edge_flip_count;
                        flipped = true;
                        break;
                    }
                }
            }
            if (!flipped) {
                size_t const prior_size = positions.size();
                if (!rebuild_renderer_pslg()) {
                    positions.resize(prior_size);
                    return false;
                }
            }
        }
        size_t const next = classify();
        if (next >= remaining) {
            throw std::logic_error("river Q5 repair did not reduce reversals");
        }
        remaining = next;
    }
    // Protected chains retain their mapped geometry. Each semantic sector
    // keeps its own attributes at a shared grid node; retriangulation changes
    // unconstrained diagonals and explicitly recorded pixel equivalences.
    return true;
}

} // namespace detail

namespace {

// Keep the original fan radials in the planar topology. Authority segments may
// assign different heights to the same XY; those values belong to sectors.
void emitRiverFan(
    std::vector<StoneFaceInstance>& faces,
    std::array<double, 2> const minimum,
    std::array<double, 2> const maximum,
    std::array<double, 2> const center,
    std::span<RiverFanSector const> const sectors,
    river_field_candidate::RiverConstraintExport const& constraints,
    generation::GlobalAtlas const& atlas,
    shared::TerrainWorld const& world,
    std::unordered_map<uint64_t, shared::Block>& bank_materials,
    RiverFanQueryMemo* const query_memo,
    RiverFanQueryMemo const* const predecessor_query_memo,
    bool const combine_height_and_wet,
    int32_t& minimum_height,
    int32_t& maximum_height,
    TerrainLodRiverGeometryAudit& audit)
{
    char const* const evidence = std::getenv("MC_RIVER_SCENE_EVIDENCE");
    bool const census = riverSceneDiagnosticCensus();
    ++audit.inspected_fan_count;
    if (census && audit.inspected_fan_count % 4'096U == 0U) {
        std::clog << "river scene census: inspected " << audit.inspected_fan_count
            << ", unresolved " << audit.diagnostic_failed_fan_count << '\n';
    }
    bool const capture_failure = evidence != nullptr && evidence[0] != '\0';
    std::vector<std::array<double, 2>> anchors{center};
    std::vector<std::array<uint32_t, 2>> edges;
    std::vector<std::array<uint32_t, 2>> fan_radials;
    std::vector<std::array<double, 2>> original_anchors;
    // Source kind0=fan radial,1=fan perimeter,2=river polyline; followed by
    // original sector/polyline index and original polyline segment index.
    std::vector<std::array<uint64_t, 3>> edge_sources;
    bool course_input = false;
    if (capture_failure) {
        original_anchors.push_back(center);
    }
    auto const add_edge = [&](std::array<double, 2> a, std::array<double, 2> b,
                              std::array<uint64_t, 3> const source) {
        course_input = course_input || source[0] == 2U;
        auto const original_a = a, original_b = b;
        // All consumers use the same Q5 points before noding. This avoids a
        // different subpixel endpoint on opposite sides of a coarse boundary.
        for (auto* point : {&a, &b}) {
            for (double& component : *point) {
                component = std::round(component * 32.0) / 32.0;
            }
        }
        if (a == b) {
            // A tangent/collapsed segment can still supply the shared boundary
            // node required by the fan on the other side of the perimeter.
            anchors.push_back(a);
            if (capture_failure) {
                original_anchors.push_back(original_a);
            }
            return;
        }
        uint32_t const first = static_cast<uint32_t>(anchors.size());
        anchors.push_back(a);
        anchors.push_back(b);
        edges.push_back({first, first + 1U});
        if (source[0] == 0U) {
            fan_radials.push_back({first, first + 1U});
        }
        if (capture_failure) {
            original_anchors.push_back(original_a);
            original_anchors.push_back(original_b);
            edge_sources.push_back(source);
        }
    };
    constexpr double INSET_WEIGHT = 0.75;
    auto const boundary_step = [&](size_t const right) {
        auto const& a = sectors[(right + sectors.size() - 1U) % sectors.size()];
        auto const& b = sectors[right];
        return maximum[0] - minimum[0] == 2.0 && maximum[1] - minimum[1] == 2.0
            && (a.authority_height || b.authority_height) && a.authority_height != b.authority_height;
    };
    for (size_t i = 0U; i < sectors.size(); ++i) {
        auto const& point = sectors[i].a;
        if (boundary_step(i)) {
            std::array<double, 2> const inset{std::lerp(center[0], point[0], INSET_WEIGHT),
                std::lerp(center[1], point[1], INSET_WEIGHT)};
            add_edge(center, inset, {0U, i, 0U});
            add_edge(inset, point, {0U, i, 1U});
        } else {
            add_edge(center, point, {0U, i, 0U});
        }
        add_edge(point, sectors[i].b, {1U, i, 0U});
    }
    for (size_t line_index = 0U; line_index < constraints.polylines.size(); ++line_index) {
        auto const& line = constraints.polylines[line_index];
        for (size_t i = 1U; i < line.vertices.size(); ++i) {
            auto const& av = line.vertices[i - 1U].native.xy;
            auto const& bv = line.vertices[i].native.xy;
            // Choose the regional representable rail first, then clip it.
            // Clipping native coordinates first can snap an interior endpoint
            // onto this perimeter without exposing it to the neighboring fan.
            std::array<double, 2> const a{std::round(av.x_m * 32.0) / 32.0, std::round(av.y_m * 32.0) / 32.0};
            std::array<double, 2> const b{std::round(bv.x_m * 32.0) / 32.0, std::round(bv.y_m * 32.0) / 32.0};
            audit.maximum_xy_quantization_error_m = std::max({audit.maximum_xy_quantization_error_m,
                std::hypot(a[0] - av.x_m, a[1] - av.y_m), std::hypot(b[0] - bv.x_m, b[1] - bv.y_m)});
            std::array<double, 2> const delta{b[0] - a[0], b[1] - a[1]};
            double lo = 0.0, hi = 1.0;
            for (size_t axis = 0U; axis < 2U && lo <= hi; ++axis) {
                if (delta[axis] == 0.0) {
                    if (a[axis] < minimum[axis] || a[axis] > maximum[axis]) {
                        hi = -1.0;
                    }
                } else {
                    double t0 = (minimum[axis] - a[axis]) / delta[axis];
                    double t1 = (maximum[axis] - a[axis]) / delta[axis];
                    if (t0 > t1) {
                        std::swap(t0, t1);
                    }
                    lo = std::max(lo, t0);
                    hi = std::min(hi, t1);
                }
            }
            if (lo <= hi) {
                add_edge({a[0] + lo * delta[0], a[1] + lo * delta[1]},
                {a[0] + hi * delta[0], a[1] + hi * delta[1]}, {2U, line_index, i - 1U});
            }
        }
    }
    auto const base_height = [&](double const x, double const y) {
        if (combine_height_and_wet && query_memo != nullptr) {
            return query_memo->sampleHeightAndWet(x, y, atlas, predecessor_query_memo);
        }
        auto const query = [&] { return generation::v10L0ElevationAt(atlas, x, y) + 513.0; };
        return query_memo == nullptr ? query() : query_memo->sample(
            x, y, &RiverFanQueryMemo::Entry::height, predecessor_query_memo, query);
    };
    RiverBedMesh mesh;
    try {
        // Discontinuous radial state can emit bridges whose material depends
        // on CDT's owner traversal order. Keep that path unchanged; equal
        // endpoint state proves continuity along each affine radial correction.
        bool continuous_radials = !course_input;
        for (size_t i = 0U; i < sectors.size() && continuous_radials; ++i) {
            auto const& a = sectors[i];
            auto const& b = sectors[(i + 1U) % sectors.size()];
            continuous_radials = a.correction[0] == b.correction[0] && a.materials[0] == b.materials[0]
                && a.materials[2] == b.materials[1] && a.authority_height == b.authority_height
                && (a.authority_height || a.correction[2] == b.correction[1]);
        }
        auto direct = continuous_radials
            ? detail::tryBuildRiverStarMesh(minimum, maximum, anchors, edges, base_height)
            : std::nullopt;
        mesh = direct ? std::move(*direct) : buildRiverBedMesh(minimum, maximum, anchors, edges, base_height);
    } catch (std::exception const& error) {
        std::string_view const message{error.what()};
        if (capture_failure && message.starts_with("Intersection of constraint edges")
            && message.find("computed split vertex is invalid") != std::string_view::npos
            && claimRiverSceneFailureCapture()) {
            try {
                std::ofstream dump(std::string(evidence) + ".failed-graph.json");
                dump << std::setprecision(17) << "{\"schema\":\"terrain-river-fan-failure-v1\",\"crop_min\":["
                    << minimum[0] << ',' << minimum[1] << "],\"crop_max\":[" << maximum[0] << ',' << maximum[1]
                    << "],\"center\":[" << center[0] << ',' << center[1] << "],\"anchors\":[";
                auto const write_points = [&](std::span<std::array<double, 2> const> const points) {
                    for (size_t i = 0U; i < points.size(); ++i) {
                        if (i != 0U) {
                            dump << ',';
                        }
                        dump << '[' << points[i][0] << ',' << points[i][1] << ']';
                    }
                };
                write_points(anchors);
                dump << "],\"original_anchors_before_q5\":[";
                write_points(original_anchors);
                dump << "],\"edges\":[";
                for (size_t i = 0U; i < edges.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << '[' << edges[i][0] << ',' << edges[i][1] << ']';
                }
                dump << "],\"edge_source_kinds\":[\"fan_radial\",\"fan_perimeter\",\"river_polyline\"],\"edge_sources\":[";
                for (size_t i = 0U; i < edge_sources.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << '[' << edge_sources[i][0] << ',' << edge_sources[i][1] << ',' << edge_sources[i][2] << ']';
                }
                dump << "],\"sectors\":[";
                for (size_t i = 0U; i < sectors.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    auto const& sector = sectors[i];
                    dump << "{\"index\":" << i << ",\"a\":[" << sector.a[0] << ',' << sector.a[1]
                        << "],\"b\":[" << sector.b[0] << ',' << sector.b[1] << "],\"correction\":["
                        << sector.correction[0] << ',' << sector.correction[1] << ',' << sector.correction[2]
                        << "],\"authority_height\":";
                    if (sector.authority_height) {
                        dump << *sector.authority_height;
                    }
                    else {
                        dump << "null";
                    }
                    dump << '}';
                }
                dump << "],\"polylines\":[";
                for (size_t i = 0U; i < constraints.polylines.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    auto const& line = constraints.polylines[i];
                    dump << "{\"index\":" << i << ",\"feature_id\":" << std::quoted(line.feature_id)
                        << ",\"reach_index\":" << line.reach_index << ",\"course_index\":" << line.course_index
                        << ",\"role\":" << static_cast<uint32_t>(line.role) << '}';
                }
                dump << "]}\n";
            } catch (...) {
                // Optional evidence must not replace the original triangulation error.
            }
        }
        throw;
    }
    auto const weights = [&](RiverFanSector const& sector, double const x, double const y) {
        double const ax = sector.a[0] - center[0], ay = sector.a[1] - center[1];
        double const bx = sector.b[0] - center[0], by = sector.b[1] - center[1];
        double const area = ax * by - ay * bx;
        double const u = ((x - center[0]) * by - (y - center[1]) * bx) / area;
        double const v = (ax * (y - center[1]) - ay * (x - center[0])) / area;
        return std::array<double, 3>{1.0 - u - v, u, v};
    };
    // The utility samples exact original XY. The renderer has a separate Q5
    // representation, so resample changed positions explicitly before packing.
    audit.original_top_triangle_count += mesh.triangles.size();
    std::vector<size_t> render_sector_ids;
    for (auto const& triangle : mesh.triangles) {
        double x = 0.0, y = 0.0;
        for (uint32_t const vertex : triangle) {
            x += mesh.vertices[vertex][0] / 3.0;
            y += mesh.vertices[vertex][1] / 3.0;
        }
        size_t owner = 0U;
        for (; owner < sectors.size(); ++owner) {
            auto const w = weights(sectors[owner], x, y);
            if (*std::min_element(w.begin(), w.end()) >= -1e-8) {
                break;
            }
        }
        if (owner == sectors.size()) {
            throw std::runtime_error("river mesh triangle escaped fan sectors");
        }
        render_sector_ids.push_back(owner);
    }
    auto render_vertices = mesh.vertices;
    for (auto& point : render_vertices) {
        point[0] = std::round(point[0] * 32.0) / 32.0;
        point[1] = std::round(point[1] * 32.0) / 32.0;
    }
    auto render_triangles = mesh.triangles;
    std::vector<std::array<int64_t, 2>> sector_seeds;
    for (auto const& sector : sectors) {
        sector_seeds.push_back({std::llround((center[0] + sector.a[0] + sector.b[0]) * 32.0),
            std::llround((center[1] + sector.a[1] + sector.b[1]) * 32.0)});
    }
    auto const evaluate_sector_vertex = [&](std::array<double, 3> value, size_t const sector_id) {
        auto const& sector = sectors[sector_id];
        auto const w = weights(sector, value[0], value[1]);
        if (sector.authority_height) {
            value[2] += w[0] * sector.correction[0];
            double const edge_weight = w[1] + w[2];
            if (edge_weight > 0.0) {
                double const edge_x = (w[1] * sector.a[0] + w[2] * sector.b[0]) / edge_weight;
                double const edge_y = (w[1] * sector.a[1] + w[2] * sector.b[1]) / edge_weight;
                // A newly noded river crossing must meet the entire constant
                // authority segment, including nonlinear bed between endpoints.
                double const edge_base = base_height(edge_x, edge_y);
                value[2] += edge_weight * (*sector.authority_height - edge_base);
            }
        } else {
            for (size_t j = 0U; j < 3U; ++j) {
                value[2] += w[j] * sector.correction[j];
            }
        }
        value[2] = std::clamp(value[2], 1.0, double(shared::WorldExtent::DEPTH));
        return value;
    };
    auto const evaluate_vertex = [&](std::array<double, 3> const value, size_t const sector_id) {
        auto corrected = evaluate_sector_vertex(value, sector_id);
        auto const w = weights(sectors[sector_id], value[0], value[1]);
        if (w[0] <= 1e-9 || w[0] >= 1.0 - 1e-9) {
            return corrected;
        }
        size_t const next = (sector_id + 1U) % sectors.size();
        size_t const previous = (sector_id + sectors.size() - 1U) % sectors.size();
        size_t other = sector_id;
        if (std::abs(w[2]) <= 1e-9 && boundary_step(sector_id)) {
            other = previous;
        } else if (std::abs(w[1]) <= 1e-9 && boundary_step(next)) {
            other = next;
        }
        if (other != sector_id) {
            double const common = (corrected[2] + evaluate_sector_vertex(value, other)[2]) * 0.5;
            double const blend = std::clamp(w[0] / (1.0 - INSET_WEIGHT), 0.0, 1.0);
            corrected[2] = blend == 1.0 ? common : std::lerp(corrected[2], common, blend);
        }
        return corrected;
    };
    // LOD sector colors can misclassify a wet channel; keep actual-column edge colors authoritative.
    std::map<std::array<int64_t, 2>, bool> wet_positive_v10_vertices;
    auto const evaluate_material = [&](std::array<double, 2> const point, size_t const sector_id) {
        auto const& sector = sectors[sector_id];
        auto const sector_weights = weights(sector, point[0], point[1]);
        shared::Block const selected = detail::selectRiverSectorMaterial(sector_weights, sector.materials);
        if (atlas.configuration.model_tag != generation::TerrainModelTag::V10
            || (sector.authority_height && sector_weights[0] <= 1e-9)) {
            return selected;
        }

        std::array<int64_t, 2> const key{
            std::llround(point[0] * 32.0), std::llround(point[1] * 32.0)};
        auto found = wet_positive_v10_vertices.find(key);
        if (found == wet_positive_v10_vertices.end()) {
            auto const query = [&] {
                auto const water = generation::sampleV10WaterGeometry(atlas, point[0], point[1], 3U);
                return (water.ocean || water.lake || water.river) && water.elevation_m > 0.0;
            };
            bool const wet_positive = query_memo == nullptr ? query()
                : query_memo->sample(point[0], point[1], &RiverFanQueryMemo::Entry::wet_positive,
                    predecessor_query_memo, query);
            found = wet_positive_v10_vertices.emplace(key, wet_positive).first;
        }
        if (found->second || selected != shared::Block::Dirt) {
            return found->second ? shared::Block::Dirt : selected;
        }
        // Dry vertices can inherit Dirt from a wet sector sample. Use the
        // material of the actual column containing the vertex in that case.
        uint32_t const x = static_cast<uint32_t>(static_cast<int64_t>(std::floor(point[0]))) % generation::WORLD_SIZE_BLOCKS;
        uint32_t const y = static_cast<uint32_t>(static_cast<int64_t>(std::floor(point[1]))) % generation::WORLD_SIZE_BLOCKS;
        uint64_t const column_key = (static_cast<uint64_t>(x) << 32U) | y;
        auto material = bank_materials.find(column_key);
        if (material == bank_materials.end()) {
            material = bank_materials.emplace(column_key, world.sampleV10TopMaterial(x, y)).first;
        }
        return material->second;
    };
    auto const equivalent_state = [&](std::array<double, 2> const xy, size_t const a, size_t const b) {
        std::array<double, 3> const base{xy[0], xy[1], base_height(xy[0], xy[1])};
        return evaluate_vertex(base, a) == evaluate_vertex(base, b)
            && evaluate_material(xy, a) == evaluate_material(xy, b);
    };
    std::string repair_failure;
    detail::RiverPslgFailure pslg_failure;
    bool const render_repaired = detail::repairRiverFanQuantization(mesh, anchors, edges, fan_radials, minimum, maximum,
        render_vertices, render_triangles, render_sector_ids, sector_seeds, equivalent_state, audit, repair_failure,
        capture_failure ? &pslg_failure : nullptr);
    detail::resampleRiverRenderHeights(mesh.vertices, render_vertices, base_height, audit);
    auto const vertex = [&](uint32_t const id, size_t const owner) {
        auto const value = evaluate_vertex(render_vertices[id], owner);
        minimum_height = std::min(minimum_height, static_cast<int32_t>(std::floor(value[2] * 32.0)));
        maximum_height = std::max(maximum_height, static_cast<int32_t>(std::ceil(value[2] * 32.0)));
        return value;
    };
    auto const material = [&](uint32_t const id, size_t const owner) {
        return evaluate_material({render_vertices[id][0], render_vertices[id][1]}, owner);
    };
    struct EdgeOwner final {
        size_t sector;
        uint32_t a;
        uint32_t b;
    };
    bool const track_bridge_owners = std::ranges::any_of(sectors, [](RiverFanSector const& sector) {
        return sector.authority_height || sector.correction != std::array<double, 3>{};
    });
    std::map<std::array<uint32_t, 2>, EdgeOwner> owners;
    for (size_t face_index = 0U; face_index < render_triangles.size(); ++face_index) {
        auto const& triangle = render_triangles[face_index];
        auto const& pa = render_vertices[triangle[0]];
        auto const& pb = render_vertices[triangle[1]];
        auto const& pc = render_vertices[triangle[2]];
        double const snapped_area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
        if (capture_failure && snapped_area < 0.0 && audit.snapped_top_triangle_reversal_count == 0U
            && (pslg_failure.capture_claimed || claimRiverSceneFailureCapture())) {
            try {
                std::ofstream dump(std::string(evidence) + ".reversed-fan-"
                    + std::to_string(static_cast<int64_t>(minimum[0])) + "-"
                    + std::to_string(static_cast<int64_t>(minimum[1])) + ".json");
                dump << std::setprecision(17);
                auto const write_row = [&](auto const& row) {
                    dump << '[';
                    for (size_t j = 0U; j < row.size(); ++j) {
                        if (j != 0U) {
                            dump << ',';
                        }
                        dump << row[j];
                    }
                    dump << ']';
                };
                auto const write_matrix = [&](auto const& matrix) {
                    dump << '[';
                    for (size_t i = 0U; i < matrix.size(); ++i) {
                        if (i != 0U) {
                            dump << ',';
                        }
                        write_row(matrix[i]);
                    }
                    dump << ']';
                };
                dump << "{\"crop_min\":[" << minimum[0] << ',' << minimum[1]
                    << "],\"crop_max\":[" << maximum[0] << ',' << maximum[1]
                    << "],\"center\":[" << center[0] << ',' << center[1] << "],\"anchors\":";
                write_matrix(anchors);
                dump << ",\"original_anchors_before_q5\":";
                write_matrix(original_anchors);
                dump << ",\"edges\":";
                write_matrix(edges);
                dump << ",\"edge_sources\":";
                write_matrix(edge_sources);
                dump << ",\"mesh_vertices\":";
                write_matrix(mesh.vertices);
                dump << ",\"render_vertices\":";
                write_matrix(render_vertices);
                dump << ",\"triangles\":";
                write_matrix(mesh.triangles);
                dump << ",\"render_triangles\":";
                write_matrix(render_triangles);
                dump << ",\"render_sector_ids\":[";
                for (size_t i = 0U; i < render_sector_ids.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << render_sector_ids[i];
                }
                dump << ']';
                std::string reason = repair_failure;
                std::replace(reason.begin(), reason.end(), '\n', ' ');
                std::replace(reason.begin(), reason.end(), '\r', ' ');
                std::replace(reason.begin(), reason.end(), '\t', ' ');
                dump << ",\"failure_reason\":" << std::quoted(reason);
                dump << ",\"sector_seeds_q5_times_three\":";
                write_matrix(sector_seeds);
                dump << ",\"fan_radials\":";
                write_matrix(fan_radials);
                dump << ",\"renderer_pslg_input_points\":";
                write_matrix(pslg_failure.points);
                dump << ",\"renderer_pslg_input_edges\":";
                write_matrix(pslg_failure.edges);
                dump << ",\"renderer_pslg_required_representative_chains\":";
                write_matrix(pslg_failure.required);
                dump << ",\"renderer_pslg_radial_owners\":";
                write_matrix(pslg_failure.radials);
                dump << ",\"renderer_pslg_output_vertices\":";
                write_matrix(pslg_failure.rebuilt.vertices);
                dump << ",\"renderer_pslg_output_triangles\":";
                write_matrix(pslg_failure.rebuilt.triangles);
                auto const write_bits = [&](auto const& matrix) {
                    dump << '[';
                    bool first = true;
                    for (auto const& point : matrix) {
                        if (!first) {
                            dump << ',';
                        }
                        first = false;
                        dump << '[' << std::bit_cast<uint64_t>(point[0]) << ',' << std::bit_cast<uint64_t>(point[1]) << ']';
                    }
                    dump << ']';
                };
                dump << ",\"native_xy_double_bits\":";
                write_bits(mesh.vertices);
                dump << ",\"render_xy_double_bits\":";
                write_bits(render_vertices);
                dump << ",\"renderer_pslg_input_xy_double_bits\":";
                write_bits(pslg_failure.points);
                dump << ",\"renderer_pslg_output_xy_double_bits\":";
                write_bits(pslg_failure.rebuilt.vertices);
                dump << ",\"renderer_pslg_representative\":[";
                for (size_t i = 0U; i < pslg_failure.representative.size(); ++i) {
                    if (i != 0U) {
                        dump << ',';
                    }
                    dump << pslg_failure.representative[i];
                }
                dump << "],\"off_grid_output_xy_and_nearest_delta\":[";
                bool first_off_grid = true;
                for (auto const& point : pslg_failure.rebuilt.vertices) {
                    double const dx = point[0] - std::round(point[0] * 32.0) / 32.0;
                    double const dy = point[1] - std::round(point[1] * 32.0) / 32.0;
                    if (dx == 0.0 && dy == 0.0) {
                        continue;
                    }
                    if (!first_off_grid) {
                        dump << ',';
                    }
                    first_off_grid = false;
                    dump << '[' << point[0] << ',' << point[1] << ',' << dx << ',' << dy << ']';
                }
                dump << "],\"numeric_contract\":{\"xy_scale\":32,\"maximum_displacement_squared\":"
                    << 2.0 / (64.0 * 64.0) << ",\"double_digits\":" << std::numeric_limits<double>::digits
                    << ",\"long_double_digits\":" << std::numeric_limits<long double>::digits << '}';
                dump << ",\"repair_complete\":" << (render_repaired ? "true" : "false");
                dump << ",\"first_reversed_triangle\":[" << triangle[0] << ',' << triangle[1] << ',' << triangle[2]
                    << "]}\n";
            } catch (...) {
                // Optional evidence must never replace the original preparation error.
            }
        }
        if (snapped_area < 0.0) {
            ++audit.diagnostic_failed_fan_count;
            if (census) {
                if (capture_failure) {
                    std::string reason = repair_failure;
                    std::replace(reason.begin(), reason.end(), '\n', ' ');
                    std::replace(reason.begin(), reason.end(), '\r', ' ');
                    std::replace(reason.begin(), reason.end(), '\t', ' ');
                    std::ofstream index(std::string(evidence) + ".census-failures.jsonl", std::ios::app);
                    index << "{\"crop_min\":[" << minimum[0] << ',' << minimum[1]
                        << "],\"crop_max\":[" << maximum[0] << ',' << maximum[1]
                        << "],\"guard\":" << std::quoted(reason) << "}\n";
                }
                return;
            }
            throw std::runtime_error("river Q5 fan has unresolved reversals at "
                + std::to_string(minimum[0]) + "," + std::to_string(minimum[1]) + " " + repair_failure);
        }
        ++audit.generated_top_triangle_count;
        audit.snapped_top_triangle_reversal_count += snapped_area < 0.0 ? 1U : 0U;
        audit.snapped_top_triangle_collapse_count += snapped_area == 0.0 ? 1U : 0U;
        size_t const sector_id = render_sector_ids[face_index];
        // A wet corner can tint the dry side of a bank triangle.
        std::array materials{
            material(triangle[0], sector_id), material(triangle[1], sector_id), material(triangle[2], sector_id)};
        if (atlas.configuration.model_tag == generation::TerrainModelTag::V10
            && !sectors[sector_id].authority_height
            && std::ranges::find(materials, shared::Block::Dirt) != materials.end()) {
            auto const dry = std::ranges::find_if(materials, [](shared::Block block) {
                return block == shared::Block::Grass || block == shared::Block::Snow;
            });
            if (dry != materials.end() && std::ranges::all_of(materials, [bank = *dry](shared::Block block) {
                    return block == bank || block == shared::Block::Dirt;
                })) {
                auto const water = generation::sampleV10WaterGeometry(atlas,
                    (pa[0] + pb[0] + pc[0]) / 3.0, (pa[1] + pb[1] + pc[1]) / 3.0, 3U);
                materials.fill(water.river ? shared::Block::Dirt : *dry);
            }
        }
        faces.push_back(StoneFaceInstance::fromMetricTerrainMaterialTriangle(
            {vertex(triangle[0], sector_id), vertex(triangle[1], sector_id), vertex(triangle[2], sector_id)},
            materials));
        // Without per-sector height changes, adjacent triangles share their edge heights.
        if (!track_bridge_owners) {
            continue;
        }
        for (size_t i = 0U; i < 3U; ++i) {
            uint32_t const a = triangle[i], b = triangle[(i + 1U) % 3U];
            std::array<uint32_t, 2> const key{std::min(a, b), std::max(a, b)};
            auto const [owner, inserted] = owners.emplace(key, EdgeOwner{sector_id, a, b});
            if (inserted || owner->second.sector == sector_id) {
                continue;
            }
            auto const a0 = vertex(a, owner->second.sector), b0 = vertex(b, owner->second.sector);
            auto const a1 = vertex(a, sector_id), b1 = vertex(b, sector_id);
            if (std::lround(a0[2] * 32.0) == std::lround(a1[2] * 32.0)
                && std::lround(b0[2] * 32.0) == std::lround(b1[2] * 32.0)) {
                continue;
            }
            // The strip is double sided, matching the previous radial bridge.
            std::array<std::array<double, 3>, 3> const first{a0, b0, b1}, second{a0, b1, a1};
            for (auto const& strip : {first, second}) {
                auto const fixed_point = [](std::array<double, 3> const& point) {
                    return std::array<long, 3>{std::lround(point[0] * 32.0),
                        std::lround(point[1] * 32.0), std::lround(point[2] * 32.0)};
                };
                auto const p0 = fixed_point(strip[0]), p1 = fixed_point(strip[1]), p2 = fixed_point(strip[2]);
                if (p0 == p1 || p1 == p2 || p2 == p0) {
                    continue;
                }
                auto const m = material(a, sector_id);
                faces.push_back(StoneFaceInstance::fromMetricTerrainMaterialTriangle(strip, {m, m, m}));
                faces.push_back(StoneFaceInstance::fromMetricTerrainMaterialTriangle({strip[0], strip[2], strip[1]},
                    {m, m, m}));
                audit.emitted_bridge_triangle_count += 2U;
            }
        }
    }
}


static constexpr uint32_t PERIOD = generation::WORLD_SIZE_BLOCKS;
static constexpr uint32_t VERTICAL_CHUNK_COUNT = shared::WorldExtent::DEPTH / shared::Chunk::SIDE_LENGTH;

struct Cell final {
    TerrainLodRegion region;
    uint32_t extent;
    shared::Block material;
    std::array<int32_t, 4> corner_heights{};
    std::array<int32_t, 4> corner_temperatures{};
    std::array<shared::Block, 4> corner_materials{};
};

shared::BlockCoordinate neighborFaceCoordinate(uint32_t const edge, uint32_t const u, uint32_t const v)
{
    shared::BlockCoordinate local{};
    if (edge < 2U) {
        local = {
            .x = static_cast<uint8_t>(edge == 0U ? 15U : 0U),
            .y = static_cast<uint8_t>(u),
            .z = static_cast<uint8_t>(v)};
    } else if (edge < 4U) {
        local = {
            .x = static_cast<uint8_t>(u),
            .y = static_cast<uint8_t>(edge == 2U ? 15U : 0U),
            .z = static_cast<uint8_t>(v)};
    } else {
        local = {
            .x = static_cast<uint8_t>(u),
            .y = static_cast<uint8_t>(v),
            .z = static_cast<uint8_t>(edge == 4U ? 15U : 0U)};
    }
    return local;
}

uint64_t cellKey(TerrainLodRegion const region)
{
    return (static_cast<uint64_t>(region.level) << 48U) | (static_cast<uint64_t>(region.x) << 24U) | region.y;
}

uint32_t wrap(int64_t const value)
{
    return static_cast<uint32_t>((value % PERIOD + PERIOD) % PERIOD);
}

std::optional<shared::Chunk> currentChunkSnapshot(
    shared::SparseWorld const& world,
    shared::ChunkCoordinate const coordinate
)
{
    if (shared::Chunk const* const resident = world.residentChunk(coordinate)) {
        return *resident;
    }
    return world.effectiveChunkSnapshot(coordinate);
}

bool addChunkSource(
    TerrainLodColumnInput& column,
    shared::SparseWorld const& world,
    shared::ChunkCoordinate const coordinate,
    shared::Chunk const& chunk
)
{
    TerrainLodChunkSource const source{
        .coordinate = coordinate,
        .identity = chunk.contentIdentity(),
        .edit_revision = world.editChunkRevision(coordinate),
    };
    for (TerrainLodChunkSource const& existing : column.sources) {
        if (existing.coordinate == coordinate) {
            return existing == source;
        }
    }
    column.sources.push_back(source);
    return true;
}

shared::ChunkCoordinate adjacentChunkCoordinate(
    shared::ChunkCoordinate const coordinate,
    uint32_t const edge
)
{
    auto const normal = shared::faceNormal(static_cast<shared::FaceDirection>(edge));
    uint32_t const chunk_period = PERIOD / 16U;
    return {
        .x = static_cast<int32_t>((static_cast<int64_t>(coordinate.x) + normal[0] + chunk_period) % chunk_period),
        .y = static_cast<int32_t>((static_cast<int64_t>(coordinate.y) + normal[1] + chunk_period) % chunk_period),
        .z = coordinate.z + normal[2],
    };
}

bool captureNeighborFace(
    TerrainLodColumnInput& column,
    shared::SparseWorld const& world,
    shared::ChunkCoordinate const coordinate,
    uint32_t const edge,
    shared::ChunkNeighborFaces& neighbors
)
{
    std::array<shared::Block, shared::Chunk::FACE_BLOCK_COUNT> blocks;
    blocks.fill(shared::Block::Air);
    shared::ChunkCoordinate const adjacent = adjacentChunkCoordinate(coordinate, edge);
    if (adjacent.z >= 0 && adjacent.z < static_cast<int32_t>(VERTICAL_CHUNK_COUNT)) {
        auto neighbor = currentChunkSnapshot(world, adjacent);
        if (!neighbor || !addChunkSource(column, world, adjacent, *neighbor)) {
            return false;
        }
        for (uint32_t v = 0U; v < 16U; ++v) {
            for (uint32_t u = 0U; u < 16U; ++u) {
                auto const block = neighbor->blockAt(neighborFaceCoordinate(edge, u, v));
                if (!block) {
                    return false;
                }
                blocks[v * 16U + u] = *block;
            }
        }
    }
    neighbors.faces[edge] = blocks;
    return true;
}

bool captureEditedChunks(
    TerrainLodColumnInput& column,
    shared::SparseWorld const& world
)
{
    for (uint32_t z = 0U; z < VERTICAL_CHUNK_COUNT; ++z) {
        shared::ChunkCoordinate const coordinate{
            .x = static_cast<int32_t>(column.x),
            .y = static_cast<int32_t>(column.y),
            .z = static_cast<int32_t>(z),
        };
        if (world.editChunkRevision(coordinate) == 0U) {
            continue;
        }
        auto chunk = currentChunkSnapshot(world, coordinate);
        if (!chunk || !addChunkSource(column, world, coordinate, *chunk)) {
            return false;
        }
        shared::ChunkNeighborFaces neighbors;
        for (uint32_t edge = 0U; edge < 6U; ++edge) {
            if (!captureNeighborFace(column, world, coordinate, edge, neighbors)) {
                return false;
            }
        }
        column.chunks.push_back({.chunk = std::move(*chunk), .neighbors = std::move(neighbors)});
    }
    return true;
}

bool chunkSourceCurrent(TerrainLodChunkSource const& source, shared::SparseWorld const& world)
{
    if (world.editChunkRevision(source.coordinate) != source.edit_revision) {
        return false;
    }
    shared::SparseBlockQuery const probe = world.queryBlock({
        static_cast<int64_t>(source.coordinate.x) * shared::Chunk::SIDE_LENGTH,
        static_cast<int64_t>(source.coordinate.y) * shared::Chunk::SIDE_LENGTH,
        static_cast<int64_t>(source.coordinate.z) * shared::Chunk::SIDE_LENGTH,
    });
    if (probe.state != shared::GenerationState::Materialized || !probe.block) {
        return false;
    }
    auto const current = currentChunkSnapshot(world, source.coordinate);
    return current && current->contentIdentity() == source.identity;
}

bool editSourceCurrent(TerrainLodEditSource const& source, shared::SparseWorld const& world)
{
    return world.editChunkRevision(source.coordinate) == source.revision;
}

void captureEditSources(TerrainLodColumnInput& column, shared::SparseWorld const& world)
{
    static constexpr std::array OFFSETS{
        std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1},
    };
    // Include absent chunks: a sparse edit can change one before it becomes resident.
    column.edit_sources.reserve(OFFSETS.size() * VERTICAL_CHUNK_COUNT);
    for (auto const offset : OFFSETS) {
        shared::ChunkCoordinate const column_coordinate{
            .x = static_cast<int32_t>(wrap(
                (static_cast<int64_t>(column.x) + offset[0]) * shared::Chunk::SIDE_LENGTH)
                / shared::Chunk::SIDE_LENGTH),
            .y = static_cast<int32_t>(wrap(
                (static_cast<int64_t>(column.y) + offset[1]) * shared::Chunk::SIDE_LENGTH)
                / shared::Chunk::SIDE_LENGTH),
            .z = 0,
        };
        shared::ChunkCoordinate chunk_coordinate = column_coordinate;
        for (uint32_t z = 0U; z < VERTICAL_CHUNK_COUNT; ++z) {
            chunk_coordinate.z = static_cast<int32_t>(z);
            column.edit_sources.push_back({chunk_coordinate, world.editChunkRevision(chunk_coordinate)});
        }
    }
}

void appendFallbackEditSources(TerrainLodScene& scene, TerrainLodColumnInput const& column)
{
    if (column.authority_sources.empty()) {
        scene.edit_sources.insert(scene.edit_sources.end(), column.edit_sources.begin(), column.edit_sources.end());
    }
}

void normalizeTerrainLodEditSources(std::vector<TerrainLodEditSource>& sources)
{
    std::ranges::sort(sources, [](TerrainLodEditSource const& left, TerrainLodEditSource const& right) {
        if (left.coordinate.x != right.coordinate.x) {
            return left.coordinate.x < right.coordinate.x;
        }
        if (left.coordinate.y != right.coordinate.y) {
            return left.coordinate.y < right.coordinate.y;
        }
        if (left.coordinate.z != right.coordinate.z) {
            return left.coordinate.z < right.coordinate.z;
        }
        return left.revision < right.revision;
    });
    // Keep differing revisions so source validation can reject mixed captures.
    auto const unique_end = std::ranges::unique(sources, [](TerrainLodEditSource const& left,
        TerrainLodEditSource const& right) {
        return left.coordinate == right.coordinate && left.revision == right.revision;
    });
    sources.erase(unique_end.begin(), unique_end.end());
}

struct CachedAuthoritativeTileVersion final {
    uint64_t pass = 0U;
    std::optional<shared::AuthoritativeTileVersionState> state;
};

std::unordered_map<uint64_t, CachedAuthoritativeTileVersion>& authoritativeTileVersionCache()
{
    static thread_local std::unordered_map<uint64_t, CachedAuthoritativeTileVersion> cache;
    return cache;
}

uint64_t& authoritativeTileVersionCachePass()
{
    static thread_local uint64_t pass = 0U;
    return pass;
}

} // namespace

namespace detail {

double terrainLodChildHeightError(
    std::array<int32_t, 4> const& corners, shared::TerrainLodArtifact const& child)
{
    double maximum = 0.0;
    int32_t const center = (corners[0] + corners[1] + corners[2] + corners[3]) / 4;
    for (uint32_t row = 0U; row < child.side; ++row) {
        for (uint32_t col = 0U; col < child.side; ++col) {
            double const dx = static_cast<double>(col * child.spacing) / child.extent - 0.5;
            double const dy = static_cast<double>(row * child.spacing) / child.extent - 0.5;
            double const t = 2.0 * std::max(std::abs(dx), std::abs(dy));
            double height = center;
            if (t > 0.0) {
                bool const vertical = std::abs(dx) > std::abs(dy);
                double const fraction = 0.5 + (vertical ? dy : dx) / t;
                double const a = vertical ? corners[dx > 0.0 ? 1U : 0U] : corners[dy > 0.0 ? 2U : 0U];
                double const b = vertical ? corners[dx > 0.0 ? 3U : 2U] : corners[dy > 0.0 ? 3U : 1U];
                height += (a + (b - a) * fraction - height) * t;
            }
            double const actual = (child.nodes[row * child.side + col].sample.height + 1.0)
                * StoneFaceInstance::TERRAIN_HEIGHT_SCALE;
            maximum = std::max(maximum, std::abs(actual - height));
        }
    }
    return maximum / StoneFaceInstance::TERRAIN_HEIGHT_SCALE;
}

} // namespace detail

namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double distanceToCell(uint32_t const origin, uint32_t const extent, uint32_t const center)
{
    double const midpoint = static_cast<double>(origin) + static_cast<double>(extent) * 0.5;
    double distance = std::abs(midpoint - center);
    distance = std::min(distance, static_cast<double>(PERIOD) - distance);
    return std::max(0.0, distance - static_cast<double>(extent) * 0.5);
}

void collectCells(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& artifact,
    TerrainLodRegion const region,
    uint32_t const root_extent,
    uint32_t const center_x,
    uint32_t const center_y,
    std::vector<Cell>& cells,
    std::unordered_set<uint64_t> const& exact_refinement,
    std::stop_token const& cancellation)
{
    if (cancellation.stop_requested()) {
        throw std::runtime_error("terrain LOD scene cancelled");
    }
    uint32_t const extent = terrainLodRegionExtent(region.level, root_extent);
    uint32_t const origin_x = region.x * extent;
    uint32_t const origin_y = region.y * extent;
    double const distance =
        std::hypot(distanceToCell(origin_x, extent, center_x), distanceToCell(origin_y, extent, center_y));
    double const threshold = region.level == shared::TerrainLodLevel::L0
        ? 1'024.0
        : static_cast<double>(shared::ACTUAL_TERRAIN_RADIUS_BLOCKS + 32U);
    bool refine = distance < threshold || exact_refinement.contains(cellKey(region));
    std::shared_ptr<shared::TerrainLodArtifact const> child_artifact;
    if (!refine && region.level == shared::TerrainLodLevel::L1
        && world.atlas()->configuration.model_tag == generation::TerrainModelTag::V10
        && world.atlas()->configuration.use_v10_fine_terrain) {
        child_artifact = shared::buildTerrainLod(
            world, shared::TerrainLodLevel::L2, origin_x, origin_y, extent, cancellation);
        std::array<int32_t, 4> corners{};
        for (uint32_t corner = 0U; corner < 4U; ++corner) {
            auto const sample = shared::sampleTerrainLod(artifact,
                origin_x + (corner % 2U) * extent, origin_y + (corner / 2U) * extent);
            corners[corner] = static_cast<int32_t>(std::lround(
                (sample.height + 1.0) * StoneFaceInstance::TERRAIN_HEIGHT_SCALE));
        }
        // Compare against the emitted center fan, not bilinear interpolation.
        // Reuse these canonical child samples if one voxel cannot bound its error.
        static constexpr double MAX_COARSE_HEIGHT_ERROR_BLOCKS = 1.0;
        refine = detail::terrainLodChildHeightError(corners, *child_artifact) > MAX_COARSE_HEIGHT_ERROR_BLOCKS;
    }
    if (region.level < shared::TerrainLodLevel::L2 && refine) {
        auto const next_level = static_cast<shared::TerrainLodLevel>(static_cast<uint32_t>(region.level) + 1U);
        if (!child_artifact) {
            child_artifact = shared::buildTerrainLod(world, next_level, origin_x, origin_y, extent, cancellation);
        }
        for (TerrainLodRegion const child : terrainLodChildren(region, root_extent)) {
            collectCells(world, *child_artifact, child, root_extent, center_x, center_y, cells, exact_refinement,
                cancellation);
        }
        return;
    }
    shared::TerrainLodSample const sample =
        shared::sampleTerrainLod(artifact, origin_x + extent * 0.5, origin_y + extent * 0.5);
    Cell cell{
        .region = region,
        .extent = extent,
        .material = sample.top_material,
    };
    for (uint32_t corner = 0U; corner < 4U; ++corner) {
        uint32_t const corner_x = corner % 2U;
        uint32_t const corner_y = corner / 2U;
        auto const node = shared::sampleTerrainLod(
            artifact,
            static_cast<double>(origin_x + corner_x * extent),
            static_cast<double>(origin_y + corner_y * extent)
        );
        cell.corner_heights[corner] =
            static_cast<int32_t>(std::lround((node.height + 1.0) * StoneFaceInstance::TERRAIN_HEIGHT_SCALE));
        cell.corner_temperatures[corner] = static_cast<int32_t>(std::lround(node.temperature * 32.0));
        cell.corner_materials[corner] = node.top_material;
    }
    cells.push_back(cell);
}

std::vector<StoneFaceInstance> meshAuthorityProfiles(
    TerrainLodColumnInput const& column,
    std::stop_token const& cancellation
)
{
    std::vector<StoneFaceInstance> faces;
    faces.reserve(512U);
    shared::DenseSectionMask full_chunk_mask;
    for (TerrainLodChunkInput const& input : column.chunks) {
        shared::ChunkCoordinate const coordinate = input.chunk.coordinate();
        if (coordinate.x == static_cast<int32_t>(column.x)
            && coordinate.y == static_cast<int32_t>(column.y)
            && coordinate.z >= 0 && coordinate.z < static_cast<int32_t>(VERTICAL_CHUNK_COUNT)) {
            full_chunk_mask.set(static_cast<uint32_t>(coordinate.z));
        }
    }
    auto const emit =
        [&](
            uint32_t x,
            uint32_t y,
            uint32_t z,
            uint32_t direction,
            shared::Block material,
            uint32_t height = 1U,
            uint32_t width = 1U,
            uint32_t side_flags = 0U
        ) {
            faces.push_back({
                .x = static_cast<int32_t>(column.x * 16U + x),
                .y = static_cast<int32_t>(column.y * 16U + y),
                .z = static_cast<int32_t>(z),
                .direction = direction,
                .u_extent = width,
                .v_extent = height,
                .material_id = static_cast<uint32_t>(material),
                .reserved_1 = side_flags,
            });
        };
    bool const merged_bottom = std::ranges::all_of(column.authority_profiles, [](auto const& profile) {
        return profile.dense_section_mask.none();
    });
    if (merged_bottom) {
        emit(0U, 0U, 0U, 4U, shared::Block::Bedrock, 16U, 16U);
    }
    static constexpr std::array<int32_t, 4> OFFSETS{-1, 1, -18, 18};
    for (uint32_t y = 0U; y < 16U; ++y) {
        for (uint32_t x = 0U; x < 16U; ++x) {
            if (cancellation.stop_requested()) {
                throw std::runtime_error("authoritative profile meshing cancelled");
            }
            uint32_t const index = (y + 1U) * 18U + x + 1U;
            auto const& own = column.authority_profiles[index];
            uint32_t const top = own.base_highest_solid_z;
            uint32_t bank_start = top;
            // Only the two natural Dirt steps immediately beneath an intact Grass top qualify.
            if (top >= 2U && own.top_material == shared::Block::Grass
                && own.ground_material == shared::Block::Dirt
                && !own.dense_section_mask.test(top / 16U)
                && !own.dense_section_mask.test((top - 2U) / 16U)
                && !full_chunk_mask.test(top / 16U)) {
                bank_start = top - 2U;
            }
            auto const grassBankFlag = [bank_start, top](uint32_t const z, shared::Block const material) {
                return z >= bank_start && z < top && material == shared::Block::Dirt
                    ? StoneFaceInstance::VOXEL_GRASS_BANK : 0U;
            };
            shared::DenseSectionMask scan_mask = own.dense_section_mask;
            for (int32_t const offset : OFFSETS) {
                scan_mask |= column.authority_profiles[static_cast<uint32_t>(static_cast<int32_t>(index) + offset)]
                            .dense_section_mask;
            }
            scan_mask |= (scan_mask << 1U) | (scan_mask >> 1U);
            scan_mask |= full_chunk_mask;
            uint32_t const scan_section_count = scan_mask.any() ? VERTICAL_CHUNK_COUNT : 0U;
            for (uint32_t section = 0U; section < scan_section_count; ++section) {
                if (!scan_mask.test(section) || full_chunk_mask.test(section)) {
                    continue;
                }
                for (uint32_t z = section * 16U; z < (section + 1U) * 16U; ++z) {
                    auto const material = own.materialAt(static_cast<uint16_t>(z));
                    if (!shared::isSolidBlock(material)) {
                        continue;
                    }
                    if (z == 0U || !shared::isSolidBlock(own.materialAt(static_cast<uint16_t>(z - 1U)))) {
                        emit(x, y, static_cast<uint16_t>(z), 4U, material);
                    }
                    if (z == shared::WorldExtent::DEPTH - 1U
                        || !shared::isSolidBlock(own.materialAt(static_cast<uint16_t>(z + 1U)))) {
                        emit(x, y, static_cast<uint16_t>(z), 5U, material);
                    }
                    for (uint32_t direction = 0U; direction < 4U; ++direction) {
                        auto const& neighbor = column.authority_profiles[static_cast<uint32_t>(
                            static_cast<int32_t>(index) + OFFSETS[direction])];
                        if (!shared::isSolidBlock(neighbor.materialAt(static_cast<uint16_t>(z)))) {
                            uint32_t const flag = z > neighbor.base_highest_solid_z ? grassBankFlag(z, material) : 0U;
                            emit(x, y, z, direction, material, 1U, 1U, flag);
                        }
                    }
                }
            }
            if (!scan_mask.test(own.base_highest_solid_z / shared::Chunk::SIDE_LENGTH)) {
                emit(x, y, own.base_highest_solid_z, 5U, own.materialAt(own.base_highest_solid_z));
            }
            if (!merged_bottom && !scan_mask.test(0U)) {
                emit(x, y, 0U, 4U, own.materialAt(0U));
            }
            for (uint32_t direction = 0U; direction < 4U; ++direction) {
                auto const& neighbor =
                    column.authority_profiles[static_cast<uint32_t>(static_cast<int32_t>(index) + OFFSETS[direction])];
                uint32_t const first = neighbor.base_highest_solid_z + 1U;
                uint32_t const end = own.base_highest_solid_z + 1U;
                if (first >= end) {
                    continue;
                }
                std::array<uint32_t, 5> cuts{
                    first,
                    end,
                    std::clamp<uint32_t>(own.ground_first_z, first, end),
                    std::clamp<uint32_t>(own.ground_end_z, first, end),
                    std::clamp<uint32_t>(own.base_highest_solid_z, first, end),
                };
                std::sort(cuts.begin(), cuts.end());
                for (uint32_t cut = 1U; cut < cuts.size(); ++cut) {
                    if (cuts[cut] == cuts[cut - 1U]) {
                        continue;
                    }
                    uint32_t cursor = cuts[cut - 1U];
                    while (cursor < cuts[cut]) {
                        uint32_t end_run = scan_mask.none() ? cuts[cut] : std::min(cuts[cut], (cursor / 16U + 1U) * 16U);
                        auto const material = own.materialAt(static_cast<uint16_t>(cursor));
                        if (cursor < bank_start && bank_start < end_run && material == shared::Block::Dirt) {
                            end_run = bank_start;
                        }
                        if (!scan_mask.test(cursor / shared::Chunk::SIDE_LENGTH)) {
                            auto const bank_flag = grassBankFlag(cursor, material);
                            emit(x, y, cursor, direction, material, end_run - cursor, 1U, bank_flag);
                        }
                        cursor = end_run;
                    }
                }
            }
        }
    }
    return faces;
}

} // namespace

std::optional<TerrainLodColumnInput>
captureTerrainLodColumn(shared::SparseWorld const& world, uint32_t const x, uint32_t const y)
{
    if (x >= PERIOD / 16U || y >= PERIOD / 16U) {
        return std::nullopt;
    }
    TerrainLodColumnInput result{
        .x = x,
        .y = y,
        .chunks = {},
        .sources = {},
        .authority_profiles = {},
        .authority_sources = {},
        .edge_heights = {},
    };
    if (world.authoritativeColumn(static_cast<int64_t>(x) * 16, static_cast<int64_t>(y) * 16)) {
        result.authority_profiles.resize(static_cast<uint64_t>(18U) * 18U);
        result.authority_sources.reserve(5U);
        for (auto const offset :
             {std::array{0, 0}, std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
            shared::ChunkCoordinate const coordinate{
                .x = static_cast<int32_t>(wrap(
                    static_cast<int64_t>(x) * 16 + static_cast<int64_t>(offset[0]) * 16
                ) / 16U),
                .y = static_cast<int32_t>(wrap(
                    static_cast<int64_t>(y) * 16 + static_cast<int64_t>(offset[1]) * 16
                ) / 16U),
                .z = 0,
            };
            auto const identity = world.authoritativeTileIdentity(coordinate);
            if (!identity) {
                return std::nullopt;
            }
            result.authority_sources.push_back({
                coordinate,
                identity->identity,
                identity->revision,
                identity->content_hash,
                identity->dense_content_hash,
            });
        }
        captureEditSources(result, world);
        for (uint32_t row = 0U; row < 18U; ++row) {
            for (uint32_t col = 0U; col < 18U; ++col) {
                if ((row == 0U || row == 17U) && (col == 0U || col == 17U)) {
                    continue;
                }
                uint32_t const wx = wrap(static_cast<int64_t>(x * 16U + col) - 1);
                uint32_t const wy = wrap(static_cast<int64_t>(y * 16U + row) - 1);
                auto profile = world.authoritativeColumn(wx, wy);
                if (!profile) {
                    return std::nullopt;
                }
                result.authority_profiles[row * 18U + col] = std::move(*profile);
            }
        }
        for (uint32_t along = 0U; along < 16U; ++along) {
            std::array const edge_indices{(along + 1U) * 18U + 1U, (along + 1U) * 18U + 16U,
                18U + along + 1U, 16U * 18U + along + 1U};
            for (size_t edge = 0U; edge < edge_indices.size(); ++edge) {
                auto const& profile = result.authority_profiles[edge_indices[edge]];
                result.edge_heights[edge][along] = profile.highest_solid_z + 1U;
                result.edge_materials[edge][along] = profile.materialAt(profile.highest_solid_z);
            }
        }
        if (!captureEditedChunks(result, world)) {
            return std::nullopt;
        }
        return result;
    }
    captureEditSources(result, world);
    std::array<bool, VERTICAL_CHUNK_COUNT> surface_chunks{};
    for (uint32_t local_y = 0U; local_y < 16U; ++local_y) {
        for (uint32_t local_x = 0U; local_x < 16U; ++local_x) {
            auto const surface = world.verifiedSurfaceColumn(x * 16U + local_x, y * 16U + local_y);
            if (!surface) {
                return std::nullopt;
            }
            surface_chunks[surface->highest_solid_z / 16U] = true;
            uint16_t const height = static_cast<uint16_t>(surface->highest_solid_z + 1U);
            if (local_x == 0U) {
                result.edge_heights[0U][local_y] = height;
                result.edge_materials[0U][local_y] = surface->material_at_highest_solid;
            }
            if (local_x == 15U) {
                result.edge_heights[1U][local_y] = height;
                result.edge_materials[1U][local_y] = surface->material_at_highest_solid;
            }
            if (local_y == 0U) {
                result.edge_heights[2U][local_x] = height;
                result.edge_materials[2U][local_x] = surface->material_at_highest_solid;
            }
            if (local_y == 15U) {
                result.edge_heights[3U][local_x] = height;
                result.edge_materials[3U][local_x] = surface->material_at_highest_solid;
            }
        }
    }
    auto const first_surface = std::find(surface_chunks.begin(), surface_chunks.end(), true);
    auto const last_surface = std::find(surface_chunks.rbegin(), surface_chunks.rend(), true);
    uint32_t const first_z = static_cast<uint32_t>(first_surface - surface_chunks.begin());
    uint32_t const end_z = VERTICAL_CHUNK_COUNT - static_cast<uint32_t>(last_surface - surface_chunks.rbegin());
    for (uint32_t z = first_z; z < end_z; ++z) {
        surface_chunks[z] = true;
    }
    for (uint32_t z = 0U; z < VERTICAL_CHUNK_COUNT; ++z) {
        shared::ChunkCoordinate const coordinate{
            .x = static_cast<int32_t>(x),
            .y = static_cast<int32_t>(y),
            .z = static_cast<int32_t>(z),
        };
        shared::Chunk const* const chunk = world.residentChunk(coordinate);
        if (chunk == nullptr) {
            if (surface_chunks[z]) {
                return std::nullopt;
            }
            continue;
        }
        shared::ChunkNeighborFaces neighbors;
        std::vector<TerrainLodChunkSource> sources;
        bool complete = true;
        for (uint32_t edge = 0U; edge < 6U; ++edge) {
            auto const normal = shared::faceNormal(static_cast<shared::FaceDirection>(edge));
            shared::ChunkCoordinate const adjacent{
                .x = static_cast<int32_t>((static_cast<int64_t>(x) + normal[0] + PERIOD / 16U) % (PERIOD / 16U)),
                .y = static_cast<int32_t>((static_cast<int64_t>(y) + normal[1] + PERIOD / 16U) % (PERIOD / 16U)),
                .z = coordinate.z + normal[2],
            };
            std::array<shared::Block, 256> blocks;
            blocks.fill(shared::Block::Air);
            if (adjacent.z >= 0 && adjacent.z < static_cast<int32_t>(VERTICAL_CHUNK_COUNT)) {
                shared::Chunk const* const neighbor = world.residentChunk(adjacent);
                if (neighbor == nullptr) {
                    if (surface_chunks[z]) {
                        complete = false;
                        break;
                    }
                    // Unknown neighbors of lower resident chunks expose conservative
                    // closing faces.
                    neighbors.faces[edge] = blocks;
                    continue;
                }
                sources.push_back({
                    .coordinate = adjacent,
                    .identity = neighbor->contentIdentity(),
                    .edit_revision = world.editChunkRevision(adjacent),
                });
                for (uint32_t v = 0U; v < 16U; ++v) {
                    for (uint32_t u = 0U; u < 16U; ++u) {
                        shared::BlockCoordinate const local = neighborFaceCoordinate(edge, u, v);
                        auto const block = neighbor->blockAt(local);
                        if (!block.has_value()) {
                            return std::nullopt;
                        }
                        blocks[v * 16U + u] = block.value();
                    }
                }
            }
            neighbors.faces[edge] = blocks;
        }
        if (!complete) {
            return std::nullopt;
        }
        result.sources.push_back({
            .coordinate = coordinate,
            .identity = chunk->contentIdentity(),
            .edit_revision = world.editChunkRevision(coordinate),
        });
        result.sources.insert(result.sources.end(), sources.begin(), sources.end());
        result.chunks.push_back({.chunk = *chunk, .neighbors = neighbors});
    }
    return result;
}

TerrainLodCapturedColumn::TerrainLodCapturedColumn(
    CaptureToken,
    TerrainLodColumnInput&& input,
    std::shared_ptr<shared::TerrainIdentity const> authority_identity_record
)
    : m_input(std::move(input)), m_authority_identity_record(std::move(authority_identity_record))
{}

std::shared_ptr<TerrainLodCapturedColumn const> captureTerrainLodColumnForBatch(
    shared::SparseWorld const& world,
    uint32_t const x,
    uint32_t const y,
    TerrainLodAuthorityBatch& batch
)
{
    auto captured = captureTerrainLodColumn(world, x, y);
    if (!captured) {
        return nullptr;
    }

    std::shared_ptr<shared::TerrainIdentity const> identity_record = batch.m_identity_record;
    if (!captured->authority_sources.empty()) {
        if (!identity_record) {
            identity_record = std::make_shared<shared::TerrainIdentity const>(
                captured->authority_sources.front().identity
            );
        } else if (captured->authority_sources.front().identity != *identity_record) {
            return nullptr;
        }
        // SparseWorld clears authoritative tiles on identity replacement and rejects mismatched publication.
        batch.m_identity_record = identity_record;
        // The mesh uses profile materials and heights. The batch and tile sources
        // own the identities used to validate the capture.
        for (auto& profile : captured->authority_profiles) {
            std::string{}.swap(profile.identity.configuration_identity);
        }
    }

    return std::make_shared<TerrainLodCapturedColumn const>(
        TerrainLodCapturedColumn::CaptureToken{}, std::move(*captured), std::move(identity_record)
    );
}

bool terrainLodColumnSourcesCurrent(TerrainLodColumnInput const& column, shared::SparseWorld const& world)
{
    for (auto const& source : column.sources) {
        if (!chunkSourceCurrent(source, world)) {
            return false;
        }
    }
    if (column.authority_sources.empty()
        && std::ranges::any_of(column.edit_sources, [&](TerrainLodEditSource const& source) {
            return !editSourceCurrent(source, world);
        })) {
        return false;
    }
    if (!column.authority_sources.empty()) {
        shared::TerrainIdentity const& identity = column.authority_sources.front().identity;
        if (std::ranges::any_of(column.authority_sources, [&identity](auto const& source) {
                return source.identity != identity;
            }) || !world.matchesAuthoritativeTileVersions(identity, column.authority_sources)) {
            return false;
        }
    }
    return true;
}

namespace {
template <typename ColumnPointer, typename InputOf, typename BatchIdentityMatches, typename SourceIdentityMatches>
std::vector<TerrainLodColumnCoordinate> staleTerrainLodColumnsImpl(
    std::span<ColumnPointer const> const columns,
    shared::SparseWorld const& world,
    shared::TerrainIdentity const& captured_batch_identity,
    InputOf const& input_of,
    BatchIdentityMatches const& batch_identity_matches,
    SourceIdentityMatches const& source_identity_matches
)
{
    // Reuse tile state only within this synchronous validation pass. A new pass
    // refreshes every tile from SparseWorld, including direct resident edits.
    std::vector<TerrainLodColumnCoordinate> stale;
    auto& cache = authoritativeTileVersionCache();
    auto& pass = authoritativeTileVersionCachePass();
    if (pass == std::numeric_limits<uint64_t>::max() || cache.size() > 32'768U) {
        cache.clear();
        pass = 1U;
    } else {
        ++pass;
    }

    bool const has_authority_sources = std::ranges::any_of(columns, [&input_of](auto const& column) {
        return !input_of(column).authority_sources.empty();
    });
    // SparseWorld validates every published tile against its active identity and clears all tiles when it changes.
    bool const authority_identity_current = !has_authority_sources
        || world.matchesAuthoritativeTileVersions(
            captured_batch_identity, std::span<shared::AuthoritativeTileVersion const>{});
    for (auto const& column_pointer : columns) {
        TerrainLodColumnInput const& column = input_of(column_pointer);
        bool current = true;
        for (TerrainLodChunkSource const& source : column.sources) {
            if (!chunkSourceCurrent(source, world)) {
                current = false;
                break;
            }
        }
        if (current && column.authority_sources.empty()) {
            for (TerrainLodEditSource const& source : column.edit_sources) {
                if (!editSourceCurrent(source, world)) {
                    current = false;
                    break;
                }
            }
        }
        if (current && !column.authority_sources.empty()) {
            current = authority_identity_current && batch_identity_matches(column_pointer, captured_batch_identity);
            for (TerrainLodAuthoritySource const& source : column.authority_sources) {
                if (!current) {
                    break;
                }
                if (!source_identity_matches(column_pointer, source, captured_batch_identity)) {
                    current = false;
                    break;
                }
                uint64_t const key = (static_cast<uint64_t>(static_cast<uint32_t>(source.coordinate.x)) << 32U)
                    | static_cast<uint32_t>(source.coordinate.y);
                auto [entry, inserted] = cache.try_emplace(key);
                static_cast<void>(inserted);
                if (entry->second.pass != pass) {
                    entry->second.pass = pass;
                    entry->second.state = world.currentAuthoritativeTileVersionState(source.coordinate);
                }
                current = entry->second.state
                    && entry->second.state->revision == source.revision
                    && entry->second.state->dense_content_hash == source.dense_content_hash;
            }
        }
        if (!current) {
            stale.push_back({column.x, column.y});
        }
    }
    std::ranges::sort(stale, [](TerrainLodColumnCoordinate const left, TerrainLodColumnCoordinate const right) {
        return left.x != right.x ? left.x < right.x : left.y < right.y;
    });
    return stale;
}
} // namespace

std::vector<TerrainLodColumnCoordinate> staleTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const columns,
    shared::SparseWorld const& world,
    shared::TerrainIdentity const& captured_batch_identity
)
{
    auto const input_of = [](auto const& column) -> TerrainLodColumnInput const& { return *column; };
    auto const batch_identity_matches = [](auto const&, shared::TerrainIdentity const&) { return true; };
    auto const source_identity_matches = [](auto const&, auto const& source, auto const& identity) {
        return source.identity == identity;
    };
    return staleTerrainLodColumnsImpl(
        columns, world, captured_batch_identity, input_of, batch_identity_matches, source_identity_matches
    );
}

std::vector<TerrainLodColumnCoordinate> staleCapturedTerrainLodColumns(
    std::span<std::shared_ptr<TerrainLodCapturedColumn const> const> const columns,
    shared::SparseWorld const& world,
    TerrainLodAuthorityBatch const& batch
)
{
    shared::TerrainIdentity const empty_identity{};
    shared::TerrainIdentity const& batch_identity = batch.m_identity_record
        ? *batch.m_identity_record
        : empty_identity;
    auto const input_of = [](auto const& column) -> TerrainLodColumnInput const& { return column->input(); };
    auto const batch_identity_matches = [&batch](auto const& column, auto const&) {
        return batch.m_identity_record && column->m_authority_identity_record == batch.m_identity_record;
    };
    auto const source_identity_matches = [](auto const&, auto const&, auto const&) { return true; };
    return staleTerrainLodColumnsImpl(
        columns, world, batch_identity, input_of, batch_identity_matches, source_identity_matches
    );
}

bool terrainLodSourcesCurrent(TerrainLodScene const& scene, shared::SparseWorld const& world)
{
    for (TerrainLodChunkSource const& source : scene.chunk_sources) {
        if (!chunkSourceCurrent(source, world)) {
            return false;
        }
    }
    if (std::ranges::any_of(scene.edit_sources, [&](TerrainLodEditSource const& source) {
            return !editSourceCurrent(source, world);
        })) {
        return false;
    }
    if (!scene.authority_sources.empty()) {
        if (std::ranges::any_of(scene.authority_sources, [&scene](auto const& source) {
                return source.identity != scene.identity;
            }) || !world.matchesAuthoritativeTileVersions(scene.identity, scene.authority_sources)) {
            return false;
        }
    }
    return true;
}

namespace {
TerrainLodActualColumn meshColumn(TerrainLodColumnInput const& column, std::stop_token const& cancellation)
{
    TerrainLodActualColumn output{
        .x = column.x,
        .y = column.y,
        .meshes = {},
        .faces = {},
        .edge_heights = column.edge_heights,
        .edge_materials = column.edge_materials,
    };
    if (!column.authority_profiles.empty()) {
        output.faces = meshAuthorityProfiles(column, cancellation);
    }
    output.meshes.reserve(column.chunks.size());
    for (TerrainLodChunkInput const& input : column.chunks) {
        if (cancellation.stop_requested()) {
            throw std::runtime_error("terrain LOD chunk meshing cancelled");
        }
        shared::ChunkMesher mesher;
        output.meshes.push_back(mesher.update(input.chunk, input.neighbors));
    }
    return output;
}

void prepareTerrainLodDrawIndex(TerrainLodScene& scene)
{
    auto draw_index = std::make_shared<TerrainLodDrawIndex>();
    draw_index->reset(scene.groups);
    scene.draw_index = std::move(draw_index);
}

uint32_t faceInstanceCount(size_t count)
{
    if (count > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("terrain LOD face count exceeds the indexed range");
    }
    return static_cast<uint32_t>(count);
}

uint32_t appendLogicalFaceRange(uint64_t& logical_face_count, uint32_t const count)
{
    if (logical_face_count > std::numeric_limits<uint32_t>::max() - count) {
        throw std::overflow_error("terrain LOD logical face count exceeds the indexed range");
    }
    uint32_t const first = static_cast<uint32_t>(logical_face_count);
    logical_face_count += count;
    return first;
}

void finalizeTerrainLodGeometry(
    TerrainLodScene& scene, std::vector<StoneFaceInstance> staged_faces, std::stop_token const& cancellation)
{
    auto context = detail::prepareTerrainLodMeshNormalsImpl(scene, cancellation, staged_faces);
    for (TerrainLodDrawGroup& group : scene.groups) {
        if (group.range.instance_count != 0U && !group.face_storage) {
            if (group.face_storage_first_instance > staged_faces.size()
                || group.range.instance_count > staged_faces.size() - group.face_storage_first_instance) {
                throw std::invalid_argument("terrain LOD staged group range is invalid");
            }
            auto const first = staged_faces.begin() + group.face_storage_first_instance;
            group.face_storage = std::make_shared<std::vector<StoneFaceInstance> const>(
                first, first + group.range.instance_count);
            group.face_storage_first_instance = 0U;
        }
        if (group.range.instance_count != 0U && !group.face_storage) {
            throw std::invalid_argument("terrain LOD group has no geometry owner");
        }
        static_cast<void>(terrainLodGroupFaces(group));
    }
    for (size_t i = 0U; i < scene.groups.size(); ++i) {
        context->groups[i].storage = scene.groups[i].face_storage;
        context->groups[i].empty = scene.groups[i].range.instance_count == 0U;
    }
    scene.normal_context = std::move(context);
}
} // namespace

namespace detail {

std::shared_ptr<TerrainLodNormalContext> prepareTerrainLodMeshNormalsImpl(
    TerrainLodScene& scene, std::stop_token const& cancellation,
    std::span<StoneFaceInstance> staged_faces)
{
    if (cancellation.stop_requested()) {
        throw std::runtime_error("terrain mesh normal preparation cancelled");
    }
    auto const group_faces = [&](TerrainLodDrawGroup const& group) -> std::span<StoneFaceInstance const> {
        if (group.face_storage || group.range.instance_count == 0U) {
            return terrainLodGroupFaces(group);
        }
        if (group.face_storage_first_instance > staged_faces.size()
            || group.range.instance_count > staged_faces.size() - group.face_storage_first_instance) {
            throw std::invalid_argument("terrain normal staged group range is invalid");
        }
        return staged_faces.subspan(group.face_storage_first_instance, group.range.instance_count);
    };
    using Key = std::array<int32_t, 3>;
    using Point = std::array<int64_t, 3>;
    struct Hash final {
        size_t operator()(Key const& key) const noexcept
        {
            return static_cast<size_t>(static_cast<uint32_t>(key[0])) * 73'856'093U
                ^ static_cast<size_t>(static_cast<uint32_t>(key[1])) * 19'349'663U
                ^ static_cast<size_t>(static_cast<uint32_t>(key[2])) * 83'492'791U;
        }
    };
    auto const vertices = [](StoneFaceInstance const& face) {
        int64_t const scale = (face.direction & StoneFaceInstance::TERRAIN_TRIANGLE_XY_Q5) != 0U ? 1 : 32;
        auto const heights = face.terrainTriangleHeights();
        return std::array<Point, 3>{
            Point{face.x * scale, face.y * scale, static_cast<int32_t>(heights[0])},
            Point{face.x * scale + static_cast<int32_t>(static_cast<int16_t>(face.u_extent)) * scale,
                face.y * scale + static_cast<int32_t>(static_cast<int16_t>(face.u_extent >> 16U)) * scale,
                static_cast<int32_t>(heights[1])},
            Point{face.x * scale + static_cast<int32_t>(static_cast<int16_t>(face.v_extent)) * scale,
                face.y * scale + static_cast<int32_t>(static_cast<int16_t>(face.v_extent >> 16U)) * scale,
                static_cast<int32_t>(heights[2])},
        };
    };
    auto const key = [](Point const& point) {
        static constexpr uint32_t PERIOD_Q5 = generation::WORLD_SIZE_BLOCKS * 32U;
        return Key{
            static_cast<int32_t>(static_cast<uint64_t>(point[0]) & (PERIOD_Q5 - 1U)),
            static_cast<int32_t>(static_cast<uint64_t>(point[1]) & (PERIOD_Q5 - 1U)),
            static_cast<int32_t>(point[2]),
        };
    };
    auto const area_normal = [](std::array<Point, 3> const& points) {
        glm::dvec3 const a{points[0][0], points[0][1], points[0][2]};
        glm::dvec3 const b{points[1][0], points[1][1], points[1][2]};
        glm::dvec3 const c{points[2][0], points[2][1], points[2][2]};
        return glm::cross(b - a, c - a);
    };
    uint64_t visited = 0U;
    auto const check_cancelled = [&] {
        if ((visited++ % 4'096U) == 0U && cancellation.stop_requested()) {
            throw std::runtime_error("terrain mesh normal preparation cancelled");
        }
    };
    // Buckets partition exact wrapped vertex keys, not geometric distance or residency.
    auto const bucket = [](Key const& vertex) {
        return (static_cast<uint32_t>(vertex[0]) >> 9U)
            | ((static_cast<uint32_t>(vertex[1]) >> 9U) << 12U);
    };
    auto context = std::make_shared<TerrainLodNormalContext>();
    context->groups.resize(scene.groups.size());
    std::vector<bool> reused(scene.groups.size(), false);
    std::vector<std::shared_ptr<std::vector<StoneFaceInstance> const>> old_storage;
    std::vector<bool> retained;
    bool incremental = static_cast<bool>(scene.normal_context);
    if (incremental) {
        auto const& old = scene.normal_context->groups;
        old_storage.reserve(old.size());
        retained.resize(old.size(), false);
        std::unordered_map<void const*, size_t> previous_groups;
        previous_groups.reserve(old.size());
        for (size_t i = 0U; i < old.size(); ++i) {
            check_cancelled();
            if (old[i].empty) {
                old_storage.push_back(nullptr);
                continue;
            }
            auto storage = old[i].storage.lock();
            if (!storage && old[i].vertex_buckets && old[i].vertex_buckets->empty()) {
                // An expired voxel payload cannot leave stale normal contributions.
                old_storage.push_back(nullptr);
                continue;
            }
            if (!storage || !old[i].vertex_buckets || !previous_groups.emplace(storage.get(), i).second) {
                incremental = false;
                break;
            }
            old_storage.push_back(std::move(storage));
        }
        size_t previous_ordinal = 0U;
        bool have_previous = false;
        for (size_t i = 0U; incremental && i < scene.groups.size(); ++i) {
            check_cancelled();
            auto const& group = scene.groups[i];
            auto const found = previous_groups.find(group.face_storage.get());
            if (found == previous_groups.end()) {
                continue;
            }
            size_t const ordinal = found->second;
            bool const contributes = !old[ordinal].vertex_buckets->empty();
            // Only upward triangles contribute to accumulation order; voxel groups may reorder independently.
            if (retained[ordinal] || (contributes && have_previous && ordinal <= previous_ordinal)
                || old_storage[ordinal]->size() != group.range.instance_count
                || group.face_storage_first_instance != 0U) {
                incremental = false;
                break;
            }
            retained[ordinal] = true;
            reused[i] = true;
            context->groups[i].vertex_buckets = old[ordinal].vertex_buckets;
            if (contributes) {
                previous_ordinal = ordinal;
                have_previous = true;
            }
        }
    }
    if (!incremental) {
        std::fill(reused.begin(), reused.end(), false);
        for (auto& group : context->groups) {
            group.vertex_buckets.reset();
        }
    }
    std::unordered_map<Key, glm::dvec3, Hash> sums;
    std::unordered_set<uint32_t> dirty_buckets;
    auto const dirty_faces = [&](std::span<StoneFaceInstance const> faces) {
        for (auto const& face : faces) {
            check_cancelled();
            if (face.faceDirection() != StoneFaceInstance::TERRAIN_TRIANGLE) {
                continue;
            }
            auto const points = vertices(face);
            if (area_normal(points).z <= 0.0) {
                continue;
            }
            for (auto const& point : points) {
                Key const vertex = key(point);
                sums.try_emplace(vertex, glm::dvec3{0.0});
                dirty_buckets.insert(bucket(vertex));
            }
        }
    };
    if (incremental) {
        for (size_t i = 0U; i < old_storage.size(); ++i) {
            if (!retained[i] && old_storage[i]) {
                dirty_faces(*old_storage[i]);
            }
        }
        for (size_t i = 0U; i < scene.groups.size(); ++i) {
            if (!reused[i]) {
                dirty_faces(group_faces(scene.groups[i]));
            }
        }
    } else {
        sums.reserve(scene.logical_face_count / 2U);
    }
    std::vector<bool> affected(scene.groups.size(), true);
    for (size_t i = 0U; i < scene.groups.size(); ++i) {
        check_cancelled();
        auto& membership = context->groups[i].vertex_buckets;
        if (!membership) {
            std::vector<uint32_t> buckets;
            for (auto const& face : group_faces(scene.groups[i])) {
                check_cancelled();
                if (face.faceDirection() != StoneFaceInstance::TERRAIN_TRIANGLE) {
                    continue;
                }
                auto const points = vertices(face);
                if (area_normal(points).z > 0.0) {
                    for (auto const& point : points) {
                        buckets.push_back(bucket(key(point)));
                    }
                }
            }
            std::ranges::sort(buckets);
            buckets.erase(std::unique(buckets.begin(), buckets.end()), buckets.end());
            membership = std::make_shared<std::vector<uint32_t> const>(buckets.begin(), buckets.end());
        }
        affected[i] = !incremental || !reused[i] || std::ranges::any_of(*membership, [&](uint32_t value) {
            return dirty_buckets.contains(value);
        });
        if (!affected[i]) {
            continue;
        }
        // Preserve current group/face/corner order, including every retained incident triangle.
        for (auto const& face : group_faces(scene.groups[i])) {
            check_cancelled();
            if (face.faceDirection() != StoneFaceInstance::TERRAIN_TRIANGLE) {
                continue;
            }
            auto const points = vertices(face);
            auto const normal = area_normal(points);
            if (normal.z <= 0.0) {
                continue;
            }
            for (auto const& point : points) {
                if (incremental) {
                    if (auto found = sums.find(key(point)); found != sums.end()) {
                        found->second += normal;
                    }
                } else {
                    sums.try_emplace(key(point), glm::dvec3{0.0}).first->second += normal;
                }
            }
        }
    }
    auto const pack = [](glm::dvec3 normal) {
        normal /= std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
        uint32_t const x = static_cast<uint16_t>(static_cast<int16_t>(std::lround(normal.x * 32'767.0)));
        uint32_t const y = static_cast<uint16_t>(static_cast<int16_t>(std::lround(normal.y * 32'767.0)));
        return x | (y << 16U);
    };
    for (size_t group_index = 0U; group_index < scene.groups.size(); ++group_index) {
        if (!affected[group_index]) {
            continue;
        }
        auto& group = scene.groups[group_index];
        auto const faces = group_faces(group);
        std::shared_ptr<std::vector<StoneFaceInstance>> replacement;
        for (size_t index = 0U; index < faces.size(); ++index) {
            check_cancelled();
            auto const& face = faces[index];
            auto normals = incremental && reused[group_index] ? face.terrain_normals : std::array<uint32_t, 4>{};
            if (face.faceDirection() == StoneFaceInstance::TERRAIN_TRIANGLE) {
                auto const points = vertices(face);
                if (area_normal(points).z > 0.0) {
                    for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
                        if (auto found = sums.find(key(points[vertex])); found != sums.end()) {
                            normals[vertex] = pack(found->second);
                        }
                    }
                    normals[3] = 1U;
                }
            }
            if (face.terrain_normals != normals) {
                if (!group.face_storage) {
                    staged_faces[group.face_storage_first_instance + index].terrain_normals = normals;
                    continue;
                }
                if (!replacement) {
                    replacement = std::make_shared<std::vector<StoneFaceInstance>>(faces.begin(), faces.end());
                }
                (*replacement)[index].terrain_normals = normals;
            }
        }
        if (replacement) {
            group.face_storage = std::move(replacement);
        }
    }
    return context;
}

void prepareTerrainLodMeshNormals(TerrainLodScene& scene, std::stop_token const& cancellation,
    std::span<StoneFaceInstance> staged_faces)
{
    auto context = prepareTerrainLodMeshNormalsImpl(scene, cancellation, staged_faces);
    for (size_t i = 0U; i < scene.groups.size(); ++i) {
        context->groups[i].storage = scene.groups[i].face_storage;
        context->groups[i].empty = scene.groups[i].range.instance_count == 0U;
    }
    scene.normal_context = std::move(context);
}

} // namespace detail

std::span<StoneFaceInstance const> terrainLodGroupFaces(TerrainLodDrawGroup const& group)
{
    if (group.range.instance_count == 0U) {
        return {};
    }
    if (!group.face_storage || group.face_storage_first_instance != 0U
        || group.face_storage->size() != group.range.instance_count) {
        throw std::invalid_argument("terrain LOD group geometry slice is invalid");
    }
    return std::span<StoneFaceInstance const>{*group.face_storage};
}

size_t terrainLodSceneFaceCount(TerrainLodScene const& scene) noexcept
{
    return scene.logical_face_count;
}

std::shared_ptr<TerrainLodScene const> buildTerrainLodColumnPatch(
    shared::TerrainIdentity const identity,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const columns,
    std::stop_token const& cancellation)
{
    auto scene = std::make_shared<TerrainLodScene>();
    scene->identity = identity;
    std::vector<StoneFaceInstance> staged_faces;
    uint64_t logical_face_count = 0U;
    for (auto const& input : columns) {
        if (!input->authority_sources.empty() && input->authority_sources.front().identity != identity) {
            throw std::invalid_argument("terrain LOD patch column identity is stale");
        }
        auto column = meshColumn(*input, cancellation);
        uint32_t const storage_first = faceInstanceCount(staged_faces.size());
        staged_faces.insert(staged_faces.end(), column.faces.begin(), column.faces.end());
        for (auto const& mesh : column.meshes) {
            for (auto const& face : mesh.faces) {
                staged_faces.push_back(StoneFaceInstance::fromChunkFace(mesh.coordinate, face));
            }
        }
        uint32_t const instance_count = faceInstanceCount(staged_faces.size() - storage_first);
        uint32_t const first = appendLogicalFaceRange(logical_face_count, instance_count);
        scene->groups.push_back({
            .range = {first, instance_count},
            .face_storage_first_instance = storage_first,
            .origin_x = column.x * 16U,
            .origin_y = column.y * 16U,
            .extent = 16U,
            .minimum_z = 0,
            .maximum_z = static_cast<int32_t>(shared::WorldExtent::DEPTH),
        });
        scene->chunk_sources.insert(scene->chunk_sources.end(), input->sources.begin(), input->sources.end());
        scene->authority_sources.insert(
            scene->authority_sources.end(), input->authority_sources.begin(), input->authority_sources.end());
        appendFallbackEditSources(*scene, *input);
        scene->leaves.push_back({.level = shared::TerrainLodLevel::L3, .x = column.x, .y = column.y});
    }
    scene->logical_face_count = static_cast<size_t>(logical_face_count);
    scene->actual_column_count = static_cast<uint32_t>(columns.size());
    normalizeTerrainLodEditSources(scene->edit_sources);
    finalizeTerrainLodGeometry(*scene, std::move(staged_faces), cancellation);
    return scene;
}

static std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneFromColumnsImpl(
    shared::TerrainWorld const& world,
    uint32_t const center_x,
    uint32_t const center_y,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const columns,
    std::stop_token const& cancellation,
    TerrainLodScene const* previous,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> const unchanged,
    bool const promote_columns,
    std::optional<TerrainLodColumnCoordinate> const visual_center, LocalBuild* local,
    std::shared_ptr<shared::TerrainLodArtifact const> const& prepared_root = {})
{
    if (previous != nullptr && (previous->shading_atlas != world.atlas() || previous->identity != world.identity())) {
        previous = nullptr;
    }
    std::vector<TerrainLodActualColumn> actual;
    bool const trace_phases = terrainLodPhaseTraceEnabled();
    auto const started = terrainLodPhaseTime(trace_phases);
    actual.reserve(columns.size());
    std::vector<TerrainLodColumnCoordinate> changed;
    std::unordered_set<TerrainLodColumnInput const*> unchanged_inputs;
    for (auto const& input : unchanged) {
        unchanged_inputs.insert(input.get());
    }
    std::unordered_set<uint64_t> previous_actual_keys;
    if (previous != nullptr) {
        for (TerrainLodRegion const leaf : previous->leaves) {
            if (leaf.level == shared::TerrainLodLevel::L3) {
                previous_actual_keys.insert((static_cast<uint64_t>(leaf.x) << 32U) | leaf.y);
            }
        }
    }
    auto const visual = visual_center.value_or(TerrainLodColumnCoordinate{center_x / 16U, center_y / 16U});
    std::unordered_set<uint64_t> visually_retained;
    for (auto const& input : columns) {
        if (visual_center && !terrainLodVisualColumn(input->x, input->y, visual.x, visual.y)) {
            continue;
        }
        visually_retained.insert((static_cast<uint64_t>(input->x) << 32U) | input->y);
        if (!input->authority_sources.empty()
            && input->authority_sources.front().identity != world.identity()) {
            throw std::invalid_argument("terrain LOD scene column identity is stale");
        }
        uint64_t const key = (static_cast<uint64_t>(input->x) << 32U) | input->y;
        if (previous != nullptr && unchanged_inputs.contains(input.get()) && previous_actual_keys.contains(key)) {
            actual.push_back({.x = input->x, .y = input->y, .meshes = {}, .faces = {},
                .edge_heights = input->edge_heights, .edge_materials = input->edge_materials});
        } else {
            actual.push_back(meshColumn(*input, cancellation));
            changed.push_back({input->x, input->y});
        }
    }
    if (previous != nullptr) {
        std::unordered_set<uint64_t> retained;
        for (auto const& input : columns) {
            if (!visually_retained.contains((static_cast<uint64_t>(input->x) << 32U) | input->y)) {
                continue;
            }
            retained.insert((static_cast<uint64_t>(input->x) << 32U) | input->y);
        }
        for (auto const leaf : previous->leaves) {
            if (leaf.level == shared::TerrainLodLevel::L3
                && !retained.contains((static_cast<uint64_t>(leaf.x) << 32U) | leaf.y)) {
                changed.push_back({leaf.x, leaf.y});
            }
        }
    }
    auto const meshed = terrainLodPhaseTime(trace_phases);
    auto scene = std::const_pointer_cast<TerrainLodScene>(
        buildTerrainLodSceneImpl(world, center_x, center_y, actual, cancellation, previous, changed,
            promote_columns, visual_center, local, prepared_root));
    auto const built = terrainLodPhaseTime(trace_phases);
    std::unordered_set<uint64_t> rendered_columns;
    for (auto const leaf : scene->leaves) {
        if (leaf.level == shared::TerrainLodLevel::L3) {
            rendered_columns.insert((static_cast<uint64_t>(leaf.x) << 32U) | leaf.y);
        }
    }
    std::unordered_set<uint64_t> authority_keys;
    for (auto const& input_column : columns) {
        TerrainLodColumnInput const& column = *input_column;
        // Captures outside the refined scene contribute no geometry. Their
        // streaming changes must not discard valid near-ground geometry.
        if (!rendered_columns.contains((static_cast<uint64_t>(column.x) << 32U) | column.y)) {
            continue;
        }
        scene->chunk_sources.insert(scene->chunk_sources.end(), column.sources.begin(), column.sources.end());
        appendFallbackEditSources(*scene, column);
        for (auto const& source : column.authority_sources) {
            uint64_t const key =
                (static_cast<uint64_t>(source.coordinate.x) << 32U) | static_cast<uint32_t>(source.coordinate.y);
            if (authority_keys.insert(key).second) {
                scene->authority_sources.push_back(source);
            }
        }
    }
    normalizeTerrainLodEditSources(scene->edit_sources);
    if (trace_phases) {
        auto const finished = std::chrono::steady_clock::now();
        auto const ns = [](auto const duration) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        };
        CORE_INFO(
            "stream_trace event=column_scene_phases ns={} scene={} base_scene={} columns={} changed={} "
            "mesh_ns={} scene_ns={} sources_ns={}",
            ns(finished.time_since_epoch()), reinterpret_cast<uintptr_t>(scene.get()),
            reinterpret_cast<uintptr_t>(previous), columns.size(), changed.size(),
            ns(meshed - started), ns(built - meshed), ns(finished - built));
    }
    return scene;
}

namespace {
std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneImpl(
    shared::TerrainWorld const& world,
    uint32_t const center_x,
    uint32_t const center_y,
    std::span<TerrainLodActualColumn const> const actual_columns,
    std::stop_token const& cancellation,
    TerrainLodScene const* previous,
    std::span<TerrainLodColumnCoordinate const> const changed,
    bool const promote_columns,
    std::optional<TerrainLodColumnCoordinate> const visual_center, LocalBuild* local,
    std::shared_ptr<shared::TerrainLodArtifact const> const& prepared_root)
{
    if (cancellation.stop_requested()) {
        throw generation::GenerationCancelled{};
    }
    if (riverSceneDiagnosticCensus()) {
        char const* const evidence = std::getenv("MC_RIVER_SCENE_EVIDENCE");
        if (evidence == nullptr || evidence[0] == '\0') {
            throw std::invalid_argument("river census requires an evidence output path");
        }
    }
    bool const trace_phases = terrainLodPhaseTraceEnabled();
    auto const started = terrainLodPhaseTime(trace_phases);
    auto const atlas = world.atlas();
    if (previous != nullptr && (previous->shading_atlas != atlas || previous->identity != world.identity())) {
        previous = nullptr;
    }
    uint32_t const spacing = shared::terrainLodSpacing(
        atlas->configuration.resolution, shared::TerrainLodLevel::L0, atlas->configuration.model_tag);
    uint32_t const root_side = PERIOD / spacing + 1U;
    auto const compatible_root = [&](std::shared_ptr<shared::TerrainLodArtifact const> const& root) {
        return root && root->terrain_identity == world.identity()
            && root->model_identity == shared::terrainLodModel(atlas->configuration.model_tag)
            && root->level == shared::TerrainLodLevel::L0 && root->origin_x == 0U && root->origin_y == 0U
            && root->extent == PERIOD && root->spacing == spacing
            && root->side == root_side && root->nodes.size() == static_cast<uint64_t>(root_side) * root_side;
    };
    auto root = previous ? previous->root_artifact : nullptr;
    if (!compatible_root(root)) {
        root = compatible_root(prepared_root) ? prepared_root
            : shared::buildTerrainLod(world, shared::TerrainLodLevel::L0, 0U, 0U, PERIOD, cancellation);
    }
    auto scene = std::make_shared<TerrainLodScene>();
    scene->normal_context = previous != nullptr ? previous->normal_context : nullptr;
    scene->root_artifact = root;
    scene->identity = root->terrain_identity;
    scene->center_x = center_x;
    scene->center_y = center_y;
    scene->visual_center = visual_center;
    scene->shading_atlas = world.atlas();
    scene->root_extent = root->spacing;
    scene->block_sea_level = world.atlas()->block_sea_level;
    std::vector<StoneFaceInstance> staged_faces;
    std::shared_ptr<TerrainLodRiverConstraints> river_constraints;
    std::shared_ptr<RiverFanOutputCache const> previous_fan_outputs;
    std::shared_ptr<RiverFanOutputCache> fan_outputs;
    if (atlas->configuration.model_tag == generation::TerrainModelTag::V10) {
        river_constraints = previous != nullptr && previous->river_constraints
            ? std::make_shared<TerrainLodRiverConstraints>(*previous->river_constraints)
            : std::make_shared<TerrainLodRiverConstraints>();
        river_constraints->atlas = atlas;
        river_constraints->field = generation::v10RiverBedField(*atlas);
        previous_fan_outputs = river_constraints->fan_outputs;
        if (previous_fan_outputs && (previous_fan_outputs->atlas != atlas
            || previous_fan_outputs->field != river_constraints->field)) {
            previous_fan_outputs.reset();
        }
        // Retain only this build's emitted/looked-up fans. Reused whole groups
        // need no cache entries; eviction is a safe miss on a later build.
        fan_outputs = std::make_shared<RiverFanOutputCache>();
        fan_outputs->atlas = atlas;
        fan_outputs->field = river_constraints->field;
        river_constraints->fan_outputs = fan_outputs;
        scene->river_constraints = river_constraints;
    }
    auto const river_region = [&](uint32_t const x, uint32_t const y)
        -> river_field_candidate::RiverConstraintExport const& {
        uint32_t const ox = x / scene->root_extent * scene->root_extent;
        uint32_t const oy = y / scene->root_extent * scene->root_extent;
        uint64_t const key = (static_cast<uint64_t>(ox) << 32U) | oy;
        if (river_constraints->empty_regions.contains(key)) {
            ++scene->river_geometry_audit.regional_export_reuse_count;
            return river_constraints->empty_region;
        }
        auto const found = river_constraints->regions.find(key);
        if (found != river_constraints->regions.end()) {
            ++scene->river_geometry_audit.regional_export_reuse_count;
            return *found->second;
        }
        ++scene->river_geometry_audit.regional_export_build_count;
        // A 1m feature segment plus Q5 rounding must be fully present on both
        // sides of a regional boundary before representable rails are clipped.
        static constexpr double RIVER_CONSTRAINT_HALO = 2.0;
        river_field_candidate::ConstraintRequest const request{{double(ox) - RIVER_CONSTRAINT_HALO,
            double(oy) - RIVER_CONSTRAINT_HALO},
            {double(ox + scene->root_extent) + RIVER_CONSTRAINT_HALO,
                double(oy + scene->root_extent) + RIVER_CONSTRAINT_HALO},
            river_field_candidate::ConsumerKind::L0, 32.0, 1.0};
        auto result = river_field_candidate::exportRiverConstraints(*river_constraints->field, request,
            [&](std::span<river_field_candidate::WorldXY const> const xy)
                -> std::expected<std::vector<river_field_candidate::NativePointSample>,
                    river_field_candidate::Failure> {
                std::vector<std::array<double, 2>> points;
                points.reserve(xy.size());
                for (auto const& point : xy) {
                    points.push_back({point.x_m, point.y_m});
                }
                auto const native = generation::queryV10NativeBedSamples(*atlas, points, cancellation);
                std::vector<river_field_candidate::NativePointSample> samples;
                samples.reserve(native.size());
                for (auto const& point : native) {
                    samples.push_back({{point.xy_m[0], point.xy_m[1]},
                    point.foundation_m, point.standing_stage_m, point.standing_owner});
                }
                return samples;
            });
        if (!result) {
            throw generation::V10RiverFieldError(result.error());
        }
        if (result->polylines.empty()) {
            river_constraints->empty_regions.insert(key);
            return river_constraints->empty_region;
        }
        auto stored = std::make_shared<river_field_candidate::RiverConstraintExport const>(std::move(*result));
        river_constraints->regions.emplace(key, stored);
        return *stored;
    };
    std::unordered_map<uint64_t, shared::Block> bank_materials;
    std::optional<RiverFanQueryMemo> query_memo;
    std::shared_ptr<RiverFanQueryMemo const> predecessor_query_memo;
    bool const retain_queries = local != nullptr && local->transient_recenter;
    bool const combine_height_and_wet = retain_queries || atlas->configuration.use_v10_fine_terrain;
    if (river_constraints) {
        query_memo.emplace();
        if (retain_queries && previous != nullptr && previous->river_constraints
            && previous->river_constraints->atlas == atlas
            && previous->river_constraints->field == river_constraints->field) {
            predecessor_query_memo = std::move(river_constraints->query_memo);
        }
        river_constraints->query_memo.reset();
    }
    auto const emit_fan = [&](std::array<double, 2> const minimum, std::array<double, 2> const maximum,
                             std::array<double, 2> const center, std::span<RiverFanSector const> const sectors,
                             uint32_t const x, uint32_t const y, int32_t& minimum_height, int32_t& maximum_height) {
        if (cancellation.stop_requested()) {
            throw std::runtime_error("terrain LOD scene cancelled");
        }
        auto const& constraints = river_region(x, y);
        char const* const evidence = std::getenv("MC_RIVER_SCENE_EVIDENCE");
        if (maximum[0] - minimum[0] != 2.0 || maximum[1] - minimum[1] != 2.0
            || sectors.size() != CACHED_FAN_SECTORS || riverSceneDiagnosticCensus()
            || (evidence != nullptr && evidence[0] != '\0')) {
            emitRiverFan(staged_faces, minimum, maximum, center, sectors, constraints, *atlas, world, bank_materials,
                query_memo ? &*query_memo : nullptr, predecessor_query_memo.get(), combine_height_and_wet,
                minimum_height, maximum_height, scene->river_geometry_audit);
            return;
        }
        RiverFanCrop const crop{minimum[0], minimum[1], maximum[0], maximum[1], center[0], center[1]};
        auto const inputs = riverFanInputs(crop, sectors);
        std::shared_ptr<river_field_candidate::RiverConstraintExport const> source;
        if (!constraints.polylines.empty()) {
            uint32_t const ox = x / scene->root_extent * scene->root_extent;
            uint32_t const oy = y / scene->root_extent * scene->root_extent;
            source = river_constraints->regions.at((static_cast<uint64_t>(ox) << 32U) | oy);
        }
        if (previous_fan_outputs) {
            auto const found = previous_fan_outputs->entries.find(crop);
            if (found != previous_fan_outputs->entries.end()
                && found->second->inputs == inputs && found->second->source == source) {
                auto const& output = found->second;
                staged_faces.insert(staged_faces.end(), output->faces.begin(), output->faces.end());
                minimum_height = std::min(minimum_height, output->minimum_height);
                maximum_height = std::max(maximum_height, output->maximum_height);
                fan_outputs->entries.emplace(crop, output);
                // Geometry audits count only newly generated output, as for
                // whole-group reuse. Current source versions are rebuilt below.
                return;
            }
        }
        size_t const first = staged_faces.size();
        int32_t fan_minimum = std::numeric_limits<int32_t>::max();
        int32_t fan_maximum = 0;
        emitRiverFan(staged_faces, minimum, maximum, center, sectors, constraints, *atlas, world, bank_materials,
            query_memo ? &*query_memo : nullptr, predecessor_query_memo.get(), combine_height_and_wet,
            fan_minimum, fan_maximum, scene->river_geometry_audit);
        minimum_height = std::min(minimum_height, fan_minimum);
        maximum_height = std::max(maximum_height, fan_maximum);
        auto const first_face = staged_faces.begin() + static_cast<decltype(staged_faces)::difference_type>(first);
        auto output = std::make_shared<RiverFanOutput>(RiverFanOutput{
            .inputs = inputs,
            .source = std::move(source),
            .faces = {first_face, staged_faces.end()},
            .minimum_height = fan_minimum,
            .maximum_height = fan_maximum,
        });
        fan_outputs->entries.emplace(crop, std::move(output));
    };
    auto const selection_started = terrainLodPhaseTime(trace_phases);
    // New exact columns can arrive beyond the previous center's L2 window.
    // Refine only their footprints and the existing transition band; unrelated
    // coarse cells retain their selection and immutable geometry.
    std::unordered_set<uint64_t> exact_refinement;
    auto const visual = visual_center.value_or(TerrainLodColumnCoordinate{center_x / 16U, center_y / 16U});
    for (auto const& column : actual_columns) {
        if (!promote_columns) {
            break;
        }
        if (visual_center && !terrainLodVisualColumn(column.x, column.y, visual.x, visual.y)) {
            continue;
        }
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                uint32_t const x = wrap(static_cast<int64_t>(column.x) * 16 + dx * 16);
                uint32_t const y = wrap(static_cast<int64_t>(column.y) * 16 + dy * 16);
                for (auto const level : {shared::TerrainLodLevel::L0, shared::TerrainLodLevel::L1}) {
                    uint32_t const extent = terrainLodRegionExtent(level, scene->root_extent);
                    exact_refinement.insert(cellKey({.level = level, .x = x / extent, .y = y / extent}));
                }
            }
        }
    }
    std::vector<uint64_t> forcing(exact_refinement.begin(), exact_refinement.end());
    std::ranges::sort(forcing);
    auto const cached_layout = previous != nullptr ? previous->local_layout : nullptr;
    uint32_t const side = PERIOD / root->spacing;
    size_t const root_count = static_cast<size_t>(side) * side;
    bool cached_samples_valid = cached_layout && cached_layout->samples
        && cached_layout->field.lock() == atlas && cached_layout->root.lock() == root
        && cached_layout->center_x == center_x && cached_layout->center_y == center_y
        && cached_layout->samples->cells.size() == cached_layout->samples->traversal_order.size()
        && cached_layout->samples->cells.size() == cached_layout->samples->center_materials.size()
        && cached_layout->samples->root_subtree_offsets.size() == root_count + 1U
        && !cached_layout->samples->root_subtree_offsets.empty()
        && cached_layout->samples->root_subtree_offsets.front() == 0U
        && cached_layout->samples->root_subtree_offsets.back() == cached_layout->samples->cells.size()
        && std::ranges::is_sorted(cached_layout->forcing);
    if (cached_samples_valid) {
        auto const& offsets = cached_layout->samples->root_subtree_offsets;
        for (size_t i = 1U; i < offsets.size(); ++i) {
            if (offsets[i] < offsets[i - 1U] || offsets[i] > cached_layout->samples->cells.size()) {
                cached_samples_valid = false;
                break;
            }
        }
    }
    bool reuse_selection = local != nullptr && cached_samples_valid
        && (cached_layout->visual == visual_center
            || (local->transient_recenter && cached_layout->visual == previous->visual_center))
        && std::ranges::includes(forcing, cached_layout->forcing);
    if (reuse_selection) {
        auto const& prepared = *cached_layout->samples;
        // With a fixed field/center, adding a forcing bit cannot change a region
        // that was already refined. Removing forcing requires fresh selection.
        for (uint64_t const key : forcing) {
            if (cancellation.stop_requested()) {
                throw std::runtime_error("terrain LOD scene cancelled");
            }
            if (std::ranges::binary_search(cached_layout->forcing, key)) {
                continue;
            }
            auto const found = std::ranges::lower_bound(prepared.cells, key, {}, [](LocalCell const& cell) {
                return cellKey({static_cast<shared::TerrainLodLevel>(cell.level), cell.x, cell.y});
            });
            if (found != prepared.cells.end()
                && cellKey({static_cast<shared::TerrainLodLevel>(found->level), found->x, found->y}) == key) {
                if (local->transient_recenter) {
                    // A newly forced leaf must split. The transient caller can
                    // go straight to the conventional builder that reuses unaffected roots.
                    throw LocalUnsupported{"forced_selection_split"};
                }
                reuse_selection = false;
                break;
            }
        }
    }
    bool reuse_subtrees = local == nullptr && cached_samples_valid;
    TerrainLodSelectionAudit selection_audit;
    std::vector<Cell> cells;
    std::vector<size_t> root_subtree_offsets(root_count + 1U);
    auto const cell_from_cache = [](LocalCell const& value, shared::Block const material) {
        Cell cell{
            .region = {static_cast<shared::TerrainLodLevel>(value.level), value.x, value.y},
            .extent = value.extent,
            .material = material,
            .corner_heights = value.heights,
            .corner_temperatures = value.temperatures,
        };
        for (size_t corner = 0U; corner < 4U; ++corner) {
            cell.corner_materials[corner] = static_cast<shared::Block>(value.materials[corner]);
        }
        return cell;
    };
    if (reuse_selection) {
        auto const& prepared = *cached_layout->samples;
        selection_audit.reused_l0_root_count = root_count;
        cells.resize(prepared.cells.size());
        root_subtree_offsets = prepared.root_subtree_offsets;
        for (size_t i = 0U; i < prepared.cells.size(); ++i) {
            if ((i % 4'096U) == 0U && cancellation.stop_requested()) {
                throw std::runtime_error("terrain LOD scene cancelled");
            }
            cells[prepared.traversal_order[i]] = cell_from_cache(prepared.cells[i], prepared.center_materials[i]);
        }
    } else if (reuse_subtrees) {
        auto const& prepared = *cached_layout->samples;
        constexpr size_t INVALID_SAMPLE = std::numeric_limits<size_t>::max();
        std::vector<size_t> sample_by_traversal(prepared.cells.size(), INVALID_SAMPLE);
        for (size_t sample = 0U; sample < prepared.traversal_order.size(); ++sample) {
            uint32_t const traversal = prepared.traversal_order[sample];
            if (traversal >= sample_by_traversal.size() || sample_by_traversal[traversal] != INVALID_SAMPLE) {
                reuse_subtrees = false;
                break;
            }
            sample_by_traversal[traversal] = sample;
        }
        std::vector<uint8_t> changed_roots(root_count, 0U);
        auto const mark_changed_root = [&](uint64_t const key) {
            uint32_t const level_value = static_cast<uint32_t>(key >> 48U);
            if (level_value > static_cast<uint32_t>(shared::TerrainLodLevel::L1)) {
                return false;
            }
            uint32_t const region_x = static_cast<uint32_t>((key >> 24U) & 0x00ff'ffffU);
            uint32_t const region_y = static_cast<uint32_t>(key & 0x00ff'ffffU);
            uint32_t const extent = terrainLodRegionExtent(
                static_cast<shared::TerrainLodLevel>(level_value), root->spacing);
            uint64_t const origin_x = static_cast<uint64_t>(region_x) * extent;
            uint64_t const origin_y = static_cast<uint64_t>(region_y) * extent;
            if (origin_x >= PERIOD || origin_y >= PERIOD) {
                return false;
            }
            size_t const root_index = static_cast<size_t>(origin_y / root->spacing) * side
                + origin_x / root->spacing;
            if (root_index >= changed_roots.size()) {
                return false;
            }
            changed_roots[root_index] = 1U;
            return true;
        };
        if (reuse_subtrees) {
            size_t previous_key = 0U, current_key = 0U;
            while (previous_key < cached_layout->forcing.size() || current_key < forcing.size()) {
                if (current_key == forcing.size()
                    || (previous_key < cached_layout->forcing.size()
                        && cached_layout->forcing[previous_key] < forcing[current_key])) {
                    reuse_subtrees = mark_changed_root(cached_layout->forcing[previous_key]) && reuse_subtrees;
                    ++previous_key;
                } else if (previous_key == cached_layout->forcing.size()
                    || forcing[current_key] < cached_layout->forcing[previous_key]) {
                    reuse_subtrees = mark_changed_root(forcing[current_key]) && reuse_subtrees;
                    ++current_key;
                } else {
                    ++previous_key;
                    ++current_key;
                }
                if (!reuse_subtrees) {
                    break;
                }
            }
        }
        if (reuse_subtrees) {
            cells.reserve(std::max(prepared.cells.size(), root->nodes.size() + 8'192U));
            for (size_t root_index = 0U; root_index < root_count; ++root_index) {
                if ((root_index % 4'096U) == 0U && cancellation.stop_requested()) {
                    throw std::runtime_error("terrain LOD scene cancelled");
                }
                root_subtree_offsets[root_index] = cells.size();
                if (changed_roots[root_index] != 0U) {
                    ++selection_audit.rebuilt_l0_root_count;
                    collectCells(
                        world,
                        *root,
                        {.level = shared::TerrainLodLevel::L0,
                            .x = static_cast<uint32_t>(root_index % side),
                            .y = static_cast<uint32_t>(root_index / side)},
                        root->spacing,
                        center_x,
                        center_y,
                        cells,
                        exact_refinement,
                        cancellation);
                    continue;
                }
                ++selection_audit.reused_l0_root_count;
                for (size_t traversal = prepared.root_subtree_offsets[root_index];
                    traversal < prepared.root_subtree_offsets[root_index + 1U]; ++traversal) {
                    size_t const sample = sample_by_traversal[traversal];
                    cells.push_back(cell_from_cache(prepared.cells[sample], prepared.center_materials[sample]));
                }
            }
            root_subtree_offsets[root_count] = cells.size();
        }
    }
    if (!reuse_selection && !reuse_subtrees) {
        selection_audit.rebuilt_l0_root_count = root_count;
        cells.reserve(root->nodes.size() + 8'192U);
        for (uint32_t y = 0U; y < side; ++y) {
            for (uint32_t x = 0U; x < side; ++x) {
                size_t const root_index = static_cast<size_t>(y) * side + x;
                root_subtree_offsets[root_index] = cells.size();
                collectCells(
                    world,
                    *root,
                    {.level = shared::TerrainLodLevel::L0, .x = x, .y = y},
                    root->spacing,
                    center_x,
                    center_y,
                    cells,
                    exact_refinement,
                    cancellation);
                root_subtree_offsets[root_index + 1U] = cells.size();
            }
        }
    }
    scene->selection_audit = selection_audit;
    auto const selected = terrainLodPhaseTime(trace_phases);
    std::unordered_map<uint64_t, uint32_t> index;
    index.reserve(cells.size());
    for (uint32_t i = 0U; i < cells.size(); ++i) {
        index.emplace(cellKey(cells[i].region), i);
    }
    auto const cell_at = [&index, &cells, &scene](uint32_t const x, uint32_t const y) -> Cell const& {
        for (uint32_t level = 3U; level-- > 0U;) {
            auto const lod = static_cast<shared::TerrainLodLevel>(level);
            uint32_t const extent = terrainLodRegionExtent(lod, scene->root_extent);
            auto const found = index.find(cellKey({.level = lod, .x = x / extent, .y = y / extent}));
            if (found != index.end()) {
                return cells[found->second];
            }
        }
        throw std::runtime_error("terrain LOD selection has a coverage hole");
    };
    auto const column_key = [](uint32_t const x, uint32_t const y) {
        return (static_cast<uint64_t>(x) << 32U) | y;
    };
    std::unordered_map<uint64_t, TerrainLodActualColumn const*> actual;
    actual.reserve(actual_columns.size());
    for (TerrainLodActualColumn const& column : actual_columns) {
        bool covered = column.x < PERIOD / 16U && column.y < PERIOD / 16U
            && (!visual_center || terrainLodVisualColumn(column.x, column.y, visual.x, visual.y));
        for (uint32_t y = 0U; y < 16U && covered; y += 8U) {
            for (uint32_t x = 0U; x < 16U && covered; x += 8U) {
                covered = cell_at(column.x * 16U + x, column.y * 16U + y).region.level
                    == shared::TerrainLodLevel::L2;
            }
        }
        if (covered) {
            actual.emplace(column_key(column.x, column.y), &column);
        }
    }
    struct TransitionClassification final {
        std::unordered_set<uint64_t> columns;
        std::unordered_set<uint64_t> cells;
    };
    auto const classifyTransitions = [&](std::unordered_set<uint64_t> const& actual_keys) {
        TransitionClassification result;
        for (uint64_t const key : actual_keys) {
            uint32_t const column_x = static_cast<uint32_t>(key >> 32U);
            uint32_t const column_y = static_cast<uint32_t>(key);
            for (int32_t dy = -2; dy <= 2; ++dy) {
                for (int32_t dx = -2; dx <= 2; ++dx) {
                    uint32_t const x = wrap(static_cast<int64_t>(column_x) * 16 + dx * 16) / 16U;
                    uint32_t const y = wrap(static_cast<int64_t>(column_y) * 16 + dy * 16) / 16U;
                    uint64_t const neighbor = column_key(x, y);
                    if (!actual_keys.contains(neighbor)) {
                        result.columns.insert(neighbor);
                    }
                }
            }
        }
        uint32_t const step = std::min(16U, terrainLodRegionExtent(shared::TerrainLodLevel::L2, scene->root_extent));
        for (uint64_t const key : result.columns) {
            uint32_t const x = static_cast<uint32_t>(key >> 32U) * 16U;
            uint32_t const y = static_cast<uint32_t>(key) * 16U;
            for (uint32_t dy = 0U; dy < 16U; dy += step) {
                for (uint32_t dx = 0U; dx < 16U; dx += step) {
                    result.cells.insert(cellKey(cell_at(x + dx, y + dy).region));
                }
            }
        }
        return result;
    };
    std::unordered_set<uint64_t> old_actual_keys;
    if (local != nullptr && local->transient_recenter && previous != nullptr) {
        for (TerrainLodRegion const leaf : previous->leaves) {
            if (leaf.level == shared::TerrainLodLevel::L3) {
                old_actual_keys.insert(column_key(leaf.x, leaf.y));
            }
        }
    }
    std::unordered_set<uint64_t> actual_keys;
    actual_keys.reserve(actual.size());
    for (auto const& [key, column] : actual) {
        static_cast<void>(column);
        actual_keys.insert(key);
    }
    std::unordered_set<uint64_t> changed_mask_cells;
    if (local != nullptr && local->transient_recenter) {
        if (std::ranges::any_of(old_actual_keys, [&](uint64_t const key) { return !actual_keys.contains(key); })) {
            throw LocalUnsupported{"retired_actual_mask"};
        }
        for (uint64_t const key : actual_keys) {
            if (old_actual_keys.contains(key)) {
                continue;
            }
            for (uint32_t level = 0U; level < 3U; ++level) {
                auto const lod = static_cast<shared::TerrainLodLevel>(level);
                uint32_t const extent = terrainLodRegionExtent(lod, scene->root_extent);
                uint32_t const side = PERIOD / extent;
                // Invert the old column-grid scan. A cell's first and last
                // sampled columns are floor(origin / 16) - 2 and
                // floor(origin / 16) + floor(extent / 16) + 2, inclusive.
                auto const origin_range = [extent](uint32_t const column) {
                    uint32_t const base = column * 16U + 2U * PERIOD;
                    uint32_t const first = base - (extent / 16U) * 16U - TRANSITION_WIDTH;
                    uint32_t const last = base + TRANSITION_WIDTH + 15U;
                    return std::pair{(first + extent - 1U) / extent, last / extent};
                };
                auto const [min_x, max_x] = origin_range(static_cast<uint32_t>(key >> 32U));
                auto const [min_y, max_y] = origin_range(static_cast<uint32_t>(key));
                for (uint32_t y = min_y; y <= max_y; ++y) {
                    for (uint32_t x = min_x; x <= max_x; ++x) {
                        changed_mask_cells.insert(cellKey({.level = lod, .x = x % side, .y = y % side}));
                    }
                }
            }
        }
    }
    TransitionClassification const new_transitions = classifyTransitions(actual_keys);
    TransitionClassification const old_transitions = local != nullptr && local->transient_recenter
        ? classifyTransitions(old_actual_keys) : TransitionClassification{};
    std::unordered_map<uint64_t, std::array<int32_t, 3>> shared_heights;
    std::function<std::array<int32_t, 3>(uint32_t, uint32_t)> corner_height;
    corner_height = [&cell_at, &index, &cells, &shared_heights, &corner_height](
        uint32_t px,
        uint32_t py
    ) -> std::array<int32_t, 3> {
        px = wrap(px);
        py = wrap(py);
        uint64_t const key = (static_cast<uint64_t>(px) << 32U) | py;
        auto const cached = shared_heights.find(key);
        if (cached != shared_heights.end()) {
            return cached->second;
        }
        uint32_t owner_index = index.at(cellKey(cell_at(px, py).region));
        for (uint32_t dy = 0U; dy < 2U; ++dy) {
            for (uint32_t dx = 0U; dx < 2U; ++dx) {
                uint32_t const candidate_index = index.at(cellKey(
                    cell_at(wrap(static_cast<int64_t>(px) - dx), wrap(static_cast<int64_t>(py) - dy)).region
                ));
                if (cells[candidate_index].extent > cells[owner_index].extent) {
                    owner_index = candidate_index;
                }
            }
        }
        Cell const& owner = cells[owner_index];
        uint32_t const ox = owner.region.x * owner.extent;
        uint32_t const oy = owner.region.y * owner.extent;
        uint32_t const dx = wrap(static_cast<int64_t>(px) - ox);
        uint32_t const dy = wrap(static_cast<int64_t>(py) - oy);
        std::array<int32_t, 3> height{};
        if ((dx == 0U || dx == owner.extent) && (dy == 0U || dy == owner.extent)) {
            uint32_t const corner = (dy == 0U ? 0U : 2U) + (dx == 0U ? 0U : 1U);
            height = {owner.corner_heights[corner], owner.corner_temperatures[corner],
                static_cast<int32_t>(owner.corner_materials[corner])};
        } else {
            bool const vertical = dx == 0U || dx == owner.extent;
            uint32_t const ax = vertical ? px : ox;
            uint32_t const ay = vertical ? oy : py;
            uint32_t const bx = vertical ? px : ox + owner.extent;
            uint32_t const by = vertical ? oy + owner.extent : py;
            auto const a = corner_height(ax, ay);
            auto const b = corner_height(bx, by);
            double const fraction = static_cast<double>(vertical ? dy : dx) / static_cast<double>(owner.extent);
            for (uint32_t component = 0U; component < 2U; ++component) {
                height[component] =
                    static_cast<int32_t>(std::lround(a[component] + (b[component] - a[component]) * fraction));
            }
            height[2] = fraction < 0.5 ? a[2] : (fraction > 0.5 ? b[2] : std::min(a[2], b[2]));
        }
        shared_heights.emplace(key, height);
        return height;
    };

    auto describe = [](Cell const& c) {
        LocalCell d{static_cast<uint32_t>(c.region.level), c.region.x, c.region.y, c.extent,
            c.corner_heights, c.corner_temperatures, {}};
        for (size_t i = 0U; i < 4U; ++i) {
            d.materials[i] = static_cast<int32_t>(c.corner_materials[i]);
        }
        return d;
    };
    auto order = [](auto const& a, auto const& b) { return std::tie(a.level,a.x,a.y) < std::tie(b.level,b.x,b.y); };
    auto layout = std::make_shared<TerrainLodLocalLayout>();
    layout->forcing = std::move(forcing);
    layout->field = atlas;
    layout->root = root;
    layout->center_x = center_x;
    layout->center_y = center_y;
    layout->visual = visual_center;
    if (reuse_selection) {
        layout->samples = cached_layout->samples;
    } else {
        auto prepared = std::make_shared<TerrainLodLocalLayout::Samples>();
        prepared->cells.reserve(cells.size());
        for (auto const& c : cells) {
            prepared->cells.push_back(describe(c));
        }
        std::ranges::sort(prepared->cells, order);
        prepared->traversal_order.reserve(cells.size());
        prepared->center_materials.reserve(cells.size());
        prepared->root_subtree_offsets = root_subtree_offsets;
        for (auto const& value : prepared->cells) {
            uint32_t const ordinal = index.at(cellKey(
                {static_cast<shared::TerrainLodLevel>(value.level), value.x, value.y}));
            prepared->traversal_order.push_back(ordinal);
            prepared->center_materials.push_back(cells[ordinal].material);
        }
        // Preserve the physical-layout identity used by regional compatibility.
        layout->samples = cached_layout && cached_layout->samples
                && cached_layout->samples->cells == prepared->cells
                && cached_layout->samples->traversal_order == prepared->traversal_order
                && cached_layout->samples->center_materials == prepared->center_materials
                && cached_layout->samples->root_subtree_offsets == prepared->root_subtree_offsets
            ? cached_layout->samples : std::move(prepared);
    }
    scene->local_layout = std::move(layout);
    if (local != nullptr) {
        if (previous == nullptr || !cached_layout || cached_layout->samples != scene->local_layout->samples) {
            throw LocalUnsupported{"layout_changed"};
        }
        auto region = [&](std::span<TerrainLodColumnCoordinate const> targets) {
            TerrainLodLocalUnit descriptor;
            descriptor.identity = scene->identity;
            descriptor.field = atlas;
            descriptor.targets.assign(targets.begin(), targets.end());
            descriptor.root_extent = scene->root_extent;
            std::map<uint64_t, Cell const*> read_cells;
            bool all_l2 = true;
            std::vector<TerrainLodColumnCoordinate> write_cells;
            std::set<std::pair<uint32_t, uint32_t>> write_columns;
            std::vector<Cell const*> affected_cells;
            auto const& unit_actual_keys = actual_keys;
            auto const& unit_transitions = new_transitions;
            auto const edge_classification = [&](std::unordered_set<uint64_t> const& actual_set,
                std::unordered_set<uint64_t> const& transition_set, uint32_t const x, uint32_t const y) {
                uint32_t const wrapped_x = wrap(x), wrapped_y = wrap(y);
                return actual_set.contains(column_key(wrapped_x / 16U, wrapped_y / 16U))
                    || transition_set.contains(cellKey(cell_at(wrapped_x, wrapped_y).region));
            };
            auto const append_write_footprint = [&](Cell const& cell) {
                uint32_t const x = cell.region.x * cell.extent, y = cell.region.y * cell.extent;
                for (uint32_t cy = y / 16U; cy <= (y + cell.extent - 1U) / 16U; ++cy) {
                    for (uint32_t cx = x / 16U; cx <= (x + cell.extent - 1U) / 16U; ++cx) {
                        write_columns.emplace(cx % (PERIOD / 16U), cy % (PERIOD / 16U));
                    }
                }
            };
            auto const append_corner = [&](uint32_t const px, uint32_t const py) {
                Cell const* owner = &cell_at(px, py);
                for (int32_t oy = -1; oy <= 0; ++oy) {
                    for (int32_t ox = -1; ox <= 0; ++ox) {
                        uint32_t const nx = wrap(static_cast<int64_t>(px) + ox);
                        uint32_t const ny = wrap(static_cast<int64_t>(py) + oy);
                        Cell const& neighbor = cell_at(nx, ny);
                        read_cells.emplace(cellKey(neighbor.region), &neighbor);
                        if (neighbor.extent > owner->extent) {
                            owner = &neighbor;
                        }
                    }
                }
                descriptor.corners.push_back({px, py, static_cast<uint32_t>(owner->region.level),
                    owner->region.x, owner->region.y, corner_height(px, py)});
            };
            auto const append_corners = [&](Cell const& cell, uint32_t const x, uint32_t const y) {
                for (uint32_t dy : {0U, cell.extent}) {
                    for (uint32_t dx : {0U, cell.extent}) {
                        uint32_t const px = wrap(static_cast<int64_t>(x) + dx);
                        uint32_t const py = wrap(static_cast<int64_t>(y) + dy);
                        append_corner(px, py);
                    }
                }
            };
            auto const append_changed_neighbor = [&](uint32_t const nx, uint32_t const ny) {
                if (edge_classification(old_actual_keys, old_transitions.cells, nx, ny)
                    == edge_classification(unit_actual_keys, unit_transitions.cells, nx, ny)) {
                    return;
                }
                Cell const& neighbor = cell_at(nx, ny);
                uint64_t const key = cellKey(neighbor.region);
                read_cells.emplace(key, &neighbor);
                uint32_t const ox = neighbor.region.x * neighbor.extent;
                uint32_t const oy = neighbor.region.y * neighbor.extent;
                append_write_footprint(neighbor);
                if (std::ranges::equal(targets, local->targets)) {
                    local->dirty_groups.insert((static_cast<uint64_t>(ox) << 32U) | oy);
                }
            };
            for (auto const& c : cells) {
                uint32_t const x = c.region.x * c.extent, y = c.region.y * c.extent;
                bool dirty = localTouches(targets, x, y, c.extent);
                uint64_t const region_key = cellKey(c.region);
                if (local != nullptr && local->transient_recenter) {
                    dirty = dirty || changed_mask_cells.contains(region_key);
                    dirty = dirty || (old_transitions.cells.contains(region_key)
                        != unit_transitions.cells.contains(region_key));
                }
                if (!dirty) {
                    continue;
                }
                affected_cells.push_back(&c);
                if (!local->transient_recenter) {
                    all_l2 = all_l2 && c.region.level == shared::TerrainLodLevel::L2;
                    using Offset = std::pair<int32_t, int32_t>;
                    auto const is_l2 = [&](Offset const& offset) {
                        auto const& adjacent = cell_at(wrap(int64_t(x) + offset.first), wrap(int64_t(y) + offset.second));
                        return adjacent.region.level == shared::TerrainLodLevel::L2;
                    };
                    for (int32_t along : {0, 8}) {
                        for (auto o : {Offset{-1, along}, Offset{16, along}, Offset{along, -1}, Offset{along, 16}}) {
                            all_l2 = all_l2 && is_l2(o);
                        }
                    }
                    write_cells.push_back({x / 16U, y / 16U});
                } else {
                    write_columns.emplace(x / 16U, y / 16U);
                    append_write_footprint(c);
                    if (std::ranges::equal(targets, local->targets)) {
                        local->dirty_groups.insert((static_cast<uint64_t>(x) << 32U) | y);
                    }
                }
                read_cells.emplace(cellKey(c.region), &c);
                append_corners(c, x, y);
            }

            if (local != nullptr && local->transient_recenter) {
                for (Cell const* cell : affected_cells) {
                    uint32_t const x = cell->region.x * cell->extent;
                    uint32_t const y = cell->region.y * cell->extent;
                    for (uint32_t edge = 0U; edge < 4U; ++edge) {
                        for (uint32_t along = 0U; along < cell->extent; ++along) {
                            int64_t const dx = edge == 0U ? -1 : edge == 1U ? int64_t(cell->extent) : int64_t(along);
                            int64_t const dy = edge == 2U ? -1 : edge == 3U ? int64_t(cell->extent) : int64_t(along);
                            uint32_t const nx = wrap(static_cast<int64_t>(x) + dx);
                            uint32_t const ny = wrap(static_cast<int64_t>(y) + dy);
                            append_changed_neighbor(nx, ny);
                        }
                    }
                }
            }

            if (local->transient_recenter) {
                write_cells.clear();
                write_cells.reserve(write_columns.size());
                for (auto const& [x, y] : write_columns) {
                    write_cells.push_back({x, y});
                }
            }
            descriptor.halo = localCaptureHalo(write_cells);
            // Capture acceptance and coarse correction evaluation also read the selected cells/corner owners here.
            for (auto q : descriptor.halo) {
                for (uint32_t sy : {0U, 8U}) {
                    for (uint32_t sx : {0U, 8U}) {
                        auto const& c = cell_at(q.x * 16U + sx, q.y * 16U + sy);
                        read_cells.emplace(cellKey(c.region), &c);
                        uint32_t x = c.region.x * c.extent, y = c.region.y * c.extent;
                        append_corners(c, x, y);
                    }
                }
            }
            for (auto const& [key, cell] : read_cells) {
                static_cast<void>(key);
                descriptor.cells.push_back(describe(*cell));
            }
            std::ranges::sort(descriptor.cells, order);
            std::ranges::sort(descriptor.corners, [](auto const& a, auto const& b) {
                return std::tie(a.x, a.y, a.owner_level, a.owner_x, a.owner_y)
                    < std::tie(b.x, b.y, b.owner_level, b.owner_x, b.owner_y);
            });

            if (!local->transient_recenter && !all_l2) {
                throw LocalUnsupported{"non_l2_halo"};
            }
            return descriptor;
        };
        local->unit = std::make_shared<TerrainLodLocalUnit>(region(local->targets));
        auto& unit = *local->unit;
        unit.dependencies = localDependencies(local->base_columns, unit.halo, local->targets, previous->visual_center);
        unit.used = localDependencies(local->columns, unit.halo, {}, visual_center);
        for (auto const& expected : local->constraints) {
            auto const current = region(expected->targets);
            if (expected->identity != current.identity || expected->field != current.field
                || expected->root_extent != current.root_extent || expected->cells != current.cells
                || expected->corners != current.corners || expected->halo != current.halo) {
                throw LocalUnsupported{"constraint_layout_changed"};
            }
            auto old = expected->dependencies;
            std::erase_if(old, [&](auto const& input) { return localContains(local->targets, input.x, input.y); });
            if (old != localDependencies(local->base_columns, expected->halo, local->targets, visual_center)) {
                throw LocalUnsupported{"constraint_dependencies_changed"};
            }
        }
        // Outside the write unit, retain exactly the prepared base's mask and payload dependencies.
        std::map<std::pair<uint32_t, uint32_t>, TerrainLodColumnInput const*> outside;
        for (auto const& input : local->base_columns) {
            bool const target = localContains(local->targets, input->x, input->y);
            bool const outside_unit = local->transient_recenter ? !target
                : (!localTouches(local->targets,input->x*16U,input->y*16U,16U)
                    && (!previous->visual_center || terrainLodVisualColumn(input->x,input->y,
                        previous->visual_center->x,previous->visual_center->y)));
            if (outside_unit) {
                outside.emplace(std::pair{input->x, input->y}, input.get());
            }
        }
        for (auto const& input : local->columns) {
            bool const target = localContains(local->targets, input->x, input->y);
            bool const outside_unit = local->transient_recenter ? !target
                : (!localTouches(local->targets,input->x*16U,input->y*16U,16U)
                    && (!visual_center || terrainLodVisualColumn(input->x,input->y,visual_center->x,visual_center->y)));
            if (outside_unit) {
                auto found = outside.find({input->x,input->y});
                // Equal immutable inputs already share the full captured payload.
                if (found == outside.end() || (found->second != input.get()
                        && localCapture(*found->second) != localCapture(*input))) {
                    throw LocalUnsupported{"outside_capture_changed"};
                }
                outside.erase(found);
            }
        }
        if (!outside.empty()) {
            throw LocalUnsupported{"outside_capture_missing"};
        }
    }
    struct BoundaryPoint final {
        uint32_t x;
        uint32_t y;
        int64_t height_sum = 0;
        uint32_t count = 0U;
    };
    std::unordered_map<uint64_t, BoundaryPoint> boundary_points;
    for (auto const& [key, column] : actual) {
        static_cast<void>(key);
        for (uint32_t edge = 0U; edge < 4U; ++edge) {
            uint32_t const nx = wrap(static_cast<int64_t>(column->x * 16U) + (edge == 0U ? -1 : edge == 1U ? 16 : 0));
            uint32_t const ny = wrap(static_cast<int64_t>(column->y * 16U) + (edge == 2U ? -1 : edge == 3U ? 16 : 0));
            if (actual.contains(column_key(nx / 16U, ny / 16U))) {
                continue;
            }
            for (uint32_t along = 0U; along < 16U; ++along) {
                for (uint32_t endpoint = 0U; endpoint < 2U; ++endpoint) {
                    uint32_t const px =
                        wrap(column->x * 16U + (edge < 2U ? (edge == 1U ? 16U : 0U) : along + endpoint));
                    uint32_t const py =
                        wrap(column->y * 16U + (edge < 2U ? along + endpoint : (edge == 3U ? 16U : 0U)));
                    auto& point = boundary_points[column_key(px, py)];
                    point.x = px;
                    point.y = py;
                    point.height_sum += static_cast<int64_t>(column->edge_heights[edge][along]) * 32;
                    ++point.count;
                }
            }
        }
    }
    auto const coarse_properties = [&cell_at, &corner_height](uint32_t const px, uint32_t const py) {
        Cell const& owner = cell_at(wrap(px), wrap(py));
        uint32_t const ox = owner.region.x * owner.extent;
        uint32_t const oy = owner.region.y * owner.extent;
        double const half = owner.extent * 0.5;
        double const dx = static_cast<double>(wrap(static_cast<int64_t>(px) - ox)) - half;
        double const dy = static_cast<double>(wrap(static_cast<int64_t>(py) - oy)) - half;
        double const t = std::max(std::abs(dx), std::abs(dy)) / half;
        std::array<int32_t, 3> center{};
        for (uint32_t corner = 0U; corner < 4U; ++corner) {
            center[0] += owner.corner_heights[corner];
            center[1] += owner.corner_temperatures[corner];
        }
        center[0] /= 4;
        center[1] /= 4;
        center[2] = static_cast<int32_t>(owner.material);
        if (t == 0.0) {
            return center;
        }
        bool const vertical = std::abs(dx) > std::abs(dy);
        uint32_t const ax = vertical ? ox + (dx > 0.0 ? owner.extent : 0U) : ox;
        uint32_t const ay = vertical ? oy : oy + (dy > 0.0 ? owner.extent : 0U);
        uint32_t const bx = vertical ? ax : ox + owner.extent;
        uint32_t const by = vertical ? oy + owner.extent : ay;
        auto const a = corner_height(ax, ay);
        auto const b = corner_height(bx, by);
        double const fraction = 0.5 + (vertical ? dy : dx) / (2.0 * half * t);
        for (uint32_t component = 0U; component < 2U; ++component) {
            double const edge = a[component] + (b[component] - a[component]) * fraction;
            center[component] = static_cast<int32_t>(std::lround(center[component] + (edge - center[component]) * t));
        }
        std::array<int32_t, 3> const materials{center[2], a[2], b[2]};
        std::array<double, 3> const weights{1.0 - t, t * (1.0 - fraction), t * fraction};
        double selected_coverage = -1.0;
        for (uint32_t candidate = 0U; candidate < 3U; ++candidate) {
            double coverage = 0.0;
            for (uint32_t vertex = 0U; vertex < 3U; ++vertex) {
                if (materials[vertex] == materials[candidate]) {
                    coverage += weights[vertex];
                }
            }
            if (coverage > selected_coverage
                || (coverage == selected_coverage && materials[candidate] < center[2])) {
                center[2] = materials[candidate];
                selected_coverage = coverage;
            }
        }
        return center;
    };
    struct BoundaryCorrection final {
        uint32_t x;
        uint32_t y;
        int32_t height;
        int32_t correction;
    };
    std::unordered_map<uint64_t, std::vector<BoundaryCorrection>> boundary_buckets;
    for (auto const& [key, point] : boundary_points) {
        static_cast<void>(key);
        int32_t const height = static_cast<int32_t>(point.height_sum / point.count);
        boundary_buckets[column_key(point.x / 16U, point.y / 16U)].push_back({
            point.x,
            point.y,
            height,
            height - coarse_properties(point.x, point.y)[0],
        });
    }
    // A distant column can change hash iteration order. Keep the floating-point
    // reduction stable so it rounds identically for fresh and retained regions.
    for (auto& [key, points] : boundary_buckets) {
        static_cast<void>(key);
        std::ranges::sort(points, {}, [](BoundaryCorrection const& point) {
            return std::pair{point.x, point.y};
        });
    }
    struct TransitionSample final {
        std::array<int32_t, 3> properties;
        int32_t coarse_height;
    };
    std::unordered_map<uint64_t, TransitionSample> transition_values;
    auto const transition_properties = [&](uint32_t px, uint32_t py) {
        px = wrap(px);
        py = wrap(py);
        uint64_t const key = column_key(px, py);
        auto const cached = transition_values.find(key);
        if (cached != transition_values.end()) {
            return cached->second;
        }
        auto value = coarse_properties(px, py);
        int32_t const coarse_height = value[0];
        auto const boundary = boundary_points.find(key);
        if (boundary != boundary_points.end()) {
            value[0] = static_cast<int32_t>(boundary->second.height_sum / boundary->second.count);
        } else {
            double nearest = TRANSITION_WIDTH;
            double weighted = 0.0;
            double weight_sum = 0.0;
            for (int32_t by = -2; by <= 2; ++by) {
                for (int32_t bx = -2; bx <= 2; ++bx) {
                    uint32_t const cx = wrap(
                        static_cast<int64_t>(px / 16U) * 16 + static_cast<int64_t>(bx) * 16
                    ) / 16U;
                    uint32_t const cy = wrap(
                        static_cast<int64_t>(py / 16U) * 16 + static_cast<int64_t>(by) * 16
                    ) / 16U;
                    auto const bucket = boundary_buckets.find(column_key(cx, cy));
                    if (bucket == boundary_buckets.end()) {
                        continue;
                    }
                    for (auto const& point : bucket->second) {
                        double dx = std::abs(static_cast<double>(px) - point.x);
                        double dy = std::abs(static_cast<double>(py) - point.y);
                        dx = std::min(dx, PERIOD - dx);
                        dy = std::min(dy, PERIOD - dy);
                        double const distance = std::hypot(dx, dy);
                        if (distance >= TRANSITION_WIDTH) {
                            continue;
                        }
                        nearest = std::min(nearest, distance);
                        double const falloff = 1.0 - distance / TRANSITION_WIDTH;
                        double const weight = falloff * falloff / (1.0 + distance * distance);
                        weighted += point.correction * weight;
                        weight_sum += weight;
                    }
                }
            }
            if (weight_sum > 0.0) {
                value[0] +=
                    static_cast<int32_t>(std::lround(weighted / weight_sum * (1.0 - nearest / TRANSITION_WIDTH)));
            }
        }
        value[0] = std::clamp(value[0], static_cast<int32_t>(StoneFaceInstance::TERRAIN_HEIGHT_SCALE),
            static_cast<int32_t>(shared::WorldExtent::DEPTH * StoneFaceInstance::TERRAIN_HEIGHT_SCALE));
        TransitionSample const sample{value, coarse_height};
        transition_values.emplace(key, sample);
        return sample;
    };
    // A coarse cell can meet the band at its far edge while its origin lies outside the band.
    auto const& transition_cells = new_transitions.cells;
    bool const native_surface_materials =
        world.atlas()->configuration.model_tag == generation::TerrainModelTag::V21
        || world.atlas()->configuration.model_tag == generation::TerrainModelTag::V10;
    auto const surface_triangle = [native_surface_materials](
        std::array<std::array<int32_t, 3>, 3> const& vertices,
        std::array<int32_t, 3> const& temperatures,
        std::array<int32_t, 3> const& materials
    ) {
        if (native_surface_materials) {
            return StoneFaceInstance::fromTerrainMaterialTriangle(vertices,
                {static_cast<shared::Block>(materials[0]), static_cast<shared::Block>(materials[1]),
                    static_cast<shared::Block>(materials[2])});
        }
        return StoneFaceInstance::fromTerrainSurfaceTriangle(vertices, temperatures);
    };
    std::unordered_map<uint64_t, TerrainLodDrawGroup const*> previous_groups;
    std::unordered_set<uint64_t> previous_leaf_regions;
    if (previous != nullptr) {
        if (previous->identity != scene->identity) {
            throw std::invalid_argument("terrain LOD progressive scene selection changed");
        }
        previous_leaf_regions.reserve(previous->leaves.size());
        for (TerrainLodRegion const leaf : previous->leaves) {
            previous_leaf_regions.insert(cellKey(leaf));
        }
        for (auto const& group : previous->groups) {
            previous_groups.emplace((static_cast<uint64_t>(group.origin_x) << 32U) | group.origin_y, &group);
        }
    }
    auto const is_actual_cell = [&actual](Cell const& cell) {
        uint32_t const x = cell.region.x * cell.extent;
        uint32_t const y = cell.region.y * cell.extent;
        return cell.region.level == shared::TerrainLodLevel::L2
            && actual.contains((static_cast<uint64_t>(x / 16U) << 32U) | (y / 16U));
    };
    std::unordered_set<uint64_t> selection_rebuild_roots;
    auto const mark_root_neighbors = [&](uint32_t const rx, uint32_t const ry) {
        for (int32_t dy = -2; dy <= 2; ++dy) {
            for (int32_t dx = -2; dx <= 2; ++dx) {
                uint32_t const nx = wrap((static_cast<int64_t>(rx) + dx) * scene->root_extent) / scene->root_extent;
                uint32_t const ny = wrap((static_cast<int64_t>(ry) + dy) * scene->root_extent) / scene->root_extent;
                selection_rebuild_roots.insert((static_cast<uint64_t>(nx) << 32U) | ny);
            }
        }
    };
    if (previous != nullptr) {
        std::unordered_set<uint64_t> unchanged;
        unchanged.reserve(previous->leaves.size());
        for (auto const leaf : previous->leaves) {
            unchanged.insert(cellKey(leaf));
        }
        std::unordered_set<uint64_t> changed_roots;
        auto const mark_changed = [&](TerrainLodRegion const region) {
            uint32_t const extent = region.level == shared::TerrainLodLevel::L3 ? 16U
                : terrainLodRegionExtent(region.level, scene->root_extent);
            uint32_t const x = region.x * extent;
            uint32_t const y = region.y * extent;
            // A changed exact mask can alter an adjacent coarse cell's transition class.
            // Its full edge segmentation then affects the next ring of roots.
            for (uint32_t ry = y / scene->root_extent; ry <= (y + extent - 1U) / scene->root_extent; ++ry) {
                for (uint32_t rx = x / scene->root_extent; rx <= (x + extent - 1U) / scene->root_extent; ++rx) {
                    changed_roots.insert((static_cast<uint64_t>(rx) << 32U) | ry);
                }
            }
        };
        auto const compare = [&](TerrainLodRegion const region) {
            if (unchanged.erase(cellKey(region)) == 0U) {
                mark_changed(region);
            }
        };
        for (auto const& cell : cells) {
            if (!is_actual_cell(cell)) {
                compare(cell.region);
            }
        }
        for (auto const& [key, column] : actual) {
            static_cast<void>(key);
            compare({.level = shared::TerrainLodLevel::L3, .x = column->x, .y = column->y});
        }
        for (auto const leaf : previous->leaves) {
            if (unchanged.contains(cellKey(leaf))) {
                mark_changed(leaf);
            }
        }
        for (auto const key : changed_roots) {
            mark_root_neighbors(static_cast<uint32_t>(key >> 32U), static_cast<uint32_t>(key));
        }
    }
    std::unordered_map<uint64_t, std::vector<TerrainLodColumnCoordinate>> changed_by_origin_root;
    if (previous != nullptr && local == nullptr && !changed.empty()) {
        uint32_t const root_extent = scene->root_extent;
        uint32_t const side = PERIOD / root_extent;
        uint32_t const maximum_reach = root_extent + (promote_columns ? root_extent : 0U) + TRANSITION_WIDTH;
        uint32_t const radius = (maximum_reach + root_extent - 1U) / root_extent;
        uint32_t const neighbor_count = std::min(side, 2U * radius + 1U);
        // Group extents never exceed one root. These bins include every origin
        // within the exact distance test below, including across the world seam.
        for (auto const coordinate : changed) {
            uint32_t const rx = coordinate.x * 16U / root_extent;
            uint32_t const ry = coordinate.y * 16U / root_extent;
            for (uint32_t dy = 0U; dy < neighbor_count; ++dy) {
                for (uint32_t dx = 0U; dx < neighbor_count; ++dx) {
                    uint32_t const nx = wrap((static_cast<int64_t>(rx) + dx - radius) * root_extent) / root_extent;
                    uint32_t const ny = wrap((static_cast<int64_t>(ry) + dy - radius) * root_extent) / root_extent;
                    changed_by_origin_root[(static_cast<uint64_t>(nx) << 32U) | ny].push_back(coordinate);
                }
            }
        }
    }
    uint64_t logical_face_count = 0U;
    std::shared_ptr<OrdinaryFanInputs> current_ordinary;
    OrdinaryFanInputs const* old_ordinary = nullptr;
    char const* const scene_evidence = std::getenv("MC_RIVER_SCENE_EVIDENCE");
    if (river_constraints && atlas->configuration.use_v10_fine_terrain && scene->root_extent <= 256U
        && !riverSceneDiagnosticCensus() && (scene_evidence == nullptr || scene_evidence[0] == 0)) {
        current_ordinary = std::make_shared<OrdinaryFanInputs>();
        current_ordinary->reserve(cells.size());
        if (previous && previous->river_constraints && previous->river_constraints->atlas == atlas
            && previous->river_constraints->field == river_constraints->field) {
            old_ordinary = previous->river_constraints->ordinary_fans.get();
        }
        river_constraints->ordinary_fans = current_ordinary;
    } else if (river_constraints) {
        river_constraints->ordinary_fans.reset();
    }
    // Startup and recenter fallback can emit ordinary fans independently once their sectors are known.
    constexpr size_t WAVE_SIZE = 4096U;
    bool const prepared_startup = !previous && prepared_root == root;
    bool const recenter_fallback = previous && visual_center && previous->visual_center != visual_center
        && previous->root_artifact == root;
    uint32_t const worker_limit = prepared_startup ? 8U : 4U;
    size_t const ordinary_workers = !local && (prepared_startup || recenter_fallback) && !actual.empty()
        && current_ordinary ? std::clamp(std::thread::hardware_concurrency(), 1U, worker_limit) : 1U;
    bool const shard_ordinary = ordinary_workers > 1U;
    struct FanJob final {
        size_t group;
        std::array<double, 2> minimum, maximum, center;
        std::vector<RiverFanSector> sectors;
        river_field_candidate::RiverConstraintExport const* constraints;
        int32_t low, high;
        uint32_t offset{}, count{};
    };
    struct FanShard final {
        size_t begin, end;
        std::vector<StoneFaceInstance> faces;
        TerrainLodRiverGeometryAudit audit;
    };
    std::vector<FanJob> ordinary_jobs;
    if (shard_ordinary) {
        ordinary_jobs.reserve(WAVE_SIZE);
    }
    auto const flush_ordinary = [&] {
        if (ordinary_jobs.empty()) {
            return;
        }
        auto const emit_range = [&](size_t const begin, size_t const end, RiverFanQueryMemo& memo,
                                     std::unordered_map<uint64_t, shared::Block>& materials) {
            FanShard shard{.begin = begin, .end = end, .faces = {}, .audit = {}};
            for (size_t index = begin; index < end; ++index) {
                if (cancellation.stop_requested()) {
                    throw std::runtime_error("terrain LOD scene cancelled");
                }
                auto& job = ordinary_jobs[index];
                job.offset = faceInstanceCount(shard.faces.size());
                emitRiverFan(shard.faces, job.minimum, job.maximum, job.center, job.sectors, *job.constraints,
                    *atlas, world, materials, &memo, nullptr, combine_height_and_wet, job.low, job.high, shard.audit);
                job.count = faceInstanceCount(shard.faces.size() - job.offset);
            }
            return shard;
        };
        auto const emit_shard = [&](size_t const begin, size_t const end) {
            RiverFanQueryMemo memo;
            std::unordered_map<uint64_t, shared::Block> materials;
            return emit_range(begin, end, memo, materials);
        };
        auto const merge_shard = [&](FanShard&& shard) {
            uint32_t const start = faceInstanceCount(staged_faces.size());
            static_cast<void>(faceInstanceCount(staged_faces.size() + shard.faces.size()));
            staged_faces.insert(staged_faces.end(), shard.faces.begin(), shard.faces.end());
            for (size_t index = shard.begin; index < shard.end; ++index) {
                auto const& job = ordinary_jobs[index];
                auto& group = scene->groups[job.group];
                group.face_storage_first_instance = start + job.offset;
                group.range.instance_count = job.count;
                group.minimum_z = job.low / 32;
                group.maximum_z = (job.high + 31) / 32;
            }
            auto& audit = scene->river_geometry_audit;
            audit.maximum_xy_quantization_error_m = std::max(audit.maximum_xy_quantization_error_m,
                shard.audit.maximum_xy_quantization_error_m);
            audit.original_top_triangle_count += shard.audit.original_top_triangle_count;
            audit.generated_top_triangle_count += shard.audit.generated_top_triangle_count;
            audit.nearest_snap_top_triangle_reversal_count += shard.audit.nearest_snap_top_triangle_reversal_count;
            audit.role_aware_top_triangle_reversal_count += shard.audit.role_aware_top_triangle_reversal_count;
            audit.radial_steiner_lattice_adjustment_count += shard.audit.radial_steiner_lattice_adjustment_count;
            audit.inspected_fan_count += shard.audit.inspected_fan_count;
            audit.diagnostic_failed_fan_count += shard.audit.diagnostic_failed_fan_count;
            audit.steiner_node_reassignment_count += shard.audit.steiner_node_reassignment_count;
            audit.unconstrained_edge_flip_count += shard.audit.unconstrained_edge_flip_count;
            audit.constrained_cavity_retriangulation_count += shard.audit.constrained_cavity_retriangulation_count;
            audit.cavity_retriangulated_triangle_count += shard.audit.cavity_retriangulated_triangle_count;
            audit.cavity_output_triangle_count += shard.audit.cavity_output_triangle_count;
            audit.duplicate_renderer_node_quotient_count += shard.audit.duplicate_renderer_node_quotient_count;
            audit.top_face_count_change += shard.audit.top_face_count_change;
            audit.snapped_top_triangle_reversal_count += shard.audit.snapped_top_triangle_reversal_count;
            audit.snapped_top_triangle_collapse_count += shard.audit.snapped_top_triangle_collapse_count;
            audit.emitted_bridge_triangle_count += shard.audit.emitted_bridge_triangle_count;
        };
        size_t const count = ordinary_jobs.size();
        size_t const workers = std::min(ordinary_workers, count);
        if (prepared_startup) {
            // Keep each worker's exact-coordinate query and material caches for the wave,
            // but claim small contiguous ranges so a costly river fan cannot strand a shard.
            constexpr size_t CLAIM_SIZE = 64U;
            size_t const claim_count = (count + CLAIM_SIZE - 1U) / CLAIM_SIZE;
            std::atomic<size_t> next_claim{0U};
            std::vector<FanShard> claims(claim_count);
            auto const emit_claims = [&] {
                RiverFanQueryMemo memo;
                std::unordered_map<uint64_t, shared::Block> materials;
                for (;;) {
                    size_t const claim = next_claim.fetch_add(1U, std::memory_order_relaxed);
                    if (claim >= claim_count) {
                        break;
                    }
                    size_t const begin = claim * CLAIM_SIZE;
                    size_t const end = std::min(begin + CLAIM_SIZE, count);
                    claims[claim] = emit_range(begin, end, memo, materials);
                }
            };
            std::vector<std::future<void>> futures;
            futures.reserve(workers - 1U);
            for (size_t worker = 1U; worker < workers; ++worker) {
                futures.push_back(std::async(std::launch::async | std::launch::deferred, emit_claims));
            }
            emit_claims();
            for (auto& future : futures) {
                future.get();
            }
            // Claim completion order cannot change group offsets, face order or audit aggregation.
            for (auto& shard : claims) {
                merge_shard(std::move(shard));
            }
        } else {
            std::vector<std::future<FanShard>> futures;
            futures.reserve(workers - 1U);
            for (size_t worker = 1U; worker < workers; ++worker) {
                futures.push_back(std::async(std::launch::async | std::launch::deferred, emit_shard,
                    count * worker / workers, count * (worker + 1U) / workers));
            }
            merge_shard(emit_shard(0U, count / workers));
            for (auto& future : futures) {
                merge_shard(future.get());
            }
        }
        ordinary_jobs.clear();
    };
    auto const retain_ordinary = [&](uint64_t const key, TerrainLodDrawGroup const& group) {
        if (current_ordinary && old_ordinary) {
            if (auto const found = old_ordinary->find(key); found != old_ordinary->end()
                && group.face_storage && found->second.faces.lock() == group.face_storage) {
                current_ordinary->emplace(key, found->second);
            }
        }
    };
    auto const reuse_group = [&](TerrainLodRegion const region, uint32_t const x, uint32_t const y,
        uint32_t const extent) {
        // A declared local unit can repair an edit without changing the actual mask.
        // Its coarse groups must be rebuilt even when ordinary selection reuse is valid.
        if (local != nullptr && (localTouches(local->targets, x, y, extent)
                || (local->transient_recenter
                    && local->dirty_groups.contains((static_cast<uint64_t>(x) << 32U) | y)))) {
            return false;
        }
        if (local != nullptr && !localTouches(local->targets, x, y, extent)) {
            if (!previous_leaf_regions.contains(cellKey(region))) {
                return false;
            }
            auto found = previous_groups.find((static_cast<uint64_t>(x) << 32U) | y);
            if (found == previous_groups.end() || found->second->extent != extent) {
                throw LocalUnsupported{"retained_group_missing"};
            }
            auto group = *found->second;
            group.range.first_instance = appendLogicalFaceRange(logical_face_count, group.range.instance_count);
            scene->groups.push_back(std::move(group));
            retain_ordinary(column_key(x, y), *found->second);
            return true;
        }
        if (selection_rebuild_roots.contains((static_cast<uint64_t>(x / scene->root_extent) << 32U)
                | (y / scene->root_extent))) {
            return false;
        }
        if (!previous_leaf_regions.contains(cellKey(region))) {
            return false;
        }
        auto const found = previous_groups.find((static_cast<uint64_t>(x) << 32U) | y);
        if (found == previous_groups.end() || found->second->extent != extent) {
            return false;
        }
        auto const candidates = changed_by_origin_root.find(
            (static_cast<uint64_t>(x / scene->root_extent) << 32U) | (y / scene->root_extent));
        auto const nearby = candidates == changed_by_origin_root.end()
            ? std::span<TerrainLodColumnCoordinate const>{}
            : std::span<TerrainLodColumnCoordinate const>{candidates->second};
        for (auto const coordinate : nearby) {
            int64_t const dx = std::abs(static_cast<int64_t>(x) - coordinate.x * 16LL);
            int64_t const dy = std::abs(static_cast<int64_t>(y) - coordinate.y * 16LL);
            int64_t const wx = std::min(dx, static_cast<int64_t>(PERIOD) - dx);
            int64_t const wy = std::min(dy, static_cast<int64_t>(PERIOD) - dy);
            // Promotion can split an entire L0 ancestor, not just the exact
            // footprint. Its neighboring cells share boundary segmentation
            // and corner ownership. Include that ancestor extent as well as
            // the transition band before reusing any old coarse geometry.
            int64_t const affected_extent = static_cast<int64_t>(extent)
                + (promote_columns ? scene->root_extent : 0U) + TRANSITION_WIDTH;
            if (wx <= affected_extent && wy <= affected_extent) {
                return false;
            }
        }
        auto group = *found->second;
        group.range.first_instance = appendLogicalFaceRange(logical_face_count, group.range.instance_count);
        scene->groups.push_back(group);
        retain_ordinary(column_key(x, y), *found->second);
        return true;
    };
    auto const emit_transition_tile = [&](uint32_t const tx, uint32_t const ty, int32_t& minimum, int32_t& maximum) {
        auto const center_sample = transition_properties(tx + 1U, ty + 1U);
        auto const& cp = center_sample.properties;
        std::array<int32_t, 3> const center{
            static_cast<int32_t>(tx + 1U),
            static_cast<int32_t>(ty + 1U),
            cp[0],
        };
        std::array<std::array<int32_t, 3>, 8> starts{};
        std::array<std::array<int32_t, 3>, 8> ends{};
        std::array<int32_t, 8> ta{};
        std::array<int32_t, 8> tb{};
        std::array<int32_t, 8> ma{};
        std::array<int32_t, 8> mb{};
        std::vector<RiverFanSector> river_sectors;
        for (uint32_t segment = 0U; segment < 8U; ++segment) {
            uint32_t const edge = segment / 2U;
            uint32_t const along = segment % 2U;
            uint32_t const ax = edge == 0U ? tx + along
                : edge == 1U               ? tx + 2U
                : edge == 2U               ? tx + 2U - along
                : tx;
            uint32_t const ay = edge == 0U ? ty
                : edge == 1U               ? ty + along
                : edge == 2U               ? ty + 2U
                : ty + 2U - along;
            uint32_t const bx = ax + (edge == 0U ? 1U : 0U) - (edge == 2U ? 1U : 0U);
            uint32_t const by = ay + (edge == 1U ? 1U : 0U) - (edge == 3U ? 1U : 0U);
            auto a_sample = transition_properties(ax, ay);
            auto b_sample = transition_properties(bx, by);
            auto& ap = a_sample.properties;
            auto& bp = b_sample.properties;
            uint32_t const nx = wrap(static_cast<int64_t>(std::min(ax, bx)) + (edge == 3U ? -1 : 0));
            uint32_t const ny = wrap(static_cast<int64_t>(std::min(ay, by)) + (edge == 0U ? -1 : 0));
            auto const neighbor = actual.find(column_key(nx / 16U, ny / 16U));
            if (neighbor != actual.end()) {
                static constexpr std::array<uint32_t, 4> ACTUAL_EDGE{3U, 0U, 2U, 1U};
                uint32_t const offset = (edge % 2U == 0U ? nx : ny) % 16U;
                uint32_t const actual_edge = ACTUAL_EDGE[edge];
                ap[0] = neighbor->second->edge_heights[actual_edge][offset] * 32;
                bp[0] = ap[0];
                shared::Block const material = neighbor->second->edge_materials[actual_edge][offset];
                if (river_constraints && material != shared::Block::Air) {
                    ap[2] = static_cast<int32_t>(material);
                    bp[2] = ap[2];
                }
            }
            if (river_constraints) {
                bool const authoritative = neighbor != actual.end();
                double const ca = authoritative ? 0.0 : (ap[0] - a_sample.coarse_height) / 32.0;
                double const cb = authoritative ? 0.0 : (bp[0] - b_sample.coarse_height) / 32.0;
                river_sectors.push_back({{double(ax), double(ay)}, {double(bx), double(by)},
                    {(cp[0] - center_sample.coarse_height) / 32.0, ca, cb},
                    {static_cast<shared::Block>(cp[2]), static_cast<shared::Block>(ap[2]),
                        static_cast<shared::Block>(bp[2])},
                    authoritative ? std::optional<double>{ap[0] / 32.0} : std::nullopt});
            }
            starts[segment] = {static_cast<int32_t>(ax), static_cast<int32_t>(ay), ap[0]};
            ends[segment] = {static_cast<int32_t>(bx), static_cast<int32_t>(by), bp[0]};
            ta[segment] = ap[1];
            tb[segment] = bp[1];
            ma[segment] = ap[2];
            mb[segment] = bp[2];
            minimum = std::min({minimum, cp[0], ap[0], bp[0]});
            maximum = std::max({maximum, cp[0], ap[0], bp[0]});
        }
        if (river_constraints) {
            emit_fan({double(tx), double(ty)}, {double(tx + 2U), double(ty + 2U)},
                {double(tx + 1U), double(ty + 1U)}, river_sectors, tx, ty, minimum, maximum);
            return;
        }
        for (uint32_t segment = 0U; segment < 8U; ++segment) {
            staged_faces.push_back(surface_triangle(
                {center, starts[segment], ends[segment]}, {cp[1], ta[segment], tb[segment]},
                {cp[2], ma[segment], mb[segment]}));
            uint32_t const next = (segment + 1U) % 8U;
            if (ends[segment][2] != starts[next][2]) {
                staged_faces.push_back(surface_triangle(
                    {center, ends[segment], starts[next]}, {cp[1], tb[segment], ta[next]},
                    {cp[2], mb[segment], ma[next]}));
                staged_faces.push_back(surface_triangle(
                    {center, starts[next], ends[segment]}, {cp[1], ta[next], tb[segment]},
                    {cp[2], ma[next], mb[segment]}));
            }
        }
    };
    staged_faces.reserve(cells.size() * 3U);
    scene->groups.reserve(cells.size());
    scene->leaves.reserve(cells.size());
    for (Cell const& cell : cells) {
        if (cancellation.stop_requested()) {
            throw std::runtime_error("terrain LOD scene cancelled");
        }
        uint32_t const x = cell.region.x * cell.extent;
        uint32_t const y = cell.region.y * cell.extent;
        bool const actual_ready = is_actual_cell(cell);
        if (actual_ready) {
            continue;
        }
        if (reuse_group(cell.region, x, y, cell.extent)) {
            scene->leaves.push_back(cell.region);
            continue;
        }
        uint32_t const storage_first = faceInstanceCount(staged_faces.size());
        if (transition_cells.contains(cellKey(cell.region))) {
            int32_t minimum = std::numeric_limits<int32_t>::max();
            int32_t maximum = 0;
            for (uint32_t ty = y; ty < y + cell.extent; ty += 2U) {
                for (uint32_t tx = x; tx < x + cell.extent; tx += 2U) {
                    emit_transition_tile(tx, ty, minimum, maximum);
                }
            }
            uint32_t const instance_count = faceInstanceCount(staged_faces.size() - storage_first);
            uint32_t const first = appendLogicalFaceRange(logical_face_count, instance_count);
            scene->groups.push_back({
                .range = {.first_instance = first, .instance_count = instance_count},
                .face_storage_first_instance = storage_first,
                .origin_x = x,
                .origin_y = y,
                .extent = cell.extent,
                .minimum_z = minimum / 32,
                .maximum_z = (maximum + 31) / 32,
            });
            scene->leaves.push_back(cell.region);
            continue;
        }
        std::array<int32_t, 3> const center{
            static_cast<int32_t>(x + cell.extent / 2U),
            static_cast<int32_t>(y + cell.extent / 2U),
            (cell.corner_heights[0] + cell.corner_heights[1] + cell.corner_heights[2] + cell.corner_heights[3]) / 4,
        };
        std::vector<RiverFanSector> river_sectors;
        int32_t minimum_height = center[2];
        int32_t maximum_height = center[2];
        for (uint32_t edge = 0U; edge < 4U; ++edge) {
            uint32_t const along_origin = edge < 2U ? y : x;
            uint32_t offset = 0U;
            while (offset < cell.extent) {
                uint32_t const across = edge < 2U
                    ? wrap(static_cast<int64_t>(x) + (edge == 0U ? -1 : static_cast<int64_t>(cell.extent)))
                    : wrap(static_cast<int64_t>(y) + (edge == 2U ? -1 : static_cast<int64_t>(cell.extent)));
                Cell const& neighbor =
                    edge < 2U ? cell_at(across, along_origin + offset) : cell_at(along_origin + offset, across);
                uint32_t const neighbor_origin =
                    edge < 2U ? neighbor.region.y * neighbor.extent : neighbor.region.x * neighbor.extent;
                uint32_t length =
                    std::min(cell.extent - offset, neighbor_origin + neighbor.extent - (along_origin + offset));
                uint32_t const nx = edge < 2U ? across : along_origin + offset;
                uint32_t const ny = edge < 2U ? along_origin + offset : across;
                auto const actual_neighbor = actual.find((static_cast<uint64_t>(nx / 16U) << 32U) | (ny / 16U));
                if (actual_neighbor != actual.end() || transition_cells.contains(cellKey(neighbor.region))) {
                    length = 1U;
                }
                uint32_t const ex = edge < 2U ? x + (edge == 1U ? cell.extent : 0U) : along_origin + offset;
                uint32_t const ey = edge < 2U ? along_origin + offset : y + (edge == 3U ? cell.extent : 0U);
                std::array<int32_t, 3> a{
                    static_cast<int32_t>(ex),
                    static_cast<int32_t>(ey),
                    corner_height(ex, ey)[0],
                };
                std::array<int32_t, 3> b{
                    static_cast<int32_t>(ex + (edge < 2U ? 0U : length)),
                    static_cast<int32_t>(ey + (edge < 2U ? length : 0U)),
                    corner_height(ex + (edge < 2U ? 0U : length), ey + (edge < 2U ? length : 0U))[0],
                };
                minimum_height = std::min({minimum_height, a[2], b[2]});
                maximum_height = std::max({maximum_height, a[2], b[2]});
                int32_t temperature_a = corner_height(ex, ey)[1];
                int32_t temperature_b =
                    corner_height(ex + (edge < 2U ? 0U : length), ey + (edge < 2U ? length : 0U))[1];
                int32_t material_a = corner_height(ex, ey)[2];
                int32_t material_b =
                    corner_height(ex + (edge < 2U ? 0U : length), ey + (edge < 2U ? length : 0U))[2];
                if (edge == 0U || edge == 3U) {
                    std::swap(material_a, material_b);
                    std::swap(a, b);
                    std::swap(temperature_a, temperature_b);
                }
                int32_t const center_temperature = (cell.corner_temperatures[0] + cell.corner_temperatures[1] +
                            cell.corner_temperatures[2] + cell.corner_temperatures[3]) /
                    4;
                if (river_constraints) {
                    river_sectors.push_back({{double(a[0]), double(a[1])}, {double(b[0]), double(b[1])}, {},
                        {cell.material, static_cast<shared::Block>(material_a),
                            static_cast<shared::Block>(material_b)}, std::nullopt});
                } else {
                    staged_faces.push_back(
                    surface_triangle(
                        {center, a, b}, {center_temperature, temperature_a, temperature_b},
                        {static_cast<int32_t>(cell.material), material_a, material_b}));
                }
                offset += length;
            }
        }
        // Ordinary fans have integer endpoints, zero corrections and no authority edge.
        // Compare their ordered local endpoints and material inputs before reusing geometry.
        if (current_ordinary) {
            uint64_t const key = column_key(x, y);
            OrdinaryFanInput input{minimum_height, maximum_height, cell.material, {}};
            input.edges.reserve(river_sectors.size());
            for (auto const& sector : river_sectors) {
                input.edges.push_back({{static_cast<uint16_t>(sector.a[0] - x),
                    static_cast<uint16_t>(sector.a[1] - y), static_cast<uint16_t>(sector.b[0] - x),
                    static_cast<uint16_t>(sector.b[1] - y)}, {sector.materials[1], sector.materials[2]}});
            }
            if (old_ordinary && previous_leaf_regions.contains(cellKey(cell.region))) {
                auto const old = old_ordinary->find(key);
                auto const group = previous_groups.find(key);
                if (old != old_ordinary->end() && group != previous_groups.end()
                    && group->second->extent == cell.extent && group->second->face_storage
                    && old->second.faces.lock() == group->second->face_storage && *old->second.input == input) {
                    auto retained = *group->second;
                    retained.range.first_instance = appendLogicalFaceRange(logical_face_count,
                        retained.range.instance_count);
                    scene->groups.push_back(std::move(retained));
                    scene->leaves.push_back(cell.region);
                    current_ordinary->emplace(key, old->second);
                    continue;
                }
            }
            current_ordinary->emplace(key, OrdinaryFanEntry{
                .input = std::make_shared<OrdinaryFanInput const>(std::move(input)), .faces = {}});
        }
        if (shard_ordinary) {
            ordinary_jobs.push_back(FanJob{
                .group = scene->groups.size(),
                .minimum = {double(x), double(y)},
                .maximum = {double(x + cell.extent), double(y + cell.extent)},
                .center = {double(center[0]), double(center[1])},
                .sectors = std::move(river_sectors),
                .constraints = &river_region(x, y),
                .low = minimum_height,
                .high = maximum_height,
            });
        } else if (river_constraints) {
            emit_fan({double(x), double(y)}, {double(x + cell.extent), double(y + cell.extent)},
                {double(center[0]), double(center[1])}, river_sectors, x, y, minimum_height, maximum_height);
        }
        uint32_t const instance_count = faceInstanceCount(staged_faces.size() - storage_first);
        uint32_t const first = appendLogicalFaceRange(logical_face_count, instance_count);
        scene->groups.push_back({
            .range = {.first_instance = first, .instance_count = instance_count},
            .face_storage_first_instance = storage_first,
            .origin_x = x,
            .origin_y = y,
            .extent = cell.extent,
            .minimum_z = minimum_height / 32,
            .maximum_z = (maximum_height + 31) / 32,
        });
        scene->leaves.push_back(cell.region);
        if (ordinary_jobs.size() == WAVE_SIZE) {
            flush_ordinary();
        }
    }
    if (shard_ordinary) {
        flush_ordinary();
        logical_face_count = 0U;
        for (auto& group : scene->groups) {
            group.range.first_instance = appendLogicalFaceRange(logical_face_count, group.range.instance_count);
        }
    }
    for (auto const& [key, column_pointer] : actual) {
        static_cast<void>(key);
        TerrainLodActualColumn const& column = *column_pointer;
        bool const changed_column = std::ranges::find(changed, TerrainLodColumnCoordinate{column.x,
            column.y}) != changed.end();
        if (previous != nullptr && !changed_column
            && previous_leaf_regions.contains(cellKey({.level = shared::TerrainLodLevel::L3,
                .x = column.x, .y = column.y}))) {
            // Exact neighbors already captured their one-cell halo. Their
            // immutable geometry remains valid while the coarse rim changes.
            auto const found = previous_groups.find((static_cast<uint64_t>(column.x * 16U) << 32U) | (column.y * 16U));
            if (found == previous_groups.end() || found->second->extent != 16U) {
                throw std::invalid_argument("terrain LOD progressive exact group is absent");
            }
            auto group = *found->second;
            group.range.first_instance = appendLogicalFaceRange(logical_face_count, group.range.instance_count);
            scene->groups.push_back(group);
            scene->leaves.push_back({.level = shared::TerrainLodLevel::L3, .x = column.x, .y = column.y});
            continue;
        }
        uint32_t const storage_first = faceInstanceCount(staged_faces.size());
        staged_faces.insert(staged_faces.end(), column.faces.begin(), column.faces.end());
        for (shared::ChunkMesh const& mesh : column.meshes) {
            for (shared::MeshFace const& face : mesh.faces) {
                staged_faces.push_back(StoneFaceInstance::fromChunkFace(mesh.coordinate, face));
            }
        }
        uint32_t const instance_count = faceInstanceCount(staged_faces.size() - storage_first);
        uint32_t const first = appendLogicalFaceRange(logical_face_count, instance_count);
        scene->groups.push_back({
            .range = {.first_instance = first, .instance_count = instance_count},
            .face_storage_first_instance = storage_first,
            .origin_x = column.x * 16U,
            .origin_y = column.y * 16U,
            .extent = 16U,
            .minimum_z = 0,
            .maximum_z = static_cast<int32_t>(shared::WorldExtent::DEPTH),
        });
        scene->leaves.push_back({.level = shared::TerrainLodLevel::L3, .x = column.x, .y = column.y});
    }
    scene->logical_face_count = static_cast<size_t>(logical_face_count);
    scene->actual_column_count = static_cast<uint32_t>(actual.size());
    if (riverSceneDiagnosticCensus()) {
        if (char const* const evidence = std::getenv("MC_RIVER_SCENE_EVIDENCE")) {
            auto const& audit = scene->river_geometry_audit;
            std::ofstream summary(std::string(evidence) + ".census-summary.json");
            summary << "{\"complete\":true,\"published_scene\":false,\"inspected_fans\":" << audit.inspected_fan_count
                << ",\"failed_fans\":" << audit.diagnostic_failed_fan_count
                << ",\"nearest_reversals\":" << audit.nearest_snap_top_triangle_reversal_count
                << ",\"role_aware_reversals\":" << audit.role_aware_top_triangle_reversal_count
                << ",\"radial_adjustments\":" << audit.radial_steiner_lattice_adjustment_count << "}\n";
        }
        throw std::runtime_error("diagnostic river fan census complete; no scene published; unresolved fans "
            + std::to_string(scene->river_geometry_audit.diagnostic_failed_fan_count));
    }
    auto const groups_built = terrainLodPhaseTime(trace_phases);
    // Consecutive urgent scenes overlap. Keep only the small, immutable set
    // used by this build so retaining a scene cannot retain a chain of caches.
    if (retain_queries && query_memo && query_memo->canRetain()) {
        river_constraints->query_memo = std::make_shared<RiverFanQueryMemo const>(std::move(*query_memo));
    }
    prepareTerrainLodDrawIndex(*scene);
    finalizeTerrainLodGeometry(*scene, std::move(staged_faces), cancellation);
    if (current_ordinary) {
        // Local upload merges may replace a group at the same origin. Reuse
        // only when the input record still describes the actual payload.
        for (auto const& group : scene->groups) {
            auto const found = current_ordinary->find(column_key(group.origin_x, group.origin_y));
            if (found != current_ordinary->end()) {
                found->second.faces = group.face_storage;
            }
        }
    }
    if (trace_phases) {
        auto const finished = std::chrono::steady_clock::now();
        auto const ns = [](auto const duration) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        };
        CORE_INFO(
            "stream_trace event=scene_build_phases ns={} scene={} base_scene={} setup_ns={} selection_ns={} "
            "groups_ns={} finalize_ns={} reused_roots={} rebuilt_roots={} ordinary_workers={}",
            ns(finished.time_since_epoch()), reinterpret_cast<uintptr_t>(scene.get()),
            reinterpret_cast<uintptr_t>(previous), ns(selection_started - started),
            ns(selected - selection_started), ns(groups_built - selected), ns(finished - groups_built),
            scene->selection_audit.reused_l0_root_count, scene->selection_audit.rebuilt_l0_root_count, ordinary_workers);
    }
    return scene;
}


} // namespace

std::shared_ptr<TerrainLodScene const> buildTerrainLodSceneFromColumns(
    shared::TerrainWorld const& world, uint32_t center_x, uint32_t center_y,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns, std::stop_token const& cancellation,
    TerrainLodScene const* previous, std::span<std::shared_ptr<TerrainLodColumnInput const> const> unchanged,
    bool promote_columns, std::optional<TerrainLodColumnCoordinate> visual_center,
    std::shared_ptr<shared::TerrainLodArtifact const> const& prepared_root)
{
    return buildTerrainLodSceneFromColumnsImpl(world, center_x, center_y, columns, cancellation, previous, unchanged,
        promote_columns, visual_center, nullptr, prepared_root);
}
std::shared_ptr<TerrainLodScene const> buildTerrainLodScene(
    shared::TerrainWorld const& world, uint32_t center_x, uint32_t center_y,
    std::span<TerrainLodActualColumn const> actual, std::stop_token const& cancellation,
    TerrainLodScene const* previous, std::span<TerrainLodColumnCoordinate const> changed,
    bool promote_columns, std::optional<TerrainLodColumnCoordinate> visual_center)
{
    return buildTerrainLodSceneImpl(
        world, center_x, center_y, actual, cancellation, previous, changed, promote_columns, visual_center, nullptr);
}
TerrainLodLocalPreparation prepareTerrainLodLocalScene(
    shared::TerrainWorld const& world, TerrainLodScene const& base,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> base_columns,
    std::span<std::shared_ptr<TerrainLodColumnInput const> const> columns,
    std::span<TerrainLodColumnCoordinate const> targets,
    std::span<std::shared_ptr<TerrainLodLocalUnit const> const> constraints,
    std::stop_token const& cancellation, bool const transient_recenter)
{
    if (targets.empty() || !base.local_layout || base.identity != world.identity() || base.shading_atlas != world.atlas()) {
        return {};
    }
    LocalBuild local{base_columns,columns,targets,constraints,{},transient_recenter,{}};
    std::vector<std::shared_ptr<TerrainLodColumnInput const>> unchanged;
    for (auto const& input : columns) {
        if (std::ranges::find(base_columns,input) != base_columns.end()) {
            unchanged.push_back(input);
        }
    }
    std::optional<TerrainLodColumnCoordinate> const visual_center =
        transient_recenter ? std::nullopt : base.visual_center;
    try {
        auto scene = std::const_pointer_cast<TerrainLodScene>(buildTerrainLodSceneFromColumnsImpl(
            world,base.center_x,base.center_y,columns,cancellation,&base,unchanged,true,visual_center,&local));
        for (auto target : targets) {
            if (!std::ranges::any_of(scene->leaves, [=](auto leaf) {
                    return leaf.level == shared::TerrainLodLevel::L3 && leaf.x == target.x && leaf.y == target.y;
                })) {
                return {};
            }
        }
        scene->session_join_epoch = base.session_join_epoch;
        scene->coverage_epoch = base.coverage_epoch;
        return {std::move(scene),std::move(local.unit)};
    } catch (LocalUnsupported const& error) {
        char const* const trace = std::getenv("MC_DIAGNOSTIC_STREAMING_TRACE");
        if (trace != nullptr && std::string_view(trace) == "1" && !targets.empty()) {
            std::clog << "stream_trace event=local_unsupported reason=" << error.reason
                << " base_x=" << base.center_x << " base_y=" << base.center_y
                << " target_x=" << targets.front().x << " target_y=" << targets.front().y
                << " targets=" << targets.size() << '\n';
        }
        return {};
    }
}
bool terrainLodLocalSourcesCurrent(TerrainLodLocalUnit const& unit, shared::SparseWorld const& world)
{
    TerrainLodScene source;
    source.identity = unit.identity;
    for (auto const& input : unit.used) {
        source.chunk_sources.insert(source.chunk_sources.end(),input.chunks.begin(),input.chunks.end());
        source.authority_sources.insert(source.authority_sources.end(),input.authority.begin(),input.authority.end());
        source.edit_sources.insert(source.edit_sources.end(),input.edits.begin(),input.edits.end());
    }
    return terrainLodSourcesCurrent(source,world);
}
std::span<TerrainLodColumnCoordinate const> terrainLodLocalTargets(TerrainLodLocalUnit const& unit)
{
    return unit.targets;
}

} // namespace client
