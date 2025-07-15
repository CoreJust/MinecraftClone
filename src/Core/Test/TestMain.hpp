#pragma once

#ifndef _TEST
#define TEST_MAIN()
#else
#define TEST_MAIN() core::test_main()

namespace core {
	void test_main();
} // namespace core
#endif
