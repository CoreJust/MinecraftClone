#pragma once
#include <Core/Meta/If.hpp>

using i8  = signed   char;
using u8  = unsigned char;
using i16 = signed   short int;
using u16 = unsigned short int;
using i32 = core::meta::If<sizeof(int) == 4,   signed int,   signed long int>;
using u32 = core::meta::If<sizeof(int) == 4, unsigned int, unsigned long int>;
using i64 = signed   long long int;
using u64 = unsigned long long int;

using isize = core::meta::If<sizeof(void*) == 8, i64, i32>;
using usize = core::meta::If<sizeof(void*) == 8, u64, u32>;
