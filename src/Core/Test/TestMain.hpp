#pragma once

#ifndef _TEST
#define TEST_MAIN()
#else
#define TEST_MAIN()          \
	core::test::test_main();

namespace core::test {
	void test_main();
} // namespace core::test
#endif
