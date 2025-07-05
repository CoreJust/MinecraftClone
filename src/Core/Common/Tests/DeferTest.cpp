#include <Core/Test/UnitTest.hpp>
#include "../Defer.hpp"

UNIT_TEST(DeferTest) {
    int cnt = 0;
    {
        defer { cnt += 1; };
        CHECK(cnt == 0);
    }
    CHECK(cnt == 1);
};
