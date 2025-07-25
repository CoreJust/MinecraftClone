#include <Core/Test/UnitTest.hpp>
#include <Core/Memory/Move.hpp>
#include "../DynArray.hpp"

using namespace core;

UNIT_TEST(CreationTest) {
    DynArray<int> arr(10, 1);
    CHECK(arr.size() == 10);
    for (int a : arr)
        CHECK(a == 1);
};

UNIT_TEST(AdvancedCreationTest) {
    DynArray<int> arr1(16, [cnt = 0](usize) mutable { return cnt += 2; });
    DynArray<int> arr2(16, [](int* ptr, usize i) mutable { *ptr = static_cast<int>(i); });
    CHECK(arr1.size() == 16);
    CHECK(arr2.size() == 16);
    for (usize i = 0; i < 16; ++i) {
        CHECK(arr1[i] == static_cast<int>(i * 2 + 2));
        CHECK(arr2[i] == static_cast<int>(i));
    }
};

UNIT_TEST(MoveTest) {
    DynArray<int> arr(16, 0);
    CHECK(arr.size() == 16);

    DynArray<int> c = move(arr);
    CHECK(arr.size() == 0);
    CHECK(c.size() == 16);
    CHECK(c[0] == 0);
    CHECK(c[15] == 0);
};
