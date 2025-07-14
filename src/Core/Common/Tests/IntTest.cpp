#include "../Int.hpp"

#define CHECK_INT(sign, size) static_assert(sizeof(sign##size) == (size / 8))
CHECK_INT(i, 8);
CHECK_INT(i, 16);
CHECK_INT(i, 32);
CHECK_INT(i, 64);
CHECK_INT(u, 8);
CHECK_INT(u, 16);
CHECK_INT(u, 32);
CHECK_INT(u, 64);
#undef CHECK_INT