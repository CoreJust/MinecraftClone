#include <Core/Test/UnitTest.hpp>
#include "../Mat.hpp"

using namespace core;

UNIT_TEST(MatIdentityTest) {
    Mat4f a = Mat4f::Identity();
    CHECK(a * a == a);
};