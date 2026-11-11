#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace generation::detail
{

using V21FinePoint = std::array<double, 2U>;

struct V21FineStanding final
{
    double stage = 0.0;
    int32_t owner = 0;
};

enum class V21FineFailureKind : uint8_t
{
    InvalidInput,
    Cancelled,
    LimitExceeded,
    CyclicGraph,
    ConflictingStandingOutlet,
    IterationScheduleNotConverged,
    ImmutableStandingOutletInfeasible,
    BankSupportInfeasible,
    ComponentOrderError,
    CallbackFailure,
};

struct V21FineWitness final
{
    int64_t edge = -1;
    int64_t node = -1;
    int32_t source_vertex = -1;
    int32_t side = -1;
    double required_stage = 0.0;
    double maximum_cut_only_stage = 0.0;
    double deficit = 0.0;
    V21FinePoint xy0{}, xy1{};
    double substrate0 = 0.0, substrate1 = 0.0;
    double depth0 = 0.0, depth1 = 0.0;
    double stage0 = 0.0, stage1 = 0.0;
    double last_chord_m = 0.0;
    double required_support_m = 0.0;
};

struct V21FineFailure final
{
    V21FineFailureKind kind = V21FineFailureKind::InvalidInput;
    std::string reason;
    std::vector<V21FineWitness> witnesses;
};

// Horizontal coordinates, halfwidths and supports use the 65536 m torus.
// All vertical values use the approved game-block metric, already scaled .34.
struct V21FineChord final
{
    V21FinePoint a{}, delta{};
    int32_t start = 0, target = 0;
    double grade0 = 0.0, grade1 = 0.0;
    double water_stage0 = 0.0, water_stage1 = 0.0;
    double halfwidth = 0.0;
    double discharge_m3_s = 0.0;
    bool shore_truncated = false;
    uint64_t global_order = 0U;
    double support_m = 0.0, support_coarse_m = 0.0;
    double bank_slope_left = 0.0, bank_slope_right = 0.0;
    double coarse_bank_slope_left = 0.0, coarse_bank_slope_right = 0.0;
    double normal_depth = 0.0, normal_slope = 0.0;
};

struct V21FineGeometry final
{
    std::string immutable_identity;
    std::vector<V21FineChord> chords;
    std::vector<int32_t> vertex_owner;
    double bank_fraction = .1;
    double repose_slope = .65;
};

struct V21FineCallbacks final
{
    using Heights = std::function<std::vector<double>(std::span<V21FinePoint const>)>;
    Heights substrate;
    Heights base;
    std::function<std::vector<V21FineStanding>(std::span<V21FinePoint const>)> standing;
    // Exact V21 protected-contact operation; supply an explicit identity
    // callback when the geometry has no contacts. No ridge floor is added here.
    std::function<std::vector<double>(std::span<double const>, std::span<V21FinePoint const>)> protect;
};

struct V21FineGrid final
{
    double origin_x_m = 0.0, origin_y_m = 0.0;
    double spacing_m = 1.0;
    uint32_t rows = 0U, columns = 0U;
};

class V21FinePlan final
{
public:
    [[nodiscard]] std::span<V21FineChord const> chords() const noexcept;
    [[nodiscard]] std::span<std::string const> connectedComponentIds() const noexcept;
    [[nodiscard]] std::string const& sourceIdentity() const noexcept;
    [[nodiscard]]
    std::expected<std::vector<double>, V21FineFailure> applyProfile(
        std::span<double const> background,
        std::span<double const> x_m,
        std::span<double const> y_m,
        uint32_t lod,
        std::stop_token cancellation = {}
    ) const;

private:
    struct Impl;
    explicit V21FinePlan(std::shared_ptr<Impl const> impl);
    std::shared_ptr<Impl const> m_impl;
    friend class V21FineProvider;
};

class V21FineProvider final
{
public:
    // Copies the immutable course/ownership input. Callbacks must remain
    // deterministic for immutable_identity and evaluate prechannel terrain.
    V21FineProvider(V21FineGeometry geometry, V21FineCallbacks callbacks);
    ~V21FineProvider();
    V21FineProvider(V21FineProvider const&) = delete;
    V21FineProvider& operator=(V21FineProvider const&) = delete;

    // Whole connected components are selected conservatively, built on a fixed
    // 64-update schedule, cached by immutable input and merged in global order.
    [[nodiscard]]
    std::expected<std::shared_ptr<V21FinePlan const>, V21FineFailure> forRegion(
        V21FineGrid const& grid,
        uint32_t lod,
        std::stop_token cancellation = {}
    ) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace generation::detail
