#include "NoOpt.hpp"

#if defined(_MSC_VER)
char const volatile* g_volatilePtr;

void useVolatile(char const volatile* const ptr) noexcept {
	g_volatilePtr = ptr;
}
#endif
