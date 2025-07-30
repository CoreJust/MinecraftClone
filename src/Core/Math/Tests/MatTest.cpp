#include <Core/Test/UnitTest.hpp>
#include "../Mat.hpp"

using namespace core;

UNIT_TEST(MatConstantsTest) {
    Mat4f id = Mat4f::identity();
    Mat4f zero = Mat4f::zero();
    CHECK(id * id == id);
    CHECK(zero * zero == zero);
    CHECK(id * zero == zero);
    CHECK(zero * id == zero);
};