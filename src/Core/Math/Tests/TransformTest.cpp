#include <Core/Test/UnitTest.hpp>
#include "../Transform.hpp"

using namespace core;

UNIT_TEST(Transform3DConstantsTest) {
    Transform3f t = Transform3f::translation({ 1.f, 2.f, 3.f });
    Transform3f s = Transform3f::scale({ 1.f, 2.f, 3.f });
    Transform3f trs = t * s;
    Vec3f p1 = Vec3f::zero();
    Vec3f p2 = Vec3f::constant(1.f);
    CHECK((trs * p1).slice<3>() == Vec3f { 1.f, 2.f, 3.f });
    CHECK((trs * p2).slice<3>() == Vec3f { 2.f, 4.f, 6.f });
};
