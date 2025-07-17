#include <Core/Test/UnitTest.hpp>
#include "../Function.hpp"

using namespace core;

int testFunc(int x) { return x; }

UNIT_TEST(FunctionPtrTest) {
    Function<int, int> fn = testFunc;
    CHECK(fn(10) == 10);
    CHECK(fn(1) == 1);
};

UNIT_TEST(FunctionSimpleLambdaTest) {
    Function<int, int const&> fn = [](int const& x) { return x; };
    CHECK(fn(10) == 10);
    CHECK(fn(1) == 1);
};

UNIT_TEST(FunctionLambdaTest) {
    Function<int> fn = [cnt = 0]() mutable { return cnt += 2; };
    CHECK(fn() == 2);
    CHECK(fn() == 4);
};
