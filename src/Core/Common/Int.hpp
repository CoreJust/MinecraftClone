#pragma once
#include <Core/Meta/If.hpp>

using i8  = signed   char;
using u8  = unsigned char;
using i16 = signed   short int;
using u16 = unsigned short int;
using i32 = core::If<sizeof(int) == 4,   signed int,   signed long int>;
using u32 = core::If<sizeof(int) == 4, unsigned int, unsigned long int>;
using i64 = signed   long long int;
using u64 = unsigned long long int;

using isize = core::If<sizeof(void*) == 8, i64, i32>;
using usize = core::If<sizeof(void*) == 8, u64, u32>;

template<usize>
struct IntImpl { };
template<>
struct IntImpl<1> {
    using SignedType   = i8;
    using unsignedType = u8;
};
template<>
struct IntImpl<2> {
    using SignedType   = i16;
    using unsignedType = u16;
};
template<>
struct IntImpl<4> {
    using SignedType   = i32;
    using unsignedType = u32;
};
template<>
struct IntImpl<8> {
    using SignedType   = i64;
    using unsignedType = u64;
};

template<usize Bytes>
using Int = IntImpl<Bytes>::SignedType;
template<usize Bytes>
using Uint = IntImpl<Bytes>::UnsignedType;
