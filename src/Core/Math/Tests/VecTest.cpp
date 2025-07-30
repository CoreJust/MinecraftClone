#include <Core/Test/UnitTest.hpp>
#include "../Vec.hpp"

using namespace core;

UNIT_TEST(VecConstantsTest) {
    Vec4f zero4f = Vec4f::zero();
    Vec4f one4f = Vec4f::constant(1.f);
    Vec4f unitX = Vec4f::unit(0);
    CHECK(zero4f == Vec4f { 0.f, 0.f, 0.f, 0.f });
    CHECK(one4f  == Vec4f { 1.f, 1.f, 1.f, 1.f });
    CHECK(unitX  == Vec4f { 1.f, 0.f, 0.f, 0.f });
};
