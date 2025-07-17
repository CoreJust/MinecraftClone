#include <Core/Test/UnitTest.hpp>
#include "../UniquePtr.hpp"
#include "../Move.hpp"

using namespace core;

UNIT_TEST(UniquePtrCreation) {
    auto ptr = makeUP<int>(10);
    CHECK(bool(ptr));
    CHECK(*ptr == 10);
};

UNIT_TEST(UniquePtrMove) {
    auto ptr = makeUP<int>(33);
    auto newPtr = move(ptr);
    CHECK(!ptr);
    CHECK(bool(newPtr));
    CHECK(*newPtr == 33);
};
