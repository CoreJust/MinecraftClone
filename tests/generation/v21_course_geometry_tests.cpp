#include "V21CourseGeometry.hpp"

#include <gtest/gtest.h>

namespace generation::detail
{
namespace
{
struct Inputs final
{
    std::vector<double> bed = std::vector<double>(65'536U), q = std::vector<double>(65'536U);
    std::vector<int32_t> downstream = std::vector<int32_t>(65'536U, -1);
    std::vector<uint8_t> river = std::vector<uint8_t>(65'536U), virtual_lake = std::vector<uint8_t>(65'536U);
    std::vector<float> stage = std::vector<float>(65'536U);
    V21CourseCallbacks callbacks;
    static double height(double x, double y)
    {
        return 100.0 - .01 * x + .00001 * (y - 1024.0) * (y - 1024.0);
    }
    Inputs()
    {
        for (size_t i = 0U; i < bed.size(); ++i)
        {
            bed[i] = height(double(i % 256U) * 256.0, double(i / 256U) * 256.0);
        }
        downstream[1028U] = 1029;
        downstream[1029U] = 1286;
        q[1028U] = .02;
        q[1029U] = .03;
        river[1028U] = river[1029U] = 1U;
        callbacks.scene_base = callbacks.reference_base = [](std::span<V21FinePoint const> points)
        {
            std::vector<double> values;
            values.reserve(points.size());
            for (auto p : points)
            {
                values.push_back(height(p[0], p[1]));
            }
            return values;
        };
        callbacks.standing = [](std::span<V21FinePoint const> points)
        { return std::vector<V21FineStanding>(points.size()); };
    }
    V21CourseInput view() const
    {
        return {bed, q, downstream, river, virtual_lake, stage, {}};
    }
};
} // namespace

TEST(V21CourseGeometryTest, ReplaysApprovedCurvedCourseChordWitnesses)
{
    Inputs const input;
    auto const result = buildV21CourseGeometry(input.view(), input.callbacks);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->courses.size(), 2U);
    ASSERT_EQ(result->chords.size(), 312U);
    EXPECT_EQ(result->chords[0U].start, 1028);
    EXPECT_EQ(result->chords[130U].start, 1029);
    EXPECT_DOUBLE_EQ(result->chords[61U].delta[0], 2.059'870'213'799'7407);
    EXPECT_DOUBLE_EQ(result->chords[130U].a[0], 1280.0);
    EXPECT_DOUBLE_EQ(result->chords[130U].delta[1], .424'025'472'180'80144);
    EXPECT_DOUBLE_EQ(result->chords[311U].water_stage1, 85.295'36);
    EXPECT_DOUBLE_EQ(result->chords[0U].halfwidth, 1.656'570'610'838'594);
    EXPECT_EQ(result->chords.back().global_order, 311U);
}

TEST(V21CourseGeometryTest, CancelledQueryHasExplicitFailureKind)
{
    Inputs const input;
    std::stop_source source;
    source.request_stop();
    auto const result = buildV21CourseGeometry(input.view(), input.callbacks, source.get_token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, V21CourseFailure::Kind::Cancelled);
}

TEST(V21CourseGeometryTest, MissingNativeFieldFailsBeforeCallbacks)
{
    Inputs const input;
    auto fields = input.view();
    fields.reference_stage = {};
    auto const result = buildV21CourseGeometry(fields, input.callbacks);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, V21CourseFailure::Kind::InvalidInput);
}

TEST(V21CourseGeometryTest, ReconstructsSceneContactCollarFromRawSceneGeometry)
{
    Inputs const input;
    std::array<int32_t, 1U> const vertices{1284};
    auto fields = input.view();
    fields.critical_spill_vertices = vertices;
    auto const result = buildV21CourseGeometry(fields, input.callbacks);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->contacts.size(), 1U);
    EXPECT_EQ(result->contacts[0U].vertex_index, 1284);
    EXPECT_DOUBLE_EQ(result->contacts[0U].radius_m, 64.0);
}

TEST(V21CourseGeometryTest, InvalidCallbackExtentHasExplicitFailureKind)
{
    Inputs const input;
    auto callbacks = input.callbacks;
    callbacks.reference_base = [](std::span<V21FinePoint const>) { return std::vector<double>{}; };
    auto const result = buildV21CourseGeometry(input.view(), callbacks);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, V21CourseFailure::Kind::InvalidInput);
}

} // namespace generation::detail
