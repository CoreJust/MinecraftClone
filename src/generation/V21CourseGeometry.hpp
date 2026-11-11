#pragma once

#include "V21FinePlan.hpp"

namespace generation::detail
{

struct V21CourseContact final
{
    V21FinePoint xy_m{};
    double radius_m = 0.0;
    int32_t vertex_index = -1;
    double height_blocks = 0.0;
};

struct V21CourseInput final
{
    // Native row-major 256-square arrays. Vertical fields are game blocks;
    // Q is unchanged m3/s and horizontal coordinates are metres on the torus.
    std::span<double const> canonical_bed;
    std::span<double const> discharge_m3_s;
    std::span<int32_t const> downstream;
    std::span<uint8_t const> river, virtual_through_lake;
    std::span<float const> reference_stage;
    // IDs where abs(canonical_bed - body.spill_m) < 1e-9 for any actual body.
    std::span<int32_t const> critical_spill_vertices;
};

struct V21CourseCallbacks final
{
    using Heights = V21FineCallbacks::Heights;
    // scene_base is V21 geometry.base: physical plain replay, no ridge floor.
    // reference_base is reference_geometry.base used by the approved course
    // cost: metric plain replay plus that reference object's watershed floors.
    Heights scene_base, reference_base;
    std::function<std::vector<V21FineStanding>(std::span<V21FinePoint const>)> standing;
};

struct V21CourseRecord final
{
    int32_t source_vertex = -1;
    double chord_m = 0.0, arc_m = 0.0, slope = 0.0, confinement = 0.0;
    double lateral_excursion_m = 0.0, minimum_radius_m = 0.0;
    double required_radius_m = 0.0, cost = 0.0;
    V21FinePoint a{}, delta{}, m0{}, m1{}, shift{};
};

struct V21CourseResult final
{
    // Source vertices ascend; each course emits successive parameter-uniform
    // chords. Lazy FinePlan further splits any actual chord longer than 2 m.
    std::vector<V21FineChord> chords;
    std::vector<V21CourseRecord> courses;
    std::vector<V21CourseContact> contacts;
};

struct V21CourseFailure final
{
    enum class Kind : uint8_t
    {
        InvalidInput,
        Cancelled,
        NoAdmissibleCourse,
        CallbackFailure
    };
    Kind kind = Kind::InvalidInput;
    int32_t source_vertex = -1;
    std::string message;
};

[[nodiscard]]
std::expected<V21CourseResult, V21CourseFailure> buildV21CourseGeometry(V21CourseInput const &input,
    V21CourseCallbacks const &callbacks,
    std::stop_token cancellation = {});

} // namespace generation::detail
