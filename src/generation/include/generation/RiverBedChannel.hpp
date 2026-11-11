#pragma once

#include <generation/RiverBedField.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace river_field_candidate
{

// Geometry of one selected stored course. These values do not describe wetness.
struct NearestCourseGeometry final
{
    uint64_t course_id;
    uint64_t reach_index;
    double centreline_distance_m;
    double nominal_halfwidth_m;
    double reference_grade_m;
    bool within_nominal_channel;
    uint64_t source_reach_index;
    int32_t source_start_vertex;
    int32_t source_target_vertex;
    double course_fraction;
    bool receiver_extension;
    int32_t receiver_owner;
};

struct RiverChannelSample final
{
    RiverBedSample bed;
    double native_standing_stage_m;
    int32_t native_standing_owner;
    // Empty when the point's spatial-index cell has no course candidates.
    // A candidate can lie outside actual valley support.
    std::optional<NearestCourseGeometry> nearest_course;
    // Every containing nominal channel, in canonical course order. Valley
    // support alone never enters this list. Source indices precede field sorting.
    std::vector<NearestCourseGeometry> containing_courses;
};

struct RiverMapCoursePoint final
{
    WorldXY xy;
    double nominal_halfwidth_m;
};

struct RiverMapCourse final
{
    uint64_t course_id;
    uint64_t source_reach_index;
    int32_t source_start_vertex;
    int32_t source_target_vertex;
    bool receiver_extension;
    int32_t receiver_owner;
    std::vector<RiverMapCoursePoint> points;
};

// Sparse stored geometry, with no densification, bed evaluation or native callback.
[[nodiscard]]
std::vector<RiverMapCourse> exportRiverMapCourses(PreparedRiverField const& field);

// Uses the same sampled curves, periodic images, and bed evaluator as sampleRiverBed().
// At an overlap, selects the minimum centreline distance; ties select the lower
// canonical course_id. Membership is for that selected course, not a union.
// Reference grade is the selected course's interpolated graph grade; it is not
// a free surface or a claim that the altered bed is wet.
[[nodiscard]] std::expected<std::vector<RiverChannelSample>, Failure> sampleRiverChannel(
    PreparedRiverField const& field, std::span<NativePointSample const> native_samples
);

} // namespace river_field_candidate
